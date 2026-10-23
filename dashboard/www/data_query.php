<?PHP
/***************************************************************************
 *   Copyright (C) 2025 by Rick KD0OSS             *
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

set_time_limit(60);

function connect_db()
{
    global $mysql_host;
    global $mysql_username;
    global $mysql_password;
    $mysql_username = "openmt";
    $foundDB = false;

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
 * db_select()
 *
 * Runs a SELECT as a prepared statement, so values from the web request can
 * never change the query.  Every value goes in as a ? marker and is bound
 * here.
 *
 * $mysql  - Open database connection.
 * $sql    - The query, with ? where a value goes.
 * $types  - mysqli bind types, one letter per value ("s" text, "i" integer),
 *           or "" when there are no values.
 * $params - The values, in order.
 *
 * Returns an array of rows; each row is an array indexed by column number.
 * It is empty if the query fails or finds nothing.
 */
function db_select($mysql, $sql, $types, $params)
{
    $rows = array();
    $stmt = mysqli_prepare($mysql, $sql);

    if (!$stmt)
        return $rows;
    if ($types !== "")
        mysqli_stmt_bind_param($stmt, $types, ...$params);
    if (!mysqli_stmt_execute($stmt))
    {
        mysqli_stmt_close($stmt);
        return $rows;
    }

    $meta = mysqli_stmt_result_metadata($stmt);
    if ($meta)
    {
        $count = mysqli_num_fields($meta);
        $cols = array_fill(0, $count, null);
        $refs = array();
        for ($i = 0; $i < $count; $i++)
            $refs[$i] = &$cols[$i];
        mysqli_stmt_bind_result($stmt, ...$refs);
        while (mysqli_stmt_fetch($stmt))
        {
            $row = array();
            for ($i = 0; $i < $count; $i++)
                $row[$i] = $cols[$i];
            $rows[] = $row;
        }
        mysqli_free_result($meta);
    }
    mysqli_stmt_close($stmt);
    return $rows;
}

/*
 * db_exec()
 *
 * Runs an INSERT, UPDATE or DELETE as a prepared statement (see db_select()).
 *
 * Returns true if the statement ran, false if it failed.
 */
function db_exec($mysql, $sql, $types, $params)
{
    $stmt = mysqli_prepare($mysql, $sql);

    if (!$stmt)
        return false;
    if ($types !== "")
        mysqli_stmt_bind_param($stmt, $types, ...$params);
    $ok = mysqli_stmt_execute($stmt);
    mysqli_stmt_close($stmt);
    return $ok;
}

/* ---- Manage Data helpers: only these tables can be viewed or trimmed ---- */
function manage_table($name)
{
    $allowed = array('errors', 'gps', 'history', 'sms_messages');
    return in_array($name, $allowed, true) ? $name : null;
}

/* Only a logged in dashboard session may use the Manage Data commands */
function manage_require_auth()
{
    global $mysql;
    session_start();
    $ok = !empty($_SESSION['openmt_auth']);
    session_write_close();
    if (!$ok)
    {
        echo "unauthorized";
        mysqli_close($mysql);
        exit;
    }
}

/* Name of the column that holds the record time, or "" when the table has none */
function manage_date_column($table)
{
    global $mysql;
    $col = "";
    $stmt = mysqli_prepare($mysql, "SELECT COLUMN_NAME FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = 'modem_host' AND TABLE_NAME = ? AND DATA_TYPE IN ('datetime', 'timestamp', 'date') ORDER BY (COLUMN_NAME = 'datetime') DESC, ORDINAL_POSITION LIMIT 1");
    mysqli_stmt_bind_param($stmt, "s", $table);
    mysqli_stmt_execute($stmt);
    mysqli_stmt_bind_result($stmt, $name);
    if (mysqli_stmt_fetch($stmt))
        $col = $name;
    mysqli_stmt_close($stmt);
    return $col;
}

function manage_quote($col)
{
    return "`".str_replace("`", "``", $col)."`";
}

/* YYYY-MM-DD that is a real date, else null */
function manage_date($text)
{
    if (!preg_match('/^(\d{4})-(\d{2})-(\d{2})$/', $text, $m))
        return null;
    if (!checkdate((int)$m[2], (int)$m[3], (int)$m[1]))
        return null;
    return $text;
}

$mysql = connect_db();
$command = "";

