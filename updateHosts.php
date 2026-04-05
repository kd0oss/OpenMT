<?PHP
/***************************************************************************
 *   Copyright (C) 2026 by Rick KD0OSS             *
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
 * Based on the update script by Tony Corbett G0WFV and Andy Taylor MW0MWZ *
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

function getHostFile($file)
{
    $user_agent = "Pi-Star_20260101";

    // Initialize a cURL session
    $ch = curl_init();

    // Set the URL for the cURL transfer
    curl_setopt($ch, CURLOPT_URL, $file);

    // Set the User-Agent header field
    curl_setopt($ch, CURLOPT_USERAGENT, $user_agent);

    // Set CURLOPT_RETURNTRANSFER to true to return the response as a string instead of outputting it directly
    curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);

    // Optional: follow redirects
    curl_setopt($ch, CURLOPT_FOLLOWLOCATION, true);

    // Execute the cURL request
    $response = curl_exec($ch);

    // Check for errors
    if ($response === false)
    {
        echo 'cURL error: ' . curl_error($ch);
        return "error";
    }

    // Close the cURL session to free up resources
    curl_close($ch);
    return $response;
}

function updateP25Hosts()
{
    global $mysql;

    $file = "http://www.pistar.uk/downloads/P25_Hosts.txt";
    $hosts = getHostFile($file);
    if ($hosts != "error")
    {
        mysqli_query($mysql, "DELETE FROM modem_host.reflectors WHERE refl_type = 'P25' AND status = 'Unlinked'");
        $lines = explode("\n", $hosts);
        for ($x = 0; $x < count($lines); $x++)
        {
            if (strlen($lines[$x]) > 6 && $lines[$x][0] != '#')
            {
                $fields = explode("\t", $lines[$x]);
                $query = "SELECT * FROM modem_host.reflectors WHERE refl_type = 'P25' AND Nick = '".$fields[0]."'";
                $result = mysqli_query($mysql, $query);
                if (mysqli_num_rows($result) > 0)
                    $query = "UPDATE modem_host.reflectors SET ip4 = '".$fields[1]."', port = ".$fields[2]." WHERE Nick = '".$fields[0]."' AND refl_type = 'P25'";
                else
                    $query = "INSERT INTO modem_host.reflectors SET Nick = '".$fields[0]."', ip4 = '".$fields[1]."', port = ".$fields[2].", refl_type = 'P25'";
                mysqli_free_result($result);
//                echo $query."\n";
                mysqli_query($mysql, $query);
            }
        }
    }
}

function updateDSTARHosts()
{
    global $mysql;

    $file = "http://www.pistar.uk/downloads/DPlus_WithXRF_Hosts.txt";
    $hosts = getHostFile($file);
    if ($hosts != "error")
    {
        mysqli_query($mysql, "DELETE FROM modem_host.reflectors WHERE refl_type = 'DSTAR' AND status = 'Unlinked'");
        $lines = explode("\n", $hosts);
        for ($x = 0; $x < count($lines); $x++)
        {
            if (strlen($lines[$x]) > 6 && $lines[$x][0] != '#')
            {
                $fields = explode("\t", $lines[$x]);
                $query = "SELECT * FROM modem_host.reflectors WHERE refl_type = 'DSTAR' AND Nick = '".$fields[0]."'";
                $result = mysqli_query($mysql, $query);
                if (mysqli_num_rows($result) > 0)
                    $query = "UPDATE modem_host.reflectors SET ip4 = '".$fields[1]."', port = 0 WHERE Nick = '".$fields[0]."' AND refl_type = 'DSTAR'";
                else
                    $query = "INSERT INTO modem_host.reflectors SET Nick = '".$fields[0]."', ip4 = '".$fields[1]."', port = 0, refl_type = 'DSTAR', has_modules = 'yes'";
                mysqli_free_result($result);
          //      echo $query."\n";
                mysqli_query($mysql, $query);
            }
        }
    }

    $file = "http://www.pistar.uk/downloads/DExtra_NoXRF_Hosts.txt";
    $hosts = getHostFile($file);
    if ($hosts != "error")
    {
        $lines = explode("\n", $hosts);
        for ($x = 0; $x < count($lines); $x++)
        {
            if (strlen($lines[$x]) > 6 && $lines[$x][0] != '#')
            {
                $fields = explode("\t", $lines[$x]);
                $query = "SELECT * FROM modem_host.reflectors WHERE refl_type = 'DSTAR' AND Nick = '".$fields[0]."'";
                $result = mysqli_query($mysql, $query);
                if (mysqli_num_rows($result) > 0)
                    $query = "UPDATE modem_host.reflectors SET ip4 = '".$fields[1]."', port = 0 WHERE Nick = '".$fields[0]."' AND refl_type = 'DSTAR'";
                else
                    $query = "INSERT INTO modem_host.reflectors SET Nick = '".$fields[0]."', ip4 = '".$fields[1]."', port = 0, refl_type = 'DSTAR', has_modules = 'yes'";
                mysqli_free_result($result);
        //        echo $query."\n";
                mysqli_query($mysql, $query);
            }
        }
    }

    $file = "http://www.pistar.uk/downloads/DCS_Hosts.txt";
    $hosts = getHostFile($file);
    if ($hosts != "error")
    {
        $lines = explode("\n", $hosts);
        for ($x = 0; $x < count($lines); $x++)
        {
            if (strlen($lines[$x]) > 6 && $lines[$x][0] != '#')
            {
                $fields = explode("\t", $lines[$x]);
                $query = "SELECT * FROM modem_host.reflectors WHERE refl_type = 'DSTAR' AND Nick = '".$fields[0]."'";
                $result = mysqli_query($mysql, $query);
                if (mysqli_num_rows($result) > 0)
                    $query = "UPDATE modem_host.reflectors SET ip4 = '".$fields[1]."', port = 0 WHERE Nick = '".$fields[0]."' AND refl_type = 'DSTAR'";
                else
                    $query = "INSERT INTO modem_host.reflectors SET Nick = '".$fields[0]."', ip4 = '".$fields[1]."', port = 0, refl_type = 'DSTAR', has_modules = 'yes'";
                mysqli_free_result($result);
      //          echo $query."\n";
                mysqli_query($mysql, $query);
            }
        }
    }
}

function updateM17Hosts()
{
    global $mysql;

    $file = "http://www.pistar.uk/downloads/M17_Hosts.txt";
    $hosts = getHostFile($file);
    if ($hosts != "error")
    {
        mysqli_query($mysql, "DELETE FROM modem_host.reflectors WHERE refl_type = 'M17' AND status = 'Unlinked'");
        $lines = explode("\n", $hosts);
        for ($x = 0; $x < count($lines); $x++)
        {
            if (strlen($lines[$x]) > 6 && $lines[$x][0] != '#')
            {
                $fields = explode("\t", $lines[$x]);
                $query = "SELECT * FROM modem_host.reflectors WHERE refl_type = 'M17' AND Nick = '".$fields[0]."'";
                $result = mysqli_query($mysql, $query);
                if (mysqli_num_rows($result) > 0)
                    $query = "UPDATE modem_host.reflectors SET ip4 = '".$fields[1]."', port = ".$fields[2]." WHERE Nick = '".$fields[0]."' AND refl_type = 'M17'";
                else
                    $query = "INSERT INTO modem_host.reflectors SET Nick = '".$fields[0]."', ip4 = '".$fields[1]."', port = ".$fields[2].", refl_type = 'M17', has_modules = 'yes'";
                mysqli_free_result($result);
              //  echo $query."\n";
                mysqli_query($mysql, $query);
            }
        }
    }
}

function updateDMRHosts()
{
    global $mysql;

    $file = "http://www.pistar.uk/downloads/DMR_Hosts.txt";
    $hosts = getHostFile($file);
    if ($hosts != "error")
    {
        echo $hosts;
        return;
        mysqli_query($mysql, "DELETE FROM modem_host.reflectors WHERE refl_type = 'DMR' AND status = 'Unlinked'");
        $lines = explode("\n", $hosts);
        for ($x = 0; $x < count($lines); $x++)
        {
            if (strlen($lines[$x]) > 6 && $lines[$x][0] != '#')
            {
                $fields = explode("\t", $lines[$x]);
                $query = "SELECT * FROM modem_host.reflectors WHERE refl_type = 'DMR' AND Nick = '".$fields[0]."'";
                $result = mysqli_query($mysql, $query);
                if (mysqli_num_rows($result) > 0)
                    $query = "UPDATE modem_host.reflectors SET ip4 = '".$fields[1]."', port = ".$fields[2]." WHERE Nick = '".$fields[0]."' AND refl_type = 'DMR'";
                else
                    $query = "INSERT INTO modem_host.reflectors SET Nick = '".$fields[0]."', ip4 = '".$fields[1]."', port = ".$fields[2].", refl_type = 'DMR'";
                mysqli_free_result($result);
              //  echo $query."\n";
                mysqli_query($mysql, $query);
            }
        }
    }
}

function updateDMRIds()
{
    global $mysql;

    exec("curl -sSL http://www.pistar.uk/downloads/DMRIds.dat.gz  --user-agent Pi-Star_Pi-Star_20260101 | gunzip -c > DMRIds.dat", $out, $ret);
echo $out."   ".$ret."\n";
//    if ($hosts != "error")
    {
        mysqli_query($mysql, "TRUNCATE modem_host.dmr_ids");
        $lines = file("DMRIds.dat", FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES);
        for ($x = 0; $x < count($lines); $x++)
        {
            if (strlen($lines[$x]) > 6 && $lines[$x][0] != '#')
            {
                $fields = explode("\t", $lines[$x]);
                $query = "INSERT INTO modem_host.dmr_ids SET DMR_Id = '".$fields[0]."', Callsign = '".$fields[1]."', Name = '".$fields[2]."'";
                mysqli_query($mysql, $query);
                echo ".";
            }
        }
    }
}

echo "Updating P25 Hosts.\n";
updateP25Hosts();
echo "Updating M17 Hosts.\n";
updateM17Hosts();
echo "Updating DSTAR Hosts.\n";
updateDSTARHosts();
echo "Updating DMR Id's.\n";
updateDMRIds();
echo "\nUpdates complete.\n";

mysqli_close($mysql);
?>
