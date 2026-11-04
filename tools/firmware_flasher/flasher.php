<?PHP
/***************************************************************************
 *   Copyright (C) 2026 by Rick KD0OSS                                     *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 3 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, see <http://www.gnu.org/licenses/>   *
 *                                                                         *
 ***************************************************************************/

/*
 * OpenMT firmware flasher.  Runs as root (see openmt-flasher.service) and does what the dashboard's Firmware tab
 * asks for when a person presses Flash: it takes a file of a build from modem_host.firmware_files, stops the OpenMT
 * services (they own the modem's serial port), runs openmt_flash.sh of the sample firmware, starts the services
 * that were running again and writes the log back for the page.
 *
 *     php flasher.php [--once] [--systemctl PATH] [--script PATH] [--source DIR]
 *
 * --once makes it handle the requests that are waiting and stop (tests); the others point it at other programs.
 *
 * The web server has no rights over the serial port, the GPIO pins or systemd; only this program has, and it checks
 * every request again with fwFlashPlan(): the file must be a result of a build, the method, port and pins must
 * be of the allowed kind, and the script is run directly, never through a shell, with arguments built from those
 * checked values.  A request not taken up within 2 minutes is marked "expired" and never run later.
 */

if (PHP_SAPI !== 'cli')
{
    http_response_code(403);
    exit;
}

set_time_limit(0);

const FL_LOG_LIMIT     = 60000;     /* longest log kept, in bytes */
const FL_BEAT_S        = 3;         /* seconds between heartbeats and log writes */
const FL_EXPIRE_S      = 120;       /* a request not taken up within this time is marked expired */
const FL_FLASH_LIMIT_S = 240;       /* longest time the script may take */

/*
 * flConnect()
 *
 * Opens the database with the settings from /etc/openmt.ini, the same way the dashboard does.
 *
 * Returns the connection, or false when it could not be opened.
 */
function flConnect()
{
    $host     = "localhost";
    $password = "";
    $foundDB  = false;
    $file     = fopen("/etc/openmt.ini", "r");

    if ($file != false)
    {
        while (!feof($file))
        {
            $line = fgets($file);
            if ($foundDB)
            {
                if (stristr($line, "host=") != false)
                {
                    parse_str($line, $tmp);
                    $host = trim($tmp['host']);
                }
                if (stristr($line, "passwd=") != false)
                {
                    parse_str($line, $tmp);
                    $password = trim($tmp['passwd']);
                }
            }
            if (stristr($line, "[database]") != false && !$foundDB)
                $foundDB = true;
        }
        fclose($file);
    }
    return mysqli_connect($host, "openmt", $password);
}

/*
 * flBeat()
 *
 * Writes the heartbeat the page reads to know that the flasher is running.
 *
 * Returns nothing.
 */
function flBeat()
{
    global $mysql;

    try
    {
        mysqli_query($mysql, "INSERT INTO modem_host.firmware_flasher (id, updated) VALUES (1, NOW()) ON DUPLICATE KEY UPDATE updated = NOW()");
    }
    catch (Throwable $e)
    {
        fwrite(STDERR, "database error: " . $e->getMessage() . "\n");
    }
}

/*
 * flWrite()
 *
 * Stores the log of a request and the heartbeat, and optionally the state.
 *
 * $id    - Id of the request.
 * $log   - The log so far (only the last FL_LOG_LIMIT bytes are kept).
 * $state - New state ("running", "done", "failed"), or "" to leave it as it is.
 * $units - Text for the "units" column, or null to leave it.
 *
 * Returns nothing.
 */