if (isset($_GET['command']))
{
    global $command;
    $command = htmlspecialchars($_GET['command']);
    //    echo "Command: " . $command . "<br>";
}

if ($command == "auth")
{
    global $mysql;

    $user = htmlspecialchars($_POST['username'] ?? '');
    $pass = htmlspecialchars($_POST['passwd'] ?? '');

    if ($user === '' || $pass === '')
    {
        echo "authfailed";
        return;
    }

    $stmt = mysqli_prepare($mysql, "SELECT * FROM modem_host.auth WHERE username = ? AND password = PASSWORD(?)");
    mysqli_stmt_bind_param($stmt, "ss", $user, $pass);
    mysqli_stmt_execute($stmt);
    mysqli_stmt_store_result($stmt);
    if (mysqli_stmt_num_rows($stmt) > 0)
    {
        session_start();                      /* the Manage Data commands check this on the server */
        $_SESSION['openmt_auth'] = true;
        echo "authpassed";
    }
    else
        echo "authfailed";
    mysqli_stmt_close($stmt);
}
else
if ($command == "getModemConfig")
{
    global $mysql;

    if (isset($_GET['modem_name']))
    {
        $modem_name = $_GET['modem_name'];
        $params = "";
        $rows = db_select($mysql, "SELECT * FROM modem_host.config WHERE modem_name = ? AND module = 'config' ORDER BY id", "s", array($modem_name));
        foreach ($rows as $row)
        {
            if ($params != "") $params .= "\x1E";
            $params .= $row[3]."\x1D".$row[5];
        }
        if ($params != "")
            echo htmlentities($params);
        else
            echo "none";
    }
    else
        echo "error";
}
else
if ($command == "getProtocolConfig")
{
    global $mysql;

    if (isset($_GET['modem_name']))
    {
        $modem_name = $_GET['modem_name'];
        $params = "";
        $rows = db_select($mysql, "SELECT * FROM modem_host.config WHERE modem_name = ? AND module != 'config' ORDER BY id", "s", array($modem_name));
        foreach ($rows as $row)
        {
            if ($params != "") $params .= "\x1E";
            $params .= $row[2]."\x1D".$row[3]."\x1D".$row[5];
        }
        if ($params != "")
            echo htmlentities($params);
        else
            echo "none";
    }
    else
        echo "error";
}
else
if ($command == "getDashConfig")
{
    global $mysql;

    if (isset($_GET['modem_name']))
    {
        $modem_name = $_GET['modem_name'];
        $params = "";
        $rows = db_select($mysql, "SELECT parameter, value FROM modem_host.config WHERE modem_name = ? AND module = 'main' ORDER BY id", "s", array($modem_name));
        foreach ($rows as $row)
        {
            if ($params != "") $params .= "\x1E";
            $params .= $row[0]."\x1D".$row[1];
        }
        if ($params != "")
            echo htmlentities($params);
        else
            echo "none";
    }
    else
        echo "error";
}
else
if ($command == "saveGeneral")
{
    global $mysql;

    foreach ($_GET as $key => $value)
    {
        $safeKey   = htmlspecialchars($key);
        $safeValue = htmlspecialchars($value);

        /* Free text sent on to the DMR master: keep it as typed (the
           statement below is prepared, and the page shows it as plain text). */
        if ($safeKey == "location" || $safeKey == "description")
            $safeValue = $value;

        if ($safeKey == "command")
            continue;

        if ($safeKey == "modem_name")
        {
            $modem_name = $safeValue;
            continue;
        }

        if ($safeKey == "moduleName")
        {
            $module_name = $safeValue;
            continue;
        }

        if (!db_exec($mysql, "UPDATE modem_host.config SET value = ? WHERE modem_name = ? AND module = ? AND parameter = ?",
                     "ssss", array($safeValue, $modem_name, $module_name, $safeKey)))
        {
            mysqli_close($mysql);
            exit("failed ".$safeKey);
        }
    }

    echo "success";
}
else
if ($command == "saveModem")
{
    global $mysql;

    foreach ($_GET as $key => $value)
    {
        $safeKey   = htmlspecialchars($key);
        $safeValue = htmlspecialchars($value);

        if ($safeKey == "command")
            continue;

        if ($safeKey == "modem_name")
        {
            $modem_name = $safeValue;
            continue;
        }

//        echo "Parameter: " . $safeKey . " | Value: " . $safeValue . "<br>\n";
        if (!db_exec($mysql, "UPDATE modem_host.config SET value = ? WHERE modem_name = ? AND module = 'config' AND parameter = ?",
                     "sss", array($safeValue, $modem_name, $safeKey)))
        {
            mysqli_close($mysql);
            exit("failed ".$safeKey);
        }
    }

    echo "success";
}
else
if ($command == "getStatus")
{
    global $mysql;

    if (isset($_GET['modem_name']))
    {
        $modem_name = $_GET['modem_name'];
        $data = "";
        $id = 0;
        $rows = db_select($mysql, "SELECT * FROM modem_host.last_call WHERE modem_name = ? ORDER BY pos LIMIT 2", "s", array($modem_name));
        foreach ($rows as $row)
        {
            if ($data != "") $data .= "\x1E";
            if ($row[9] == 1)
                $tx = "on";
            else
                $tx = "off";
            $data .= "Id\x1D".$row[0]."\x1Crow\x1D".$row[1]."\x1Ctx_state\x1D".$tx."\x1Cmode\x1D".$row[3]."\x1Ctype\x1D".$row[4]."\x1CsrcCall\x1D".$row[5]."\x1Csuffix\x1D".$row[6]."\x1CdstCall\x1D".$row[7]."\x1CmetaText\x1D".$row[8];
            $id = (int)$row[0];

            $rows2 = db_select($mysql, "SELECT * FROM modem_host.sms_messages WHERE id = ?", "i", array($id));
            foreach ($rows2 as $row2)
            {
                $data .= "\x1CsmsText\x1D".$row2[2];
            }

            $rows2 = db_select($mysql, "SELECT * FROM modem_host.gps WHERE id = ?", "i", array($id));
            foreach ($rows2 as $row2)
            {
                $data .= "\x1Clatitude\x1D".$row2[1];
                $data .= "\x1Clongitude\x1D".$row2[2];
                $data .= "\x1Caltitude\x1D".$row2[3];
                $data .= "\x1Cbearing\x1D".$row2[4];
                $data .= "\x1Cspeed\x1D".$row2[5];
            }

            $rows2 = db_select($mysql, "SELECT property, value FROM modem_host.host_status WHERE modem_name = ? AND module = 'main'", "s", array($modem_name));
            foreach ($rows2 as $row2)
            {
                $data .= "\x1Cproperty\x1D".$row2[0];
                $data .= "\x1Cvalue\x1D".$row2[1];
            }
        }
        if ($data != "")
            echo htmlentities($data);
        else
            echo "none";
    }
    else
        echo "error";
}
else
if ($command == "getHistory")
{
    global $mysql;

    if (isset($_GET['modem_name']))
    {
        $modem_name = $_GET['modem_name'];
        $data = "";
        $rows1 = db_select($mysql, "SELECT MAX(id) AS id FROM modem_host.history WHERE modem_name = ? GROUP BY Source, suffix, mode ORDER BY id DESC LIMIT 25", "s", array($modem_name));
        foreach ($rows1 as $row1)
        {
            $rows2 = db_select($mysql, "SELECT id, type, mode, Source, suffix, Destination, Loss_BER, ss_message, MAX(DATE_FORMAT(datetime, '%Y-%m-%d  %H:%i')) AS datetime, duration FROM modem_host.history WHERE id = ?", "i", array((int)$row1[0]));
            foreach ($rows2 as $row)
            {
                if ($data != "") $data .= "\x1E";
                $data .= "type\x1D".$row[1]."\x1Cmode\x1D".$row[2]."\x1Csource\x1D".$row[3]."\x1Csuffix\x1D".$row[4]."\x1Cdest\x1D".$row[5]."\x1Closs_ber\x1D".$row[6]."\x1Css_message\x1D".$row[7];
                $data .= "\x1Cdatetime\x1D".$row[8]."\x1Cduration\x1D".$row[9];
            }
        }
        if ($data != "")
            echo htmlentities($data);
        else
            echo "none";
    }
    else
        echo "error";
}
else
if ($command == "getReflectors")
{
    global $mysql;

    if (isset($_GET['refl_type']))
    {
        $refl_type = htmlspecialchars($_GET['refl_type']);
        $data = "";
        $rows = db_select($mysql, "SELECT * FROM modem_host.reflectors WHERE refl_type = ? ORDER BY Nick", "s", array($refl_type));
        foreach ($rows as $row)
        {
            if ($data != "") $data .= "\x1E";
            $data .= "nick\x1D".$row[2]."\x1Cip4\x1D".$row[3]."\x1Cip6\x1D".$row[4]."\x1Cport\x1D".$row[5]."\x1Cdash\x1D".$row[6];
            $data .= "\x1Cname\x1D".$row[7]."\x1Ccountry\x1D".$row[8]."\x1Chas_modules\x1D".$row[10];
        }
        if ($data != "")
            echo htmlentities($data);
        else
            echo "none";
    }
    else
        echo "failed";
}
else
if ($command == "getLinkInput")
{
    global $mysql;

    /* A mode that links to a typed target (not a reflector list) describes it with two config rows, display_type
       'linkinput': linkLabel (text beside the box) and linkPattern (regular expression the target must match). */
    if (isset($_GET['modem_name']) && isset($_GET['mode']))
    {
        $modem_name = $_GET['modem_name'];
        $mode = htmlspecialchars($_GET['mode']);
        $label = "";
        $pattern = "";
        $found = false;
        $rows = db_select($mysql, "SELECT parameter, value FROM modem_host.config WHERE modem_name = ? AND module = ? AND display_type = 'linkinput'", "ss", array($modem_name, $mode));
        foreach ($rows as $row)
        {
            $found = true;
            if ($row[0] == "linkLabel") $label = $row[1];
            if ($row[0] == "linkPattern") $pattern = $row[1];
        }
        if ($found)
            echo str_replace("\x1D", "", $label)."\x1D".$pattern;
        else
            echo "none";
    }
    else
        echo "failed";
}
else
if ($command == "getLinkedReflectors")
{
    global $mysql;

    if (isset($_GET['modem_name']))
    {
        $modem_name = $_GET['modem_name'];
        $data = "";
        $rows = db_select($mysql, "SELECT module, value FROM modem_host.host_status WHERE modem_name = ? AND property = 'reflector' ORDER BY module", "s", array($modem_name));
        foreach ($rows as $row)
        {
            if ($data != "") $data .= "\x1E";
            $tmp = explode(' ', $row[1]);
            $data .= "refl_type\x1D".$row[0]."\x1Cnick\x1D".$tmp[0]."\x1Cmodule\x1D".$tmp[count($tmp)-1];
        }
        if ($data != "")
            echo htmlentities($data);
        else
            echo "none";
    }
    else
        echo "error";
}
else
if ($command == "dashbCom")
{
    global $mysql;
    $name = "";
    $mod = "";
    $action = "";
    $parameter = "na";

    if (isset($_GET['action']) && isset($_GET['modem_name']))
    {
        $modem_name = $_GET['modem_name'];
        $action = htmlspecialchars($_GET['action']);
        if (isset($_GET['parameter']))
            $parameter = htmlspecialchars($_GET['parameter']);

        db_exec($mysql, "INSERT INTO modem_host.dashb_commands SET modem_name = ?, command = ?, parameter = ?", "sss", array($modem_name, $action, $parameter));

        $id = mysqli_insert_id($mysql);
        if ($id < 1)
        {
            echo "failed";
        }
        else
        {
            $timeout = 10;
            while ($timeout)
            {
                $rows = db_select($mysql, "SELECT result FROM modem_host.dashb_commands WHERE id = ?", "i", array((int)$id));
                $row = isset($rows[0]) ? $rows[0] : array(NULL);
                if ($row[0] != NULL)
                {
                    echo $row[0];
                    break;
                }
                sleep(1);
                $timeout--;
            }
            db_exec($mysql, "DELETE FROM modem_host.dashb_commands WHERE id = ?", "i", array((int)$id));
            if ($timeout == 0)
                echo "failed";
        }
    }
    else
    {
        echo "failed";
    }
}
else
if ($command == "getModeOptions")
{
    global $mysql;

    if (isset($_GET['mode']) && isset($_GET['modem_name']))
    {
        $modem_name = $_GET['modem_name'];
        $mode = htmlspecialchars($_GET['mode']);
        $data = "";
        $rows = db_select($mysql, "SELECT parameter, display_type, value FROM modem_host.config WHERE modem_name = ? AND module = ? AND display_type NOT IN ('misc', 'linkinput')", "ss", array($modem_name, $mode));
        foreach ($rows as $row)
        {
            if ($data != "") $data .= "\x1E";
            $data .= "parameter\x1D".$row[0]."\x1Cdtype\x1D".$row[1]."\x1Cvalue\x1D".$row[2];
        }
        if ($data != "")
            echo htmlentities($data);
        else
            echo "none";
    }
    else
    {
        echo "failed";
    }
}
else
if ($command == "saveModeOption")
{
    global $mysql;

    if (isset($_GET['mode']) && isset($_GET['parameter']) && isset($_GET['value']) && isset($_GET['modem_name']))
    {
        $modem_name = $_GET['modem_name'];
        $mode = htmlspecialchars($_GET['mode']);
        $parameter = htmlspecialchars($_GET['parameter']);
        $value = htmlspecialchars($_GET['value']);
        if (!db_exec($mysql, "UPDATE modem_host.config SET value = ? WHERE modem_name = ? AND module = ? AND parameter = ?",
                     "ssss", array($value, $modem_name, $mode, $parameter)))
        {
            mysqli_close($mysql);
            exit("failed");
        }
        echo "success";
    }
    else
    {
        echo "failed";
    }
}

