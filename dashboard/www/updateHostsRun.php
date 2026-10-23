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
 * Runs updateHosts.php for the sections of one update started from the dashboard.
 *
 *     php updateHostsRun.php <run id>
 *
 * data_query.php (command startHostUpdate) stores the request in modem_host.host_update_runs and starts this
 * script in the background.  Here every section is run on its own, the output is collected into the "log"
 * column so that the dashboard can show it while it runs, and the result of each section is written to
 * modem_host.host_update_sections.  The "updated" column is touched every few seconds; the dashboard treats an
 * update whose heartbeat has stopped as failed.
 *
 * Only the command line may run it.
 */

if (PHP_SAPI !== 'cli')
{
    http_response_code(403);
    exit;
}

set_time_limit(0);

const HU_SECTION_LIMIT_S = 900;     /* longest time one section may take */
const HU_LOG_LIMIT       = 60000;   /* longest log kept, in bytes */
const HU_BEAT_S          = 3;       /* seconds between writes of the log and heartbeat */

/*
 * connect_db()
 *
 * Opens the database with the settings from /etc/openmt.ini, the same way the dashboard does.
 *
 * Returns the connection, or false when it could not be opened.
 */
function connect_db()
{
    $mysql_host     = "localhost";
    $mysql_password = "";
    $mysql_username = "openmt";
    $foundDB        = false;

    $file = fopen("/etc/openmt.ini", "r");
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
                    $mysql_host = trim($tmp['host']);
                }
                if (stristr($line, "passwd=") != false)
                {
                    parse_str($line, $tmp);
                    $mysql_password = trim($tmp['passwd']);
                }
            }
            if (stristr($line, "[database]") != false && !$foundDB)
                $foundDB = true;
        }
        fclose($file);
    }
    return mysqli_connect($mysql_host, $mysql_username, $mysql_password);
}  /** end connect_db **/

/*
 * huEsc()
 *
 * Escapes a text for use inside a quoted SQL string.
 *
 * $text - The text.
 *
 * Returns the escaped text.
 */
function huEsc($text)
{
    global $mysql;

    return mysqli_real_escape_string($mysql, (string)$text);
}

/*
 * huWriteLog()
 *
 * Stores the log of the run, and the heartbeat, and optionally the state.  Only the last HU_LOG_LIMIT bytes of
 * the log are kept.
 *
 * $runId - Id of the run.
 * $log   - The log text so far.
 * $state - New state ("running", "done", "failed"), or "" to leave it as it is.
 *
 * Returns nothing.
 */
function huWriteLog($runId, $log, $state)
{
    global $mysql;

    if (strlen($log) > HU_LOG_LIMIT)
        $log = "...\n" . substr($log, -HU_LOG_LIMIT);
    $set = "log = '" . huEsc($log) . "', updated = NOW()";
    if ($state === "done" || $state === "failed")
        $set .= ", state = '" . $state . "', finished = NOW()";
    else if ($state === "running")
        $set .= ", state = 'running'";
    mysqli_query($mysql, "UPDATE modem_host.host_update_runs SET " . $set . " WHERE id = " . (int)$runId);
}

/*
 * huCount()
 *
 * Counts the entries of a section after its update.
 *
 * $reflType - Reflector type of the section, or null for the user id list.
 *
 * Returns the number of entries, or -1 if it could not be counted.
 */
function huCount($reflType)
{
    global $mysql;

    if ($reflType === null || $reflType === "")
        $res = mysqli_query($mysql, "SELECT COUNT(*) FROM modem_host.dmr_ids");
    else
        $res = mysqli_query($mysql, "SELECT COUNT(*) FROM modem_host.reflectors WHERE refl_type = '" . huEsc($reflType) . "'");
    if (!$res)
        return -1;
    $row = mysqli_fetch_row($res);
    mysqli_free_result($res);
    return $row ? (int)$row[0] : -1;
}

/*
 * huRunSection()
 *
 * Runs updateHosts.php for one section in its own process and collects what it prints.  The process runs in a
 * new folder in the temporary directory, because the script leaves the files it downloads in its working
 * folder.  The log and the heartbeat are written while it runs.
 *
 * $runId  - Id of the run (for the log).
 * $name   - Section name, already checked against host_update_sections.
 * $log    - The log so far; the output of the section is added to it (passed by reference).
 * $failed - Set to true if the section failed (passed by reference).
 *
 * Returns nothing.
 */