function flWrite($id, $log, $state, $units = null)
{
    global $mysql;

    /* stm32flash draws its progress with carriage returns and the script may print other control bytes; the
       database column is text, so keep printable ASCII and newlines only, and one line per progress update */
    $log = preg_replace('/\r+(?!\n)/', "\n", (string)$log);
    $log = preg_replace('/[^\x09\x0a\x20-\x7e]/', '?', $log);
    $log = preg_replace('/\n{3,}/', "\n\n", $log);
    if (strlen($log) > FL_LOG_LIMIT)
        $log = "...\n" . substr($log, -FL_LOG_LIMIT);
    $set = "log = '" . mysqli_real_escape_string($mysql, $log) . "', updated = NOW()";
    if ($state === "done" || $state === "failed")
        $set .= ", state = '" . $state . "', finished = NOW()";
    else if ($state === "running")
        $set .= ", state = 'running'";
    if ($units !== null)
        $set .= ", units = '" . mysqli_real_escape_string($mysql, $units) . "'";
    /* a database error must never end a flash half way (the services would stay stopped), so it is only reported */
    try
    {
        mysqli_query($mysql, "UPDATE modem_host.firmware_flash_runs SET " . $set . " WHERE id = " . (int)$id);
        flBeat();
    }
    catch (Throwable $e)
    {
        fwrite(STDERR, "database error: " . $e->getMessage() . "\n");
        if ($state === "done" || $state === "failed")
        {
            try
            {
                mysqli_query($mysql, "UPDATE modem_host.firmware_flash_runs SET state = '" . $state . "', finished = NOW(), updated = NOW() WHERE id = " . (int)$id);
            }
            catch (Throwable $e2)
            {
            }
        }
    }
}

/*
 * flSystemctl()
 *
 * Runs systemctl with fixed arguments (no shell) and returns what it printed.
 *
 * $args - Arguments, for example array("is-active", "modem-host.service").
 *
 * Returns the output text, trimmed.
 */
function flSystemctl($args)
{
    global $systemctl;

    $proc = proc_open(array_merge(array($systemctl), $args), array(1 => array('pipe', 'w'), 2 => array('pipe', 'w')), $pipes);
    if (!is_resource($proc))
        return '';
    stream_set_blocking($pipes[1], false);
    stream_set_blocking($pipes[2], false);
    $out = '';
    $start = time();
    while (true)
    {
        $out .= (string)stream_get_contents($pipes[1]);
        stream_get_contents($pipes[2]);
        $st = proc_get_status($proc);
        if (!$st['running'])
            break;
        if (time() - $start > 60)
        {
            proc_terminate($proc, 9);
            break;
        }
        flBeat();           /* a slow stop or start must not look like a dead flasher */
        usleep(300000);
    }
    $out .= (string)stream_get_contents($pipes[1]);
    fclose($pipes[1]);
    fclose($pipes[2]);
    proc_close($proc);
    return trim($out);
}

/*
 * flUnits()
 *
 * The services the dashboard manages, in the order they are started (modem bridge first, gateways last).
 *
 * Returns an array of unit names.
 */
function flUnits()
{
    global $mysql;
    $units = array();

    $res = mysqli_query($mysql, "SELECT unit FROM modem_host.services ORDER BY sort_order, unit");
    while ($res && ($r = mysqli_fetch_row($res)))
    {
        if (preg_match('/^[A-Za-z0-9_.@-]+\.service$/', $r[0]))
            $units[] = $r[0];
    }
    return $units;
}

/*
 * flStart()
 *
 * Starts the services again, in the order given, and adds what happened to the log.
 *
 * $units - Unit names.
 * $log   - The log (by reference).
 *
 * Returns nothing.
 */
function flStart($units, &$log)
{
    foreach ($units as $u)
    {
        flSystemctl(array('start', $u));
        $state = flSystemctl(array('is-active', $u));
        $log .= "start " . $u . ": " . ($state !== '' ? $state : 'no answer') . "\n";
    }
}

/*
 * flDo()
 *
 * Does one request: checks it, stops the services, flashes, starts the services again.
 *
 * $id - Id of the request (already marked running).
 *
 * Returns nothing; the result is in the database.
 */
