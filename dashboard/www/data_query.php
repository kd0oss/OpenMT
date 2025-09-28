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
                    $mysql_host = trim($host);
                }
                if (stristr($line, "passwd=") != false)
                {
                    parse_str($line, $tmp);
                    $mysql_password = trim($passwd);
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

    $user = htmlspecialchars($_GET['username']);
    $pass = htmlspecialchars($_GET['passwd']);
    $result = mysqli_query($mysql, "SELECT * FROM modem_host.auth WHERE username = '".$user."' AND password = PASSWORD('".$pass."')");
    $rows = mysqli_affected_rows($mysql);
    if ($rows > 0)
        echo "authpassed";
    else
        echo "authfailed";
}

if ($command == "getModemConfig")
{
    global $mysql;

    $params = "";
    $result = mysqli_query($mysql, "SELECT * FROM dvmodem.config WHERE modem_name = 'modem1' AND protocol = 'config' ORDER BY id");
    while ($row = mysqli_fetch_row($result))
    {
        if ($params != "") $params .= "\x1E";
        $params .= $row[3]."\x1D".$row[4];
    }
    if ($params != "")
        echo htmlentities($params);
    else
        echo "none";
}

if ($command == "getProtocolConfig")
{
    global $mysql;

    $params = "";
    $result = mysqli_query($mysql, "SELECT * FROM dvmodem.config WHERE modem_name = 'modem1' AND protocol != 'config' ORDER BY id");
    while ($row = mysqli_fetch_row($result))
    {
        if ($params != "") $params .= "\x1E";
        $params .= $row[2]."\x1D".$row[3]."\x1D".$row[4];
    }
    if ($params != "")
        echo htmlentities($params);
    else
        echo "none";
}

if ($command == "getDashConfig")
{
    global $mysql;

    $params = "";
    $result = mysqli_query($mysql, "SELECT * FROM modem_host.config WHERE module = 'main' ORDER BY id");
    while ($row = mysqli_fetch_row($result))
    {
        if ($params != "") $params .= "\x1E";
        $params .= $row[2]."\x1D".$row[3];
    }
    if ($params != "")
        echo htmlentities($params);
    else
        echo "none";
}