function huRunSection($runId, $name, &$log, &$failed)
{
    $work = sys_get_temp_dir() . "/openmt_hosts_" . getmypid() . "_" . $name;
    @mkdir($work, 0700, true);

    $cmd  = array(PHP_BINARY, __DIR__ . "/updateHosts.php", $name);
    $pipes = array();
    $proc = proc_open($cmd, array(1 => array("pipe", "w"), 2 => array("pipe", "w")), $pipes, $work);
    if (!is_resource($proc))
    {
        $log .= "Could not start the update.\n";
        $failed = true;
        return;
    }
    stream_set_blocking($pipes[1], false);
    stream_set_blocking($pipes[2], false);

    $text    = "";
    $start   = time();
    $lastLog = 0;
    $open    = true;
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
                if ($chunk !== false && $chunk !== "")
                    $text .= $chunk;
            }
        }
        $status = proc_get_status($proc);
        if (!$status['running'])
            $open = false;
        if (time() - $start > HU_SECTION_LIMIT_S)
        {
            proc_terminate($proc, 9);
            $text .= "\nStopped: this update took more than " . (HU_SECTION_LIMIT_S / 60) . " minutes.\n";
            $failed = true;
            $open = false;
        }
        if (time() - $lastLog >= HU_BEAT_S)
        {
            huWriteLog($runId, $log . $text, "");
            $lastLog = time();
        }
    }
    foreach (array($pipes[1], $pipes[2]) as $p)
    {
        $rest = stream_get_contents($p);
        if ($rest !== false)
            $text .= $rest;
        fclose($p);
    }
    $exit = proc_close($proc);

    if ($exit > 0 && !$failed)
        $failed = true;
    if (stripos($text, "cURL error") !== false || stripos($text, "Fatal error") !== false)
        $failed = true;

    /* the script says "Updates complete." after every section; the run says it once at the end */
    $text = str_replace("\nUpdates complete.\n", "\n", "\n" . $text);
    $log .= trim($text, "\n") . "\n";

    /* remove what the script downloaded */
    foreach (glob($work . "/*") ?: array() as $f)
        @unlink($f);
    @rmdir($work);
}

/*
 * main part: the run id is the only argument
 */
$runId = (int)($argv[1] ?? 0);
if ($runId <= 0)
{
    fwrite(STDERR, "usage: php updateHostsRun.php <run id>\n");
    exit(1);
}

$mysql = connect_db();
if (!$mysql)
{
    fwrite(STDERR, "Could not open the database.\n");
    exit(1);
}

$res = mysqli_query($mysql, "SELECT sections FROM modem_host.host_update_runs WHERE id = " . $runId . " AND state = 'queued'");
$row = $res ? mysqli_fetch_row($res) : null;
if (!$row)
{
    fwrite(STDERR, "Run " . $runId . " was not found or has been started already.\n");
    exit(1);
}
mysqli_free_result($res);

$log = "";
huWriteLog($runId, $log, "running");
mysqli_query($mysql, "UPDATE modem_host.host_update_runs SET started = NOW() WHERE id = " . $runId);

$anyFailed = false;
foreach (explode(",", $row[0]) as $name)
{
    $name = trim($name);
    $res  = mysqli_query($mysql, "SELECT label, refl_type FROM modem_host.host_update_sections WHERE section = '" . huEsc($name) . "'");
    $sec  = $res ? mysqli_fetch_row($res) : null;
    if ($sec === null || $sec === false)
    {
        $log .= "Skipped '" . $name . "': not in the list of sections.\n";
        $anyFailed = true;
        continue;
    }
    mysqli_free_result($res);

    $log .= "=== " . $sec[0] . " ===\n";
    huWriteLog($runId, $log, "");

    $failed = false;
    huRunSection($runId, $name, $log, $failed);

    $count  = huCount($sec[1]);
    $result = $failed ? "failed" : "ok";
    if ($count >= 0)
        $result .= ", " . $count . " entries";
    $log .= "Result: " . $result . "\n\n";
    if ($failed)
        $anyFailed = true;

    mysqli_query($mysql, "UPDATE modem_host.host_update_sections SET last_run = NOW(), last_result = '" . huEsc($result) .
                         "' WHERE section = '" . huEsc($name) . "'");
    huWriteLog($runId, $log, "");
}

$log .= $anyFailed ? "Finished with errors.\n" : "Updates complete.\n";
huWriteLog($runId, $log, $anyFailed ? "failed" : "done");
mysqli_close($mysql);
?>
