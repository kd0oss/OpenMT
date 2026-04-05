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
        echo "authpassed";
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

    if (isset($_GET['title']) && isset($_GET['modem_name']) && isset($_GET['moduleName']))
    {
        $modem_name = $_GET['modem_name'];
        $module_name = $_GET['moduleName'];
        $value = htmlspecialchars($_GET['title']);
        $query = "UPDATE modem_host.config SET value = '".$value."' WHERE module = '".$module_name."' ";
        $query .= "AND modem_name = '".$modem_name."' AND parameter = 'title'";
        if (mysqli_query($mysql, $query) == false)
        {
            mysqli_close($mysql);
            exit("failed title");
        }
    }

    if (isset($_GET['callsign']) && isset($_GET['modem_name']) && isset($_GET['moduleName']))
    {
        $modem_name = $_GET['modem_name'];
        $module_name = $_GET['moduleName'];
        $value = $_GET['callsign'];
        $query = "UPDATE modem_host.config SET value = '".$value."' WHERE module = '".$module_name."' ";
        $query .= "AND modem_name = '".$modem_name."' AND parameter = 'callsign'";
        if (mysqli_query($mysql, $query) == false)
        {
            mysqli_close($mysql);
            exit("failed callsign");
        }
    }

    if (isset($_GET['username']) && isset($_GET['modem_name']) && isset($_GET['moduleName']))
    {
        $modem_name = $_GET['modem_name'];
        $module_name = $_GET['moduleName'];
        $value = htmlspecialchars($_GET['username']);
        $query = "UPDATE modem_host.config SET value = '".$value."' WHERE module = '".$module_name."' ";
        $query .= "AND modem_name = '".$modem_name."' AND parameter = 'username'";
        if (mysqli_query($mysql, $query) == false)
        {
            mysqli_close($mysql);
            exit("failed username");
        }
    }

    if (isset($_GET['latitude']) && isset($_GET['modem_name']) && isset($_GET['moduleName']))
    {
        $modem_name = $_GET['modem_name'];
        $module_name = $_GET['moduleName'];
        $value = htmlspecialchars($_GET['latitude']);
        $query = "UPDATE modem_host.config SET value = '".$value."' WHERE module = '".$module_name."' ";
        $query .= "AND modem_name = '".$modem_name."' AND parameter = 'latitude'";
        if (mysqli_query($mysql, $query) == false)
        {
            mysqli_close($mysql);
            exit("failed latitude");
        }
    }

    if (isset($_GET['longitude']) && isset($_GET['modem_name']) && isset($_GET['moduleName']))
    {
        $modem_name = $_GET['modem_name'];
        $module_name = $_GET['moduleName'];
        $value = htmlspecialchars($_GET['longitude']);
        $query = "UPDATE modem_host.config SET value = '".$value."' WHERE module = '".$module_name."' ";
        $query .= "AND modem_name = '".$modem_name."' AND parameter = 'longitude'";
        if (mysqli_query($mysql, $query) == false)
        {
            mysqli_close($mysql);
            exit("failed longitude");
        }
    }

    if (isset($_POST['username']) && isset($_POST['passwd']))
    {
        $user = htmlspecialchars($_POST['username']);
        $pass = htmlspecialchars($_POST['passwd']);
        $query = "UPDATE modem_host.auth SET username = '".$user."', password = PASSWORD('".$pass."')";
        if (mysqli_query($mysql, $query) == false)
        {
            $query = "INSERT INTO modem_host.auth SET username = '".$user."', password = PASSWORD('".$pass."')";
            if (mysqli_query($mysql, $query) == false)
            {
                mysqli_close($mysql);
                exit("failed username or password");
            }
        }
    }

    echo "success";
}
else
if ($command == "saveModem")
{
    global $mysql;

    if (isset($_GET['rxInvert']) && isset($_GET['modem_name']))
    {
        $modem_name = $_GET['modem_name'];
        $value = $_GET['rxInvert'];
        $query = "UPDATE modem_host.config SET value = '".$value."' WHERE modem_name = '".$modem_name."' ";
        $query .= "AND module = 'config' AND parameter = 'rxInvert'";
        if (mysqli_query($mysql, $query) == false)
        {
            mysqli_close($mysql);
            exit("failed rxinvert");
        }
    }

    if (isset($_GET['txInvert']) && isset($_GET['modem_name']))
    {
        $modem_name = $_GET['modem_name'];
        $value = $_GET['txInvert'];
        $query = "UPDATE modem_host.config SET value = '".$value."' WHERE modem_name = '".$modem_name."' ";
        $query .= "AND module = 'config' AND parameter = 'txInvert'";
        if (mysqli_query($mysql, $query) == false)
        {
            mysqli_close($mysql);
            exit("failed txinvert");
        }
    }

    if (isset($_GET['pttInvert']) && isset($_GET['modem_name']))
    {
        $modem_name = $_GET['modem_name'];
        $value = $_GET['pttInvert'];
        $query = "UPDATE modem_host.config SET value = '".$value."' WHERE modem_name = '".$modem_name."' ";
        $query .= "AND module = 'config' AND parameter = 'pttInvert'";
        if (mysqli_query($mysql, $query) == false)
        {
            mysqli_close($mysql);
            exit("failed pttinvert");
        }
    }

    if (isset($_GET['useCOSAsLockout']) && isset($_GET['modem_name']))
    {
        $modem_name = $_GET['modem_name'];
        $value = $_GET['useCOSAsLockout'];
        $query = "UPDATE modem_host.config SET value = '".$value."' WHERE modem_name = '".$modem_name."' ";
        $query .= "AND module = 'config' AND parameter = 'useCOSAsLockout'";
        if (mysqli_query($mysql, $query) == false)
        {
            mysqli_close($mysql);
            exit("failed useCOSAsLockout");
        }
    }

    if (isset($_GET['debug']) && isset($_GET['modem_name']))
    {
        $modem_name = $_GET['modem_name'];
        $value = $_GET['debug'];
        $query = "UPDATE modem_host.config SET value = '".$value."' WHERE modem_name = '".$modem_name."' ";
        $query .= "AND module = 'config' AND parameter = 'debug'";
        if (mysqli_query($mysql, $query) == false)
        {
            mysqli_close($mysql);
            exit("failed debug");
        }
    }

    if (isset($_GET['txDelay']) && isset($_GET['modem_name']))
    {
        $modem_name = $_GET['modem_name'];
        $value = $_GET['txDelay'];
        $query = "UPDATE modem_host.config SET value = '".$value."' WHERE modem_name = '".$modem_name."' ";
        $query .= "AND module = 'config' AND parameter = 'txDelay'";
        if (mysqli_query($mysql, $query) == false)
        {
            mysqli_close($mysql);
            exit("failed txDelay");
        }
    }

    if (isset($_GET['rxLevel']) && isset($_GET['modem_name']))
    {
        $modem_name = $_GET['modem_name'];
        $value = $_GET['rxLevel'];
        $query = "UPDATE modem_host.config SET value = '".$value."' WHERE modem_name = '".$modem_name."' ";
        $query .= "AND module = 'config' AND parameter = 'rxLevel'";
        if (mysqli_query($mysql, $query) == false)
        {
            mysqli_close($mysql);
            exit("failed rxLevel");
        }
    }

    if (isset($_GET['rfTXLevel']) && isset($_GET['modem_name']))
    {
        $modem_name = $_GET['modem_name'];
        $value = $_GET['rfTXLevel'];
        $query = "UPDATE modem_host.config SET value = '".$value."' WHERE modem_name = '".$modem_name."' ";
        $query .= "AND module = 'config' AND parameter = 'rfTXLevel'";
        if (mysqli_query($mysql, $query) == false)
        {
            mysqli_close($mysql);
            exit("failed rfTXLevel");
        }
    }

    if (isset($_GET['rxFrequency']) && isset($_GET['modem_name']))
    {
        $modem_name = $_GET['modem_name'];
        $value = $_GET['rxFrequency'];
        $query = "UPDATE modem_host.config SET value = '".$value."' WHERE modem_name = '".$modem_name."' ";
        $query .= "AND module = 'config' AND parameter = 'rxFrequency'";
        if (mysqli_query($mysql, $query) == false)
        {
            mysqli_close($mysql);
            exit("failed rxFrequency");
        }
    }

    if (isset($_GET['txFrequency']) && isset($_GET['modem_name']))
    {
        $modem_name = $_GET['modem_name'];
        $value = $_GET['txFrequency'];
        $query = "UPDATE modem_host.config SET value = '".$value."' WHERE modem_name = '".$modem_name."' ";
        $query .= "AND module = 'config' AND parameter = 'txFrequency'";
        if (mysqli_query($mysql, $query) == false)
        {
            mysqli_close($mysql);
            exit("failed txFrequency");
        }
    }

    if (isset($_GET['rxDCOffset']) && isset($_GET['modem_name']))
    {
        $modem_name = $_GET['modem_name'];
        $value = $_GET['rxDCOffset'];
        $query = "UPDATE modem_host.config SET value = '".$value."' WHERE modem_name = '".$modem_name."' ";
        $query .= "AND module = 'config' AND parameter = 'rxDCOffset'";
        if (mysqli_query($mysql, $query) == false)
        {
            mysqli_close($mysql);
            exit("failed rxDCOffset");
        }
    }

    if (isset($_GET['txDCOffset']) && isset($_GET['modem_name']))
    {
        $modem_name = $_GET['modem_name'];
        $value = $_GET['txDCOffset'];
        $query = "UPDATE modem_host.config SET value = '".$value."' WHERE modem_name = '".$modem_name."' ";
        $query .= "AND module = 'config' AND parameter = 'txDCOffset'";
        if (mysqli_query($mysql, $query) == false)
        {
            mysqli_close($mysql);
            exit("failed txDCOffset");
        }
    }

    if (isset($_GET['modem']) && isset($_GET['modem_name']))
    {
        $modem_name = $_GET['modem_name'];
        $value = htmlspecialchars($_GET['modem']);
        $query = "UPDATE modem_host.config SET value = '".$value."' WHERE modem_name = '".$modem_name."' ";
        $query .= "AND module = 'config' AND parameter = 'modem'";
        if (mysqli_query($mysql, $query) == false)
        {
            mysqli_close($mysql);
            exit("failed modem");
        }
    }

    if (isset($_GET['baud']) && isset($_GET['modem_name']))
    {
        $modem_name = $_GET['modem_name'];
        $value = $_GET['baud'];
        $query = "UPDATE modem_host.config SET value = '".$value."' WHERE modem_name = '".$modem_name."' ";
        $query .= "AND module = 'config' AND parameter = 'baud'";
        if (mysqli_query($mysql, $query) == false)
        {
            mysqli_close($mysql);
            exit("failed baud");
        }
    }

    if (isset($_GET['port']) && isset($_GET['modem_name']))
    {
        $modem_name = $_GET['modem_name'];
        $value = htmlspecialchars($_GET['port']);
        $query = "UPDATE modem_host.config SET value = '".$value."' WHERE modem_name = '".$modem_name."' ";
        $query .= "AND module = 'config' AND parameter = 'port'";
        if (mysqli_query($mysql, $query) == false)
        {
            mysqli_close($mysql);
            exit("failed port");
        }
    }

    if (isset($_GET['mode']) && isset($_GET['modem_name']))
    {
        $modem_name = $_GET['modem_name'];
        $value = $_GET['mode'];
        $query = "UPDATE modem_host.config SET value = '".$value."' WHERE modem_name = '".$modem_name."' ";
        $query .= "AND module = 'config' AND parameter = 'mode'";
        if (mysqli_query($mysql, $query) == false)
        {
            mysqli_close($mysql);
            exit("failed mode");
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
        $query = "SELECT parameter, display_type, value FROM modem_host.config WHERE modem_name = '".$modem_name."' AND module = '".$mode."'";
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

mysqli_close($mysql);
?>
