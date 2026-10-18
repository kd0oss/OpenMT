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
        $result = mysqli_query($mysql, "SELECT * FROM modem_host.config WHERE modem_name = '$modem_name' AND module = 'config' ORDER BY id");
        while ($row = mysqli_fetch_row($result))
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
        $result = mysqli_query($mysql, "SELECT * FROM modem_host.config WHERE modem_name = '$modem_name' AND module != 'config' ORDER BY id");
        while ($row = mysqli_fetch_row($result))
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
        $result = mysqli_query($mysql, "SELECT parameter, value FROM modem_host.config WHERE modem_name = '$modem_name' AND module = 'main' ORDER BY id");
        while ($row = mysqli_fetch_row($result))
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

        $query = "UPDATE modem_host.config SET value = '".$safeValue."' WHERE modem_name = '".$modem_name."' ";
        $query .= "AND module = '".$module_name."' AND parameter = '".$safeKey."'";
        if (mysqli_query($mysql, $query) == false)
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
        $query = "UPDATE modem_host.config SET value = '".$safeValue."' WHERE modem_name = '".$modem_name."' ";
        $query .= "AND module = 'config' AND parameter = '".$safeKey."'";
        if (mysqli_query($mysql, $query) == false)
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
        $result = mysqli_query($mysql, "SELECT * FROM modem_host.last_call WHERE modem_name = '".$modem_name."' ORDER BY pos LIMIT 2");
        while ($row = mysqli_fetch_row($result))
        {
            if ($data != "") $data .= "\x1E";
            if ($row[9] == 1)
                $tx = "on";
            else
                $tx = "off";
            $data .= "Id\x1D".$row[0]."\x1Crow\x1D".$row[1]."\x1Ctx_state\x1D".$tx."\x1Cmode\x1D".$row[3]."\x1Ctype\x1D".$row[4]."\x1CsrcCall\x1D".$row[5]."\x1Csuffix\x1D".$row[6]."\x1CdstCall\x1D".$row[7]."\x1CmetaText\x1D".$row[8];
            $id = $row[0];

            $result2 = mysqli_query($mysql, "SELECT * FROM modem_host.sms_messages WHERE id = ".$id);
            while ($row2 = mysqli_fetch_row($result2))
            {
                $data .= "\x1CsmsText\x1D".$row2[2];
            }

            $result2 = mysqli_query($mysql, "SELECT * FROM modem_host.gps WHERE id = ".$id);
            while ($row2 = mysqli_fetch_row($result2))
            {
                $data .= "\x1Clatitude\x1D".$row2[1];
                $data .= "\x1Clongitude\x1D".$row2[2];
                $data .= "\x1Caltitude\x1D".$row2[3];
                $data .= "\x1Cbearing\x1D".$row2[4];
                $data .= "\x1Cspeed\x1D".$row2[5];
            }

            $result2 = mysqli_query($mysql, "SELECT property, value FROM modem_host.host_status WHERE modem_name = '".$modem_name."' AND module = 'main'");
            while ($row2 = mysqli_fetch_row($result2))
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
        $result = mysqli_query($mysql, "SELECT MAX(id) AS id FROM modem_host.history WHERE modem_name = '".$modem_name."' GROUP BY Source, suffix, mode ORDER BY id DESC LIMIT 25");
        while ($row1 = mysqli_fetch_row($result))
        {
            $result2 = mysqli_query($mysql, "SELECT id, type, mode, Source, suffix, Destination, Loss_BER, ss_message, MAX(DATE_FORMAT(datetime, '%Y-%m-%d  %H:%i')) AS datetime, duration FROM modem_host.history WHERE id = ".$row1[0]);
            while ($row = mysqli_fetch_row($result2))
            {
                if ($data != "") $data .= "\x1E";
                $data .= "type\x1D".$row[1]."\x1Cmode\x1D".$row[2]."\x1Csource\x1D".$row[3]."\x1Csuffix\x1D".$row[4]."\x1Cdest\x1D".$row[5]."\x1Closs_ber\x1D".$row[6]."\x1Css_message\x1D".$row[7];
                $data .= "\x1Cdatetime\x1D".$row[8]."\x1Cduration\x1D".$row[9];
            }
            mysqli_free_result($result2);
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
        $result = mysqli_query($mysql, "SELECT * FROM modem_host.reflectors WHERE refl_type = '".$refl_type."' ORDER BY Nick");
        while ($row = mysqli_fetch_row($result))
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
if ($command == "getLinkedReflectors")
{
    global $mysql;

    if (isset($_GET['modem_name']))
    {
        $modem_name = $_GET['modem_name'];
        $data = "";
        $result = mysqli_query($mysql, "SELECT module, value FROM modem_host.host_status WHERE modem_name = '".$modem_name."' AND property = 'reflector' ORDER BY module");
        while ($row = mysqli_fetch_row($result))
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

        mysqli_query($mysql, "INSERT INTO modem_host.dashb_commands SET modem_name = '".$modem_name."', command = '".$action."', parameter = '".$parameter."'");

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
                $result = mysqli_query($mysql, "SELECT result FROM modem_host.dashb_commands WHERE id = $id");
                $row = mysqli_fetch_row($result);
                if ($row[0] != NULL)
                {
                    echo $row[0];
                    break;
                }
                sleep(1);
                $timeout--;
            }
            mysqli_query($mysql, "DELETE FROM modem_host.dashb_commands WHERE id = $id");
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
        $query = "SELECT parameter, display_type, value FROM modem_host.config WHERE modem_name = '".$modem_name."' AND module = '".$mode."' AND display_type != 'misc'";
        $result = mysqli_query($mysql, $query);
        while ($row = mysqli_fetch_row($result))
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
        $query = "UPDATE modem_host.config SET value = '$value' WHERE modem_name = '".$modem_name."' AND module = '".$mode."' AND parameter = '".$parameter."'";
        if (mysqli_query($mysql, $query) == false)
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

mysqli_close($mysql);
?>