if ($command == "saveGeneral")
{
    global $mysql;

    $module_name = $_GET['moduleName'];
    if (isset($_GET['title']))
    {
        $value = htmlspecialchars($_GET['title']);
        $query = "UPDATE modem_host.config SET value = '".$value."' WHERE module = '".$module_name."' ";
        $query .= "AND parameter = 'title'";
        if (mysqli_query($mysql, $query) == false)
        {
            mysqli_close($mysql);
            exit("failed title");
        }
    }

    if (isset($_GET['callsign']))
    {
        $value = $_GET['callsign'];
        $query = "UPDATE modem_host.config SET value = '".$value."' WHERE module = '".$module_name."' ";
        $query .= "AND parameter = 'callsign'";
        if (mysqli_query($mysql, $query) == false)
        {
            mysqli_close($mysql);
            exit("failed callsign");
        }
    }

    if (isset($_GET['username']))
    {
        $value = htmlspecialchars($_GET['username']);
        $query = "UPDATE modem_host.config SET value = '".$value."' WHERE module = '".$module_name."' ";
        $query .= "AND parameter = 'username'";
        if (mysqli_query($mysql, $query) == false)
        {
            mysqli_close($mysql);
            exit("failed username");
        }
    }

    if (isset($_GET['username']) && isset($_GET['passwd']))
    {
        $user = htmlspecialchars($_GET['username']);
        $pass = htmlspecialchars($_GET['passwd']);
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

if ($command == "saveModem")
{
    global $mysql;

    $modem_name = $_GET['modem_name'];
    if (isset($_GET['rxInvert']))
    {
        $value = $_GET['rxInvert'];
        $query = "UPDATE dvmodem.config SET value = '".$value."' WHERE modem_name = '".$modem_name."' ";
        $query .= "AND protocol = 'config' AND parameter = 'rxInvert'";
        if (mysqli_query($mysql, $query) == false)
        {
            mysqli_close($mysql);
            exit("failed rxinvert");
        }
    }

    if (isset($_GET['txInvert']))
    {
        $value = $_GET['txInvert'];
        $query = "UPDATE dvmodem.config SET value = '".$value."' WHERE modem_name = '".$modem_name."' ";
        $query .= "AND protocol = 'config' AND parameter = 'txInvert'";
        if (mysqli_query($mysql, $query) == false)
        {
            mysqli_close($mysql);
            exit("failed txinvert");
        }
    }

    if (isset($_GET['pttInvert']))
    {
        $value = $_GET['pttInvert'];
        $query = "UPDATE dvmodem.config SET value = '".$value."' WHERE modem_name = '".$modem_name."' ";
        $query .= "AND protocol = 'config' AND parameter = 'pttInvert'";
        if (mysqli_query($mysql, $query) == false)
        {
            mysqli_close($mysql);
            exit("failed pttinvert");
        }
    }

    if (isset($_GET['useCOSAsLockout']))
    {
        $value = $_GET['useCOSAsLockout'];
        $query = "UPDATE dvmodem.config SET value = '".$value."' WHERE modem_name = '".$modem_name."' ";
        $query .= "AND protocol = 'config' AND parameter = 'useCOSAsLockout'";
        if (mysqli_query($mysql, $query) == false)
        {
            mysqli_close($mysql);
            exit("failed useCOSAsLockout");
        }
    }

    if (isset($_GET['trace']))
    {
        $value = $_GET['trace'];
        $query = "UPDATE dvmodem.config SET value = '".$value."' WHERE modem_name = '".$modem_name."' ";
        $query .= "AND protocol = 'config' AND parameter = 'trace'";
        if (mysqli_query($mysql, $query) == false)
        {
            mysqli_close($mysql);
            exit("failed trace");
        }
    }

    if (isset($_GET['debug']))
    {
        $value = $_GET['debug'];
        $query = "UPDATE dvmodem.config SET value = '".$value."' WHERE modem_name = '".$modem_name."' ";
        $query .= "AND protocol = 'config' AND parameter = 'debug'";
        if (mysqli_query($mysql, $query) == false)
        {
            mysqli_close($mysql);
            exit("failed debug");
        }
    }

    if (isset($_GET['txDelay']))
    {
        $value = $_GET['txDelay'];
        $query = "UPDATE dvmodem.config SET value = '".$value."' WHERE modem_name = '".$modem_name."' ";
        $query .= "AND protocol = 'config' AND parameter = 'txDelay'";
        if (mysqli_query($mysql, $query) == false)
        {
            mysqli_close($mysql);
            exit("failed txDelay");
        }
    }

    if (isset($_GET['rxLevel']))
    {
        $value = $_GET['rxLevel'];
        $query = "UPDATE dvmodem.config SET value = '".$value."' WHERE modem_name = '".$modem_name."' ";
        $query .= "AND protocol = 'config' AND parameter = 'rxLevel'";
        if (mysqli_query($mysql, $query) == false)
        {
            mysqli_close($mysql);
            exit("failed rxLevel");
        }
    }

    if (isset($_GET['rfTXLevel']))
    {
        $value = $_GET['rfTXLevel'];
        $query = "UPDATE dvmodem.config SET value = '".$value."' WHERE modem_name = '".$modem_name."' ";
        $query .= "AND protocol = 'config' AND parameter = 'rfTXLevel'";
        if (mysqli_query($mysql, $query) == false)
        {
            mysqli_close($mysql);
            exit("failed rfTXLevel");
        }
    }

    if (isset($_GET['rxFrequency']))
    {
        $value = $_GET['rxFrequency'];
        $query = "UPDATE dvmodem.config SET value = '".$value."' WHERE modem_name = '".$modem_name."' ";
        $query .= "AND protocol = 'config' AND parameter = 'rxFrequency'";
        if (mysqli_query($mysql, $query) == false)
        {
            mysqli_close($mysql);
            exit("failed rxFrequency");
        }
    }

    if (isset($_GET['txFrequency']))
    {
        $value = $_GET['txFrequency'];
        $query = "UPDATE dvmodem.config SET value = '".$value."' WHERE modem_name = '".$modem_name."' ";
        $query .= "AND protocol = 'config' AND parameter = 'txFrequency'";
        if (mysqli_query($mysql, $query) == false)
        {
            mysqli_close($mysql);
            exit("failed txFrequency");
        }
    }

    if (isset($_GET['rxDCOffset']))
    {
        $value = $_GET['rxDCOffset'];
        $query = "UPDATE dvmodem.config SET value = '".$value."' WHERE modem_name = '".$modem_name."' ";
        $query .= "AND protocol = 'config' AND parameter = 'rxDCOffset'";
        if (mysqli_query($mysql, $query) == false)
        {
            mysqli_close($mysql);
            exit("failed rxDCOffset");
        }
    }

    if (isset($_GET['txDCOffset']))
    {
        $value = $_GET['txDCOffset'];
        $query = "UPDATE dvmodem.config SET value = '".$value."' WHERE modem_name = '".$modem_name."' ";
        $query .= "AND protocol = 'config' AND parameter = 'txDCOffset'";
        if (mysqli_query($mysql, $query) == false)
        {
            mysqli_close($mysql);
            exit("failed txDCOffset");
        }
    }

    if (isset($_GET['modem']))
    {
        $value = htmlspecialchars($_GET['modem']);
        $query = "UPDATE dvmodem.config SET value = '".$value."' WHERE modem_name = '".$modem_name."' ";
        $query .= "AND protocol = 'config' AND parameter = 'modem'";
        if (mysqli_query($mysql, $query) == false)
        {
            mysqli_close($mysql);
            exit("failed modem");
        }
    }

    if (isset($_GET['baud']))
    {
        $value = $_GET['baud'];
        $query = "UPDATE dvmodem.config SET value = '".$value."' WHERE modem_name = '".$modem_name."' ";
        $query .= "AND protocol = 'config' AND parameter = 'baud'";
        if (mysqli_query($mysql, $query) == false)
        {
            mysqli_close($mysql);
            exit("failed baud");
        }
    }

    if (isset($_GET['port']))
    {
        $value = htmlspecialchars($_GET['port']);
        $query = "UPDATE dvmodem.config SET value = '".$value."' WHERE modem_name = '".$modem_name."' ";
        $query .= "AND protocol = 'config' AND parameter = 'port'";
        if (mysqli_query($mysql, $query) == false)
        {
            mysqli_close($mysql);
            exit("failed port");
        }
    }

    if (isset($_GET['mode']))
    {
        $value = $_GET['mode'];
        $query = "UPDATE dvmodem.config SET value = '".$value."' WHERE modem_name = '".$modem_name."' ";
        $query .= "AND protocol = 'config' AND parameter = 'mode'";
        if (mysqli_query($mysql, $query) == false)
        {
            mysqli_close($mysql);
            exit("failed mode");
        }
    }

    echo "success";
}

if ($command == "getStatus")
{
    global $mysql;

    $data = "";
    $id = 0;
    $result = mysqli_query($mysql, "SELECT * FROM modem_host.last_call");
    while ($row = mysqli_fetch_row($result))
    {
        if ($data != "") $data .= "\x1E";
        if ($row[6] == 1)
            $tx = "on";
        else
            $tx = "off";
        $data .= "Id\x1D".$row[0]."\x1Ctx_state\x1D".$tx."\x1Cmode\x1D".$row[1]."\x1Ctype\x1D".$row[2]."\x1CsrcCall\x1D".$row[3]."\x1CdstCall\x1D".$row[4]."\x1CmetaText\x1D".$row[5];
        $id = $row[0];
    }

    $result = mysqli_query($mysql, "SELECT * FROM modem_host.sms_messages WHERE id = ".$id);
    while ($row = mysqli_fetch_row($result))
    {
        $data .= "\x1CsmsText\x1D".$row[2];
    }

    $result = mysqli_query($mysql, "SELECT * FROM modem_host.gps WHERE id = ".$id);
    while ($row = mysqli_fetch_row($result))
    {
        $data .= "\x1Clatitude\x1D".$row[1];
        $data .= "\x1Clongitude\x1D".$row[2];
        $data .= "\x1Caltitude\x1D".$row[3];
        $data .= "\x1Cbearing\x1D".$row[4];
        $data .= "\x1Cspeed\x1D".$row[5];
    }

    if ($data != "")
        echo htmlentities($data);
    else
        echo "none";
}

if ($command == "getHistory")
{
    global $mysql;

    $data = "";
    $result = mysqli_query($mysql, "SELECT MAX(id) AS id FROM modem_host.history GROUP BY Source, mode ORDER BY id DESC LIMIT 25");
    while ($row1 = mysqli_fetch_row($result))
    {
        $result2 = mysqli_query($mysql, "SELECT id, type, mode, Source, Destination, Loss_BER, ss_message, MAX(DATE_FORMAT(datetime, '%Y-%m-%d  %H:%i')) AS datetime, duration FROM modem_host.history WHERE id = ".$row1[0]);
        while ($row = mysqli_fetch_row($result2))
        {
            if ($data != "") $data .= "\x1E";
            $data .= "type\x1D".$row[1]."\x1Cmode\x1D".$row[2]."\x1Csource\x1D".$row[3]."\x1Cdest\x1D".$row[4]."\x1Closs_ber\x1D".$row[5]."\x1Css_message\x1D".$row[6];
            $data .= "\x1Cdatetime\x1D".$row[7]."\x1Cduration\x1D".$row[8];
        }
        mysqli_free_result($result2);
    }
    if ($data != "")
        echo htmlentities($data);
    else
        echo "none";
}

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
            $data .= "\x1Cname\x1D".$row[7]."\x1Ccountry\x1D".$row[8];
        }
        if ($data != "")
            echo htmlentities($data);
        else
            echo "none";
    }
    else
        echo "failed";
}

if ($command == "getLinkedReflectors")
{
    global $mysql;

    $data = "";
    $result = mysqli_query($mysql, "SELECT refl_type, Nick, status FROM modem_host.reflectors WHERE status != 'Unlinked' ORDER BY refl_type, Nick");
    while ($row = mysqli_fetch_row($result))
    {
        if ($data != "") $data .= "\x1E";
        $data .= "refl_type\x1D".$row[0]."\x1Cnick\x1D".$row[1]."\x1Cmodule\x1D".$row[2];
    }
    if ($data != "")
        echo htmlentities($data);
    else
        echo "none";
}

if ($command == "dashbCom")
{
    global $mysql;
    $name = "";
    $mod = "";
    $action = "";
    $parameter = "na";

    if (isset($_GET['action']))
    {
        $action = htmlspecialchars($_GET['action']);
        if (isset($_GET['parameter']))
            $parameter = htmlspecialchars($_GET['parameter']);

        mysqli_query($mysql, "INSERT INTO modem_host.dashb_commands SET command = '".$action."', parameter = '".$parameter."'");

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

mysqli_close($mysql);
?>
