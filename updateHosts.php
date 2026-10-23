<?PHP
/***************************************************************************
 *   Created by Rick KD0OSS                                                *
 *                                                                         *
 *   The host list update script now lives in dashboard/www so that it is  *
 *   installed with the dashboard, which can run it from the Services tab. *
 *   This file keeps the old command working:                              *
 *       php -q updateHosts.php [p25|nxdn|ysf|m17|dstar|ids|all]           *
 ***************************************************************************/

require __DIR__ . '/dashboard/www/updateHosts.php';
?>
