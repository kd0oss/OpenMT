<?php
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
 * Backup and restore of the configuration.  data_query.php (the dashboard's Manage Data tab) and
 * tools/config_backup/openmt_config.php (command line) both use these functions.
 *
 * The backup is a JSON file with
 *   - every row of modem_host.config (the settings of the modem, the modes, the gateways and the dashboard), and
 *   - the saved option sets of the Firmware tab (modem_host.firmware_presets).
 * It holds nothing else: no call history, no lists of reflectors or user ids, and not the dashboard password.
 * The file does contain the passwords that are settings (for example the BrandMeister password).
 *
 * A restore only adds the settings that are missing and changes the ones that differ; it never deletes a
 * setting.  A file is checked completely before anything is written.
 */

const CB_FORMAT   = 'openmt-config';
const CB_VERSION  = 1;
const CB_MAX_SIZE = 2097152;     /* bytes of a backup file */

/*
 * cbExport()
 *
 * Builds the backup of the configuration.
 *
 * Parameters:
 *   $mysql - Open connection.
 *
 * Returns: the file as JSON text.
 */
function cbExport($mysql)
{
    $config = array();
    $res = mysqli_query($mysql, "SELECT modem_name, module, parameter, display_type, value FROM modem_host.config ORDER BY module, parameter");
    while ($row = mysqli_fetch_assoc($res))
        $config[] = array('modem_name' => (string)$row['modem_name'], 'module' => (string)$row['module'],
                          'parameter' => (string)$row['parameter'], 'display_type' => (string)$row['display_type'],
                          'value' => (string)$row['value']);

    $presets = array();
    try
    {
        $res = mysqli_query($mysql, "SELECT firmware, name, options FROM modem_host.firmware_presets ORDER BY firmware, name");
        while ($row = mysqli_fetch_assoc($res))
            $presets[] = array('firmware' => (string)$row['firmware'], 'name' => (string)$row['name'], 'options' => (string)$row['options']);
    }
    catch (Throwable $e)
    {
        /* the table is not there: no saved sets */
    }

    $file = array('format' => CB_FORMAT, 'version' => CB_VERSION, 'created' => gmdate('Y-m-d\TH:i:s\Z'),
                  'host' => (string)gethostname(), 'config' => $config, 'firmware_presets' => $presets);
    return json_encode($file, JSON_PRETTY_PRINT | JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE) . "\n";
}

/*
 * cbName()
 *
 * Returns: true when $text is a string made of the characters allowed for a name (letters, digits, _) with the
 *   length 1 to $max.
 */
function cbName($text, $max)
{
    return is_string($text) && preg_match('/^[A-Za-z0-9_]{1,' . (int)$max . '}$/', $text) === 1;
}

/*
 * cbCheck()
 *
 * Reads and checks a backup file.
 *
 * Parameters:
 *   $text - The text of the file.
 *
 * Returns: array('ok' => true, 'config' => rows, 'presets' => rows) or array('ok' => false, 'error' => text).
 */
function cbCheck($text)
{
    if (!is_string($text) || $text === '')
        return array('ok' => false, 'error' => 'The file is empty.');
    if (strlen($text) > CB_MAX_SIZE)
        return array('ok' => false, 'error' => 'The file is too large for a configuration backup.');

    $data = json_decode($text, true);
    if (!is_array($data) || ($data['format'] ?? '') !== CB_FORMAT)
        return array('ok' => false, 'error' => 'This is not an OpenMT configuration backup.');
    if ((int)($data['version'] ?? 0) !== CB_VERSION)
        return array('ok' => false, 'error' => 'The backup was made by a different version of the format (' . (int)($data['version'] ?? 0) . ').');
    if (!isset($data['config']) || !is_array($data['config']))
        return array('ok' => false, 'error' => 'The backup holds no settings.');

    $config = array();
    foreach ($data['config'] as $i => $row)
    {
        $n = $i + 1;
        if (!is_array($row) || !cbName($row['module'] ?? null, 30) || !cbName($row['parameter'] ?? null, 30))
            return array('ok' => false, 'error' => "Setting $n has no valid module or parameter.");
        $modem = $row['modem_name'] ?? 'modem1';
        $disp  = $row['display_type'] ?? 'none';
        $value = $row['value'] ?? '';
        if (!is_string($modem) || preg_match('/^[A-Za-z0-9_]{0,10}$/', $modem) !== 1)
            return array('ok' => false, 'error' => "Setting $n ({$row['module']}/{$row['parameter']}) has an invalid modem name.");
        if (!is_string($disp) || preg_match('/^[A-Za-z0-9_]{0,25}$/', $disp) !== 1)
            return array('ok' => false, 'error' => "Setting $n ({$row['module']}/{$row['parameter']}) has an invalid display type.");
        if (!is_string($value) || strlen($value) > 255 || strpos($value, "\0") !== false)
            return array('ok' => false, 'error' => "Setting $n ({$row['module']}/{$row['parameter']}) has an invalid value (a text of at most 255 characters).");
        $config[] = array('modem_name' => $modem, 'module' => $row['module'], 'parameter' => $row['parameter'],
                          'display_type' => $disp, 'value' => $value);
    }

    $presets = array();
    foreach (($data['firmware_presets'] ?? array()) as $i => $row)
    {
        $n = $i + 1;
        if (!is_array($row) || !cbName($row['firmware'] ?? null, 20) ||
            !is_string($row['name'] ?? null) || preg_match('/^[A-Za-z0-9 ._()+-]{1,40}$/', $row['name']) !== 1)
            return array('ok' => false, 'error' => "Saved firmware set $n has an invalid firmware or name.");
        $opt = $row['options'] ?? '';
        if (!is_string($opt) || strlen($opt) > 8000 || !is_array(json_decode($opt, true)))
            return array('ok' => false, 'error' => "Saved firmware set $n ({$row['name']}) has invalid options.");
        $presets[] = array('firmware' => $row['firmware'], 'name' => $row['name'], 'options' => $opt);
    }

    return array('ok' => true, 'config' => $config, 'presets' => $presets,
                 'created' => (string)($data['created'] ?? ''), 'host' => (string)($data['host'] ?? ''));
}

/*
 * cbShow()
 *
 * Returns: the value for a listing; the value of a setting that holds a password is hidden.
 */
function cbShow($parameter, $value)
{
    if (preg_match('/pass|pwd|secret|token|key/i', $parameter) === 1 && $value !== '')
        return '********';
    return $value;
}

/*
 * cbRestore()
 *
 * Compares a checked backup with the database and, when $apply is true, writes the differences.
 *
 * Parameters:
 *   $mysql - Open connection.
 *   $check - The result of cbCheck().
 *   $apply - false to only count, true to write.
 *
 * Returns: array with the counts and the first changes:
 *   config  => array(add, change, same), presets => array(add, change, same), changes => list of changes.
 */
function cbRestore($mysql, $check, $apply)
{
    $sum = array('config' => array('add' => 0, 'change' => 0, 'same' => 0),
                 'presets' => array('add' => 0, 'change' => 0, 'same' => 0), 'changes' => array(), 'applied' => $apply);

    $sel = mysqli_prepare($mysql, "SELECT modem_name, display_type, value FROM modem_host.config WHERE module = ? AND parameter = ?");
    $ins = mysqli_prepare($mysql, "INSERT INTO modem_host.config (modem_name, module, parameter, display_type, value) VALUES (?, ?, ?, ?, ?)");
    $upd = mysqli_prepare($mysql, "UPDATE modem_host.config SET modem_name = ?, display_type = ?, value = ? WHERE module = ? AND parameter = ?");

    foreach ($check['config'] as $row)
    {
        mysqli_stmt_bind_param($sel, "ss", $row['module'], $row['parameter']);
        mysqli_stmt_execute($sel);
        mysqli_stmt_bind_result($sel, $oModem, $oDisp, $oValue);
        $found = mysqli_stmt_fetch($sel);
        mysqli_stmt_free_result($sel);
        $name = $row['module'] . '/' . $row['parameter'];

        if (!$found)
        {
            $sum['config']['add']++;
            if (count($sum['changes']) < 60)
                $sum['changes'][] = array('name' => $name, 'old' => null, 'new' => cbShow($row['parameter'], $row['value']));
            if ($apply)
            {
                mysqli_stmt_bind_param($ins, "sssss", $row['modem_name'], $row['module'], $row['parameter'], $row['display_type'], $row['value']);
                mysqli_stmt_execute($ins);
            }
        }
        else if ($oModem !== $row['modem_name'] || $oDisp !== $row['display_type'] || $oValue !== $row['value'])
        {
            $sum['config']['change']++;
            if (count($sum['changes']) < 60)
                $sum['changes'][] = array('name' => $name, 'old' => cbShow($row['parameter'], (string)$oValue), 'new' => cbShow($row['parameter'], $row['value']));
            if ($apply)
            {
                mysqli_stmt_bind_param($upd, "sssss", $row['modem_name'], $row['display_type'], $row['value'], $row['module'], $row['parameter']);
                mysqli_stmt_execute($upd);
            }
        }
        else
            $sum['config']['same']++;
    }
    mysqli_stmt_close($sel);
    mysqli_stmt_close($ins);
    mysqli_stmt_close($upd);

    if (count($check['presets']) > 0)
    {
        try
        {
            $psel = mysqli_prepare($mysql, "SELECT options FROM modem_host.firmware_presets WHERE firmware = ? AND name = ?");
            $pup  = mysqli_prepare($mysql, "INSERT INTO modem_host.firmware_presets (firmware, name, options, updated) VALUES (?, ?, ?, NOW()) ON DUPLICATE KEY UPDATE options = VALUES(options), updated = NOW()");
            foreach ($check['presets'] as $row)
            {
                mysqli_stmt_bind_param($psel, "ss", $row['firmware'], $row['name']);
                mysqli_stmt_execute($psel);
                mysqli_stmt_bind_result($psel, $oOpt);
                $found = mysqli_stmt_fetch($psel);
                mysqli_stmt_free_result($psel);
                if (!$found)
                    $sum['presets']['add']++;
                else if ($oOpt !== $row['options'])
                    $sum['presets']['change']++;
                else
                {
                    $sum['presets']['same']++;
                    continue;
                }
                if ($apply)
                {
                    mysqli_stmt_bind_param($pup, "sss", $row['firmware'], $row['name'], $row['options']);
                    mysqli_stmt_execute($pup);
                }
            }
            mysqli_stmt_close($psel);
            mysqli_stmt_close($pup);
        }
        catch (Throwable $e)
        {
            $sum['presetError'] = 'The saved firmware sets were skipped: the table is missing (run db/openmt.sql).';
        }
    }

    return $sum;
}
?>