function flDo($id)
{
    global $mysql, $scriptOverride, $sourceOverride;

    $log = "Flasher started.\n";
    flWrite($id, $log, "running");

    $stmt = mysqli_prepare($mysql, "SELECT q.mode, q.port, q.boot0, q.nrst, f.name, f.data, r.firmware, r.target, r.options " .
                                   "FROM modem_host.firmware_flash_runs q JOIN modem_host.firmware_files f ON f.id = q.file_id " .
                                   "JOIN modem_host.firmware_runs r ON r.id = f.run_id WHERE q.id = ?");
    mysqli_stmt_bind_param($stmt, "i", $id);
    mysqli_stmt_execute($stmt);
    mysqli_stmt_bind_result($stmt, $mode, $port, $boot0, $nrst, $name, $data, $fwId, $target, $options);
    $got = mysqli_stmt_fetch($stmt);
    mysqli_stmt_close($stmt);
    if (!$got)
    {
        flWrite($id, $log . "The file of this request is no longer in the database.\n", "failed");
        return;
    }
    $plan = fwFlashPlan($fwId, $target, $options, $name, $mode, $port, $boot0, $nrst);
    if (!$plan['ok'])
    {
        flWrite($id, $log . "Refused: " . $plan['error'] . "\n", "failed");
        return;
    }
    $root = $sourceOverride !== '' ? $sourceOverride : fwFindSource();
    $script = $scriptOverride !== '' ? $scriptOverride : ($root !== '' ? fwFindFlashScript($root) : '');
    if ($script === '')
    {
        flWrite($id, $log . "openmt_flash.sh was not found (the source files of the project: " . ($root !== '' ? $root : 'not found') . ").\n", "failed");
        return;
    }
    if (fwFindTool('stm32flash') === '')
    {
        flWrite($id, $log . "stm32flash is not installed:  sudo apt install stm32flash\n", "failed");
        return;
    }
    if (!@file_exists($port))
    {
        flWrite($id, $log . $port . " is not a serial port on this Pi.\n", "failed");
        return;
    }

    $file = tempnam(sys_get_temp_dir(), 'openmt_flash_');
    chmod($file, 0600);
    file_put_contents($file, $data);
    $log .= "Flashing " . $plan['line'] . "\nFile: " . strlen($data) . " bytes\n";

    /* stop what runs (the last started is stopped first), remembering it so it can be started again */
    $was = array();
    foreach (flUnits() as $u)
    {
        $st = flSystemctl(array('is-active', $u));
        if ($st === 'active' || $st === 'activating')
            $was[] = $u;
    }
    flWrite($id, $log, "", implode("\n", $was));
    $log .= "Stopping " . count($was) . " service(s).\n";
    flWrite($id, $log, "");
    foreach (array_reverse($was) as $u)
        flSystemctl(array('stop', $u));
    sleep(2);

    /* the script */
    $args = array_merge(array('/bin/sh', $script), $plan['args'], array($file));
    $log .= "$ " . implode(' ', array_slice($args, 1, -1)) . " <file>\n";
    $env = array('PATH' => dirname(fwFindTool('stm32flash')) . ':/usr/bin:/bin:/usr/sbin:/sbin', 'LC_ALL' => 'C');
    $pipes = array();
    $proc = proc_open($args, array(0 => array('file', '/dev/null', 'r'), 1 => array('pipe', 'w'), 2 => array('pipe', 'w')), $pipes, dirname($script), $env);
    $ok = false;
    if (!is_resource($proc))
        $log .= "Could not start the script.\n";
    else
    {
        stream_set_blocking($pipes[1], false);
        stream_set_blocking($pipes[2], false);
        $start = time();
        $last = 0;
        $open = true;
        $stopped = false;
        while ($open)
        {
            $read = array($pipes[1], $pipes[2]);
            $w = null;
            $e = null;
            if (stream_select($read, $w, $e, 1) > 0)
            {
                foreach ($read as $p)
                {
                    $chunk = fread($p, 8192);
                    if ($chunk !== false && $chunk !== '')
                        $log .= $chunk;
                }
            }
            $status = proc_get_status($proc);
            if (!$status['running'])
                $open = false;
            if (time() - $start > FL_FLASH_LIMIT_S)
            {
                proc_terminate($proc, 9);
                $log .= "\nStopped: the flash took more than " . (FL_FLASH_LIMIT_S / 60) . " minutes.\n";
                $stopped = true;
                $open = false;
            }
            if (time() - $last >= FL_BEAT_S)
            {
                flWrite($id, $log, "");
                $last = time();
            }
        }
        foreach (array($pipes[1], $pipes[2]) as $p)
        {
            $rest = stream_get_contents($p);
            if ($rest !== false && $rest !== '')
                $log .= $rest;
            fclose($p);
        }
        $exit = proc_close($proc);
        $ok = (!$stopped && $exit === 0);
        if (!$ok && !$stopped)
            $log .= "\nThe script stopped with status " . $exit . ".\n";
    }
    @unlink($file);

    /* the services come back whatever happened */
    $log .= "\nStarting the services again.\n";
    flWrite($id, $log, "");
    flStart($was, $log);
    $log .= $ok ? "\nFlashed. The modem has restarted.\n" : "\nFlashing failed. The services were started again; the modem may need the flash repeated.\n";
    flWrite($id, $log, $ok ? "done" : "failed", "");
}