else
if ($command == "getMiscOptions")
{
    global $mysql;

    if (isset($_GET['modem_name']))
    {
        $modem_name = $_GET['modem_name'];
        $data = "";
        $stmt = mysqli_prepare($mysql, "SELECT module, parameter, value FROM modem_host.config WHERE modem_name = ? AND display_type = 'misc' ORDER BY module, id");
        mysqli_stmt_bind_param($stmt, "s", $modem_name);
        mysqli_stmt_execute($stmt);
        mysqli_stmt_bind_result($stmt, $module, $parameter, $value);
        while (mysqli_stmt_fetch($stmt))
        {
            if ($data != "") $data .= "\x1E";
            $data .= $module."\x1D".$parameter."\x1D".$value;
        }
        mysqli_stmt_close($stmt);
        if ($data != "")
            echo $data;     /* raw: the page puts it into input fields as text */
        else
            echo "none";
    }
    else
        echo "error";
}
else
if ($command == "saveMiscOption")
{
    global $mysql;

    if (isset($_GET['modem_name']) && isset($_GET['module']) && isset($_GET['parameter']) && isset($_GET['value']))
    {
        $stmt = mysqli_prepare($mysql, "UPDATE modem_host.config SET value = ? WHERE modem_name = ? AND module = ? AND parameter = ? AND display_type = 'misc'");
        mysqli_stmt_bind_param($stmt, "ssss", $_GET['value'], $_GET['modem_name'], $_GET['module'], $_GET['parameter']);
        if (mysqli_stmt_execute($stmt) && mysqli_stmt_errno($stmt) == 0)
            echo "success";
        else
            echo "failed";
        mysqli_stmt_close($stmt);
    }
    else
    {
        echo "failed";
    }
}
else
if ($command == "getManageInfo")
{
    global $mysql;
    manage_require_auth();
    header('Content-Type: application/json');

    $table = manage_table($_GET['table'] ?? '');
    if ($table === null)
    {
        echo json_encode(array('error' => 'Unknown table'));
    }
    else
    {
        $col = manage_date_column($table);
        $row = mysqli_fetch_row(mysqli_query($mysql, "SELECT COUNT(*) FROM modem_host.`".$table."`"));
        $out = array('count' => (int)$row[0], 'datecol' => $col, 'oldest' => '', 'newest' => '');
        if ($col != "" && $out['count'] > 0)
        {
            $q = manage_quote($col);
            $row = mysqli_fetch_row(mysqli_query($mysql, "SELECT MIN(".$q."), MAX(".$q.") FROM modem_host.`".$table."`"));
            $out['oldest'] = (string)$row[0];
            $out['newest'] = (string)$row[1];
        }
        echo json_encode($out, JSON_INVALID_UTF8_SUBSTITUTE);
    }
}
else
if ($command == "getManageRows")
{
    global $mysql;
    manage_require_auth();
    header('Content-Type: application/json');

    $table = manage_table($_GET['table'] ?? '');
    if ($table === null)
    {
        echo json_encode(array('error' => 'Unknown table'));
    }
    else
    {
        $size = (int)($_GET['size'] ?? 50);
        if ($size < 10) $size = 10;
        if ($size > 500) $size = 500;
        $page = (int)($_GET['page'] ?? 0);
        if ($page < 0) $page = 0;
        $offset = $page * $size;

        $col = manage_date_column($table);
        $order = ($col != "") ? manage_quote($col)." DESC, 1 DESC" : "1 DESC";
        $result = mysqli_query($mysql, "SELECT * FROM modem_host.`".$table."` ORDER BY ".$order." LIMIT ".$offset.", ".$size);

        $cols = array();
        foreach (mysqli_fetch_fields($result) as $f)
            $cols[] = $f->name;
        $rows = array();
        while ($r = mysqli_fetch_row($result))
            $rows[] = $r;
        echo json_encode(array('columns' => $cols, 'rows' => $rows), JSON_INVALID_UTF8_SUBSTITUTE);
    }
}
else
if ($command == "countOlder")
{
    global $mysql;
    manage_require_auth();
    header('Content-Type: application/json');

    $table = manage_table($_GET['table'] ?? '');
    $before = manage_date($_GET['before'] ?? '');
    $col = ($table !== null) ? manage_date_column($table) : "";
    if ($table === null || $before === null || $col == "")
    {
        echo json_encode(array('error' => 'Invalid table or date'));
    }
    else
    {
        $limit = $before." 00:00:00";
        $stmt = mysqli_prepare($mysql, "SELECT COUNT(*) FROM modem_host.`".$table."` WHERE ".manage_quote($col)." < ?");
        mysqli_stmt_bind_param($stmt, "s", $limit);
        mysqli_stmt_execute($stmt);
        mysqli_stmt_bind_result($stmt, $n);
        mysqli_stmt_fetch($stmt);
        mysqli_stmt_close($stmt);
        echo json_encode(array('count' => (int)$n));
    }
}
else
if ($command == "deleteOlder")
{
    global $mysql;
    manage_require_auth();
    header('Content-Type: application/json');

    if ($_SERVER['REQUEST_METHOD'] !== 'POST')
    {
        echo json_encode(array('error' => 'POST required'));
    }
    else
    {
        $table = manage_table($_POST['table'] ?? '');
        $before = manage_date($_POST['before'] ?? '');
        $col = ($table !== null) ? manage_date_column($table) : "";
        if ($table === null || $before === null || $col == "")
        {
            echo json_encode(array('error' => 'Invalid table or date'));
        }
        else
        {
            $limit = $before." 00:00:00";
            $stmt = mysqli_prepare($mysql, "DELETE FROM modem_host.`".$table."` WHERE ".manage_quote($col)." < ?");
            mysqli_stmt_bind_param($stmt, "s", $limit);
            if (mysqli_stmt_execute($stmt))
                echo json_encode(array('deleted' => mysqli_stmt_affected_rows($stmt)));
            else
                echo json_encode(array('error' => 'Delete failed: '.mysqli_stmt_error($stmt)));
            mysqli_stmt_close($stmt);
        }
    }
}