/*
 * flRecover()
 *
 * At start: a request still marked running belongs to a flasher that died.  It is marked failed and the services it
 * had stopped are started again.
 *
 * Returns nothing.
 */
function flRecover()
{
    global $mysql;

    $res = mysqli_query($mysql, "SELECT id, units, IFNULL(log, '') FROM modem_host.firmware_flash_runs WHERE state = 'running'");
    $rows = array();
    while ($res && ($r = mysqli_fetch_row($res)))
        $rows[] = $r;
    foreach ($rows as $r)
    {
        $log = $r[2] . "\nThe flasher stopped in the middle of this request.\n";
        $units = $r[1] === '' ? array() : explode("\n", $r[1]);
        flStart(array_values(array_filter($units, function ($u) { return preg_match('/^[A-Za-z0-9_.@-]+\.service$/', $u); })), $log);
        flWrite($r[0], $log, "failed", "");
    }
}

/*
 * flCycle()
 *
 * One look at the table: expires old requests, takes the next waiting one and does it.
 *
 * Returns true if a request was handled.
 */
function flCycle()
{
    global $mysql;

    flBeat();
    mysqli_query($mysql, "UPDATE modem_host.firmware_flash_runs SET state = 'expired', finished = NOW(), log = 'The flasher did not take this request up in time.' " .
                         "WHERE state = 'queued' AND TIMESTAMPDIFF(SECOND, started, NOW()) > " . FL_EXPIRE_S);
    $res = mysqli_query($mysql, "SELECT id FROM modem_host.firmware_flash_runs WHERE state = 'queued' ORDER BY id LIMIT 1");
    $row = $res ? mysqli_fetch_row($res) : null;
    if (!$row)
        return false;
    flDo((int)$row[0]);
    return true;
}

/* ---- main ---- */
$systemctl      = 'systemctl';
$scriptOverride = '';
$sourceOverride = '';
$once           = false;

for ($i = 1; $i < $argc; $i++)
{
    if ($argv[$i] === '--once')
        $once = true;
    else if ($argv[$i] === '--systemctl' && isset($argv[$i + 1]))
        $systemctl = $argv[++$i];
    else if ($argv[$i] === '--script' && isset($argv[$i + 1]))
        $scriptOverride = $argv[++$i];
    else if ($argv[$i] === '--source' && isset($argv[$i + 1]))
        $sourceOverride = $argv[++$i];
    else
    {
        fwrite(STDERR, "usage: php flasher.php [--once] [--systemctl PATH] [--script PATH] [--source DIR]\n");
        exit(2);
    }
}

/* the checks of the dashboard are in its folder: next to this one in a clone, or the copy of the web server */
$lib = '';
foreach (array(dirname(__DIR__, 2) . '/dashboard/www/firmwareBuild.php', '/var/www/dashboard/firmwareBuild.php') as $candidate)
{
    if (is_file($candidate))
    {
        $lib = $candidate;
        break;
    }
}
if ($lib === '')
{
    fwrite(STDERR, "firmwareBuild.php was not found.\n");
    exit(1);
}
require_once $lib;

$mysql = false;
for ($tries = 0; $tries < 60 && !$mysql; $tries++)
{
    $mysql = @flConnect();
    if (!$mysql)
        sleep(2);
}
if (!$mysql)
{
    fwrite(STDERR, "Could not open the database.\n");
    exit(1);
}

flRecover();
if ($once)
{
    while (flCycle())
        ;
    exit(0);
}

while (true)
{
    if (!flCycle())
        sleep(1);
}
?>