else
if ($command == "getServices")
{
    /* The list of units the dashboard may control comes from modem_host.services, so the page needs no
       knowledge of any RF mode. Only a logged in session may see it. */
    global $mysql;
    manage_require_auth();
    header('Content-Type: application/json');

    $rows = array();
    $alive = false;
    $res = mysqli_query($mysql, "SELECT unit, label, description, active, enabled, UNIX_TIMESTAMP(NOW()) - UNIX_TIMESTAMP(updated) AS age FROM modem_host.services ORDER BY sort_order, label");
    if ($res)
    {
        while ($r = mysqli_fetch_assoc($res))
        {
            if ($r['age'] !== null && (int)$r['age'] <= 15)
                $alive = true;
            unset($r['age']);
            $rows[] = $r;
        }
    }
    echo json_encode(array('services' => $rows, 'managerAlive' => $alive), JSON_INVALID_UTF8_SUBSTITUTE);
}
else
if ($command == "serviceAction")
{
    /* Writes a request into modem_host.service_commands. The service manager (running as root) carries it out
       after checking it again. A custom header is required so that another web site cannot send this request
       for a logged in person. */
    global $mysql;
    manage_require_auth();
    header('Content-Type: application/json');

    $action = $_POST['action'] ?? '';
    $unit = $_POST['unit'] ?? '';
    if ($_SERVER['REQUEST_METHOD'] !== 'POST' || ($_SERVER['HTTP_X_OPENMT'] ?? '') !== '1')
    {
        echo json_encode(array('error' => 'Invalid request'));
    }
    else
    if (in_array($action, array('reboot', 'poweroff'), true))
    {
        /* Power actions of the Pi: only when listed in modem_host.system_actions, and always for the unit "system" */
        $stmt = mysqli_prepare($mysql, "SELECT COUNT(*) FROM modem_host.system_actions WHERE action = ?");
        mysqli_stmt_bind_param($stmt, "s", $action);
        mysqli_stmt_execute($stmt);
        mysqli_stmt_bind_result($stmt, $found);
        mysqli_stmt_fetch($stmt);
        mysqli_stmt_close($stmt);
        if ((int)$found == 0 || $unit !== 'system')
        {
            echo json_encode(array('error' => 'Unknown action'));
        }
        else
        {
            $stmt = mysqli_prepare($mysql, "INSERT INTO modem_host.service_commands (unit, action) VALUES ('system', ?)");
            mysqli_stmt_bind_param($stmt, "s", $action);
            if (mysqli_stmt_execute($stmt))
                echo json_encode(array('id' => (int)mysqli_insert_id($mysql)));
            else
                echo json_encode(array('error' => 'Could not store the request'));
            mysqli_stmt_close($stmt);
        }
    }
    else
    if (!in_array($action, array('start', 'stop', 'restart', 'enable', 'disable'), true))
    {
        echo json_encode(array('error' => 'Unknown action'));
    }
    else
    {
        $stmt = mysqli_prepare($mysql, "SELECT COUNT(*) FROM modem_host.services WHERE unit = ?");
        mysqli_stmt_bind_param($stmt, "s", $unit);
        mysqli_stmt_execute($stmt);
        mysqli_stmt_bind_result($stmt, $found);
        mysqli_stmt_fetch($stmt);
        mysqli_stmt_close($stmt);
        if ((int)$found == 0)
        {
            echo json_encode(array('error' => 'Unknown service'));
        }
        else
        {
            $stmt = mysqli_prepare($mysql, "INSERT INTO modem_host.service_commands (unit, action) VALUES (?, ?)");
            mysqli_stmt_bind_param($stmt, "ss", $unit, $action);
            if (mysqli_stmt_execute($stmt))
                echo json_encode(array('id' => (int)mysqli_insert_id($mysql)));
            else
                echo json_encode(array('error' => 'Could not store the request'));
            mysqli_stmt_close($stmt);
        }
    }
}
else
if ($command == "getSystemActions")
{
    /* The power actions listed in the database; the dashboard builds its buttons from them. */
    global $mysql;
    manage_require_auth();
    header('Content-Type: application/json');

    $rows = array();
    $res = mysqli_query($mysql, "SELECT action, label, description, confirm FROM modem_host.system_actions ORDER BY sort_order, label");
    if ($res)
    {
        while ($r = mysqli_fetch_assoc($res))
            $rows[] = $r;
    }
    echo json_encode(array('actions' => $rows), JSON_INVALID_UTF8_SUBSTITUTE);
}
else
if ($command == "getServiceResult")
{
    /* Result of one request: null while the service manager has not finished it. */
    global $mysql;
    manage_require_auth();
    header('Content-Type: application/json');

    $id = (int)($_GET['id'] ?? 0);
    $stmt = mysqli_prepare($mysql, "SELECT result FROM modem_host.service_commands WHERE id = ?");
    mysqli_stmt_bind_param($stmt, "i", $id);
    mysqli_stmt_execute($stmt);
    mysqli_stmt_bind_result($stmt, $result);
    $got = mysqli_stmt_fetch($stmt);
    mysqli_stmt_close($stmt);
    echo json_encode(array('result' => $got ? $result : 'unknown request'));
}
else
if ($command == "getHostUpdates")
{
    /* The host lists that can be updated come from modem_host.host_update_sections, so the page needs no
       knowledge of any RF mode.  The answer also holds the latest run (state and log) so that the page can
       show the progress of an update.  An update whose heartbeat stopped for 90 seconds is marked failed. */
    global $mysql;
    manage_require_auth();
    header('Content-Type: application/json');

    mysqli_query($mysql, "UPDATE modem_host.host_update_runs SET state = 'failed', finished = NOW(), log = CONCAT(IFNULL(log, ''), 'The update stopped without finishing.\\n') " .
                         "WHERE state IN ('queued', 'running') AND TIMESTAMPDIFF(SECOND, updated, NOW()) > 90");
    $sections = array();
    $res = mysqli_query($mysql, "SELECT section, label, description, last_run, last_result FROM modem_host.host_update_sections ORDER BY sort_order, label");
    if ($res)
    {
        while ($r = mysqli_fetch_assoc($res))
            $sections[] = $r;
    }
    $run = null;
    $res = mysqli_query($mysql, "SELECT id, sections, state, started, finished, log FROM modem_host.host_update_runs ORDER BY id DESC LIMIT 1");
    if ($res)
        $run = mysqli_fetch_assoc($res);
    echo json_encode(array('sections' => $sections, 'run' => $run ? $run : null), JSON_INVALID_UTF8_SUBSTITUTE);
}
else
if ($command == "startHostUpdate")
{
    /* Stores a request in modem_host.host_update_runs and starts updateHostsRun.php in the background.  Only
       names listed in host_update_sections are accepted ("all" means every listed section) and one update
       runs at a time.  A custom header is required so that another web site cannot send this request for a
       logged in person. */
    global $mysql;
    manage_require_auth();
    header('Content-Type: application/json');

    if ($_SERVER['REQUEST_METHOD'] !== 'POST' || ($_SERVER['HTTP_X_OPENMT'] ?? '') !== '1')
    {
        echo json_encode(array('error' => 'Invalid request'));
    }
    else
    {
        $listed = array();
        $res = mysqli_query($mysql, "SELECT section FROM modem_host.host_update_sections ORDER BY sort_order, label");
        if ($res)
        {
            while ($r = mysqli_fetch_row($res))
                $listed[] = $r[0];
        }
        $asked = trim($_POST['sections'] ?? '');
        $chosen = array();
        if ($asked === 'all')
            $chosen = $listed;
        else
        {
            foreach (explode(',', $asked) as $name)
            {
                $name = trim($name);
                if ($name !== '' && in_array($name, $listed, true) && !in_array($name, $chosen, true))
                    $chosen[] = $name;
            }
        }
        $php = '';
        foreach (array(PHP_BINDIR . '/php', '/usr/bin/php', '/usr/local/bin/php') as $candidate)
        {
            if (@is_executable($candidate))
            {
                $php = $candidate;
                break;
            }
        }
        $disabled = array_map('trim', explode(',', (string)ini_get('disable_functions')));

        if (count($chosen) == 0)
            echo json_encode(array('error' => 'No host list was chosen'));
        else
        if ($php === '' || in_array('exec', $disabled, true) || !function_exists('exec'))
            echo json_encode(array('error' => 'The web server cannot start the update (php command line or exec() is not available)'));
        else
        {
            /* the lock keeps two requests from starting two updates */
            $lock = mysqli_query($mysql, "SELECT GET_LOCK('openmt_host_update', 5)");
            $got = $lock ? mysqli_fetch_row($lock) : null;
            if (!$got || (int)$got[0] != 1)
                echo json_encode(array('error' => 'The database is busy, try again'));
            else
            {
                $busy = mysqli_query($mysql, "SELECT COUNT(*) FROM modem_host.host_update_runs WHERE state IN ('queued', 'running') AND TIMESTAMPDIFF(SECOND, IFNULL(updated, started), NOW()) <= 90");
                $count = $busy ? mysqli_fetch_row($busy) : array(0);
                if ((int)$count[0] > 0)
                    echo json_encode(array('error' => 'An update is already running'));
                else
                {
                    mysqli_query($mysql, "DELETE FROM modem_host.host_update_runs WHERE started < DATE_SUB(NOW(), INTERVAL 30 DAY)");
                    $csv = implode(',', $chosen);
                    $stmt = mysqli_prepare($mysql, "INSERT INTO modem_host.host_update_runs (sections, state, started, updated) VALUES (?, 'queued', NOW(), NOW())");
                    mysqli_stmt_bind_param($stmt, "s", $csv);
                    if (!mysqli_stmt_execute($stmt))
                        echo json_encode(array('error' => 'Could not store the request'));
                    else
                    {
                        $id = (int)mysqli_insert_id($mysql);
                        $cmd = 'nohup ' . escapeshellarg($php) . ' ' . escapeshellarg(__DIR__ . '/updateHostsRun.php') . ' ' . $id .
                               ' > /dev/null 2>&1 &';
                        exec($cmd);
                        echo json_encode(array('id' => $id));
                    }
                    mysqli_stmt_close($stmt);
                }
                mysqli_query($mysql, "SELECT RELEASE_LOCK('openmt_host_update')");
            }
        }
    }
}

mysqli_close($mysql);
?>
