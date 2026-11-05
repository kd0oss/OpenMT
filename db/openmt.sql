/*M!999999\- enable the sandbox mode */ 
-- MariaDB dump 10.19-11.8.3-MariaDB, for debian-linux-gnueabihf (armv8l)
--
-- Host: localhost    Database: modem_host
-- ------------------------------------------------------
-- Server version	11.8.3-MariaDB-0+deb13u1+rpi1 from Raspbian

/*!40101 SET @OLD_CHARACTER_SET_CLIENT=@@CHARACTER_SET_CLIENT */;
/*!40101 SET @OLD_CHARACTER_SET_RESULTS=@@CHARACTER_SET_RESULTS */;
/*!40101 SET @OLD_COLLATION_CONNECTION=@@COLLATION_CONNECTION */;
/*!40101 SET NAMES utf8mb4 */;
/*!40103 SET @OLD_TIME_ZONE=@@TIME_ZONE */;
/*!40103 SET TIME_ZONE='+00:00' */;
/*!40014 SET @OLD_UNIQUE_CHECKS=@@UNIQUE_CHECKS, UNIQUE_CHECKS=0 */;
/*!40014 SET @OLD_FOREIGN_KEY_CHECKS=@@FOREIGN_KEY_CHECKS, FOREIGN_KEY_CHECKS=0 */;
/*!40101 SET @OLD_SQL_MODE=@@SQL_MODE, SQL_MODE='NO_AUTO_VALUE_ON_ZERO' */;
/*M!100616 SET @OLD_NOTE_VERBOSITY=@@NOTE_VERBOSITY, NOTE_VERBOSITY=0 */;

-- ---------------------------------------------------------------------------------------------------------------------
-- OpenMT database: every table of the project in this one file.
-- Created by Rick KD0OSS
--
-- Install or update:   mysql -u openmt -p modem_host < db/openmt.sql
-- The file can be run again on an existing database: a table is only created when it is missing, the starting rows
-- are only added when they are missing, and nothing that is saved (settings, the dashboard password, the lists)
-- is changed or removed.  So when a new release adds a table, run this file again.
--
--   dashboard and modem host:  auth, config, dashb_commands, dmr_ids, errors, gps, history, host_status, last_call,
--                              reflectors, sms_messages
--   service control:           services, service_commands, system_actions
--   host list updates:         host_update_sections, host_update_runs
--   firmware tab:              firmware_runs, firmware_files, firmware_presets, firmware_flash_runs, firmware_flasher
--
-- The only other file is db/modem_display_type.sql, a one-time change for a database made before the Modem tab
-- used display_type; a new database does not need it.
-- ---------------------------------------------------------------------------------------------------------------------

USE modem_host;

--
-- Table structure for table `auth`
--

/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE IF NOT EXISTS `auth` (
  `username` varchar(10) NOT NULL,
  `password` varchar(50) NOT NULL
) ENGINE=MyISAM DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci COMMENT='Dashboard authorization';
/*!40101 SET character_set_client = @saved_cs_client */;

--
-- Table structure for table `config`
--
-- The first user, only when there is none yet (a saved password is never replaced).
INSERT INTO `auth` (`username`, `password`) SELECT 'admin', PASSWORD('admin') FROM DUAL
 WHERE NOT EXISTS (SELECT 1 FROM `auth` WHERE `username` = 'admin');

/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE IF NOT EXISTS `config` (
  `id` int(11) NOT NULL AUTO_INCREMENT,
  `modem_name` varchar(10) DEFAULT 'modem1',
  `module` varchar(30) NOT NULL DEFAULT 'main',
  `parameter` varchar(30) NOT NULL,
  `display_type` varchar(25) NOT NULL DEFAULT 'none',
  `value` varchar(255) NOT NULL,
  PRIMARY KEY (`id`),
  UNIQUE KEY `module_params` (`module`,`parameter`) USING BTREE
) ENGINE=MyISAM DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci COMMENT='Modem host configuration';
/*!40101 SET character_set_client = @saved_cs_client */;

--
-- Adding data for table `config`
--

SET @OLD_AUTOCOMMIT=@@AUTOCOMMIT, @@AUTOCOMMIT=0;
LOCK TABLES `config` WRITE;
/*!40000 ALTER TABLE `config` DISABLE KEYS */;
INSERT IGNORE INTO `config` VALUES
(1,'modem1','main','gateways','none',''),
(2,'modem1','main','activeModes','none',''),
(3,'modem1','main','username','none','admin'),
(4,'modem1','main','callsign','none','N0CALL'),
(5,'modem1','main','title','none','Repeater Dashboard'),
(6,'modem1','config','port','none','ttyAMA0'),
(7,'modem1','config','mode','none','duplex'),
(8,'modem1','config','modem','none','openmt'),
(9,'modem1','config','rxInvert','input','false'),
(10,'modem1','config','txInvert','input','true'),
(11,'modem1','config','pttInvert','input','false'),
(12,'modem1','config','useCOSAsLockout','input','false'),
(13,'modem1','config','debug','input','false'),
(14,'modem1','config','txDelay','input','100'),
(15,'modem1','config','rxLevel','input','50'),
(16,'modem1','config','rfTXLevel','input','50'),
(17,'modem1','config','rxFrequency','input','449900000'),
(18,'modem1','config','txFrequency','input','444900000'),
(19,'modem1','config','rxDCOffset','input','0'),
(20,'modem1','config','txDCOffset','input','0'),
(21,'modem1','config','baud','none','921600'),
(22,'modem1','config','fmTxLevel','input','50'),
(23,'modem1','config','cwIdTxLevel','input','50'),
(24,'modem1','main','latitude','none','0.000000'),
(25,'modem1','main','longitude','none','0.000000'),
(26,'modem1','main','description','none',''),
(27,'modem1','main','location','none',''),

/*!40000 ALTER TABLE `config` ENABLE KEYS */;
UNLOCK TABLES;
COMMIT;
SET AUTOCOMMIT=@OLD_AUTOCOMMIT;

--
-- Table structure for table `dashb_commands`
--

/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE IF NOT EXISTS `dashb_commands` (
  `id` int(11) NOT NULL AUTO_INCREMENT,
  `modem_name` varchar(10) DEFAULT 'modem1',
  `command` varchar(20) NOT NULL,
  `parameter` varchar(40) NOT NULL,
  `result` varchar(80) DEFAULT NULL,
  PRIMARY KEY (`id`)
) ENGINE=MyISAM DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci COMMENT='Dashboard commands for host program';
/*!40101 SET character_set_client = @saved_cs_client */;

--
-- Table structure for table `dmr_ids`
--

/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE IF NOT EXISTS `dmr_ids` (
  `id` int(11) NOT NULL AUTO_INCREMENT,
  `DMR_Id` int(11) DEFAULT NULL,
  `Callsign` varchar(10) DEFAULT NULL,
  `Name` varchar(40) DEFAULT NULL,
  PRIMARY KEY (`id`),
  KEY `idx_dmr_id` (`DMR_Id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_uca1400_ai_ci;
/*!40101 SET character_set_client = @saved_cs_client */;

--
-- Table structure for table `errors`
--

/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE IF NOT EXISTS `errors` (
  `id` int(11) NOT NULL AUTO_INCREMENT,
  `modem_name` varchar(10) DEFAULT 'modem1',
  `module` varchar(30) NOT NULL,
  `message` varchar(255) NOT NULL,
  `datetime` datetime NOT NULL,
  PRIMARY KEY (`id`)
) ENGINE=MyISAM DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci COMMENT='Error messages';
/*!40101 SET character_set_client = @saved_cs_client */;

--
-- Table structure for table `gps`
--

/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE IF NOT EXISTS `gps` (
  `id` int(11) NOT NULL,
  `latitude` double NOT NULL,
  `longitude` double NOT NULL,
  `altitude_meters` smallint(6) NOT NULL DEFAULT 0,
  `bearing` smallint(6) NOT NULL DEFAULT 0,
  `speed_kph` int(11) NOT NULL DEFAULT 0,
  PRIMARY KEY (`id`)
) ENGINE=MyISAM DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci COMMENT='GPS data';
/*!40101 SET character_set_client = @saved_cs_client */;

--
-- Table structure for table `history`
--

/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE IF NOT EXISTS `history` (
  `id` int(11) NOT NULL AUTO_INCREMENT,
  `modem_name` varchar(10) DEFAULT 'modem1',
  `type` enum('RF','NET') NOT NULL DEFAULT 'RF',
  `mode` varchar(15) DEFAULT NULL,
  `Source` varchar(10) NOT NULL,
  `suffix` varchar(10) DEFAULT NULL,
  `Destination` varchar(10) NOT NULL,
  `Loss_BER` float NOT NULL,
  `ss_message` varchar(80) NOT NULL,
  `datetime` datetime NOT NULL,
  `duration` int(11) NOT NULL,
  PRIMARY KEY (`id`)
) ENGINE=MyISAM DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci COMMENT='Communicatons history';
/*!40101 SET character_set_client = @saved_cs_client */;

--
-- Table structure for table `host_status`
--

/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE IF NOT EXISTS `host_status` (
  `id` int(11) NOT NULL AUTO_INCREMENT,
  `modem_name` varchar(10) DEFAULT 'modem1',
  `module` varchar(30) NOT NULL,
  `property` varchar(20) NOT NULL,
  `value` varchar(50) NOT NULL,
  PRIMARY KEY (`id`)
) ENGINE=MyISAM DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci COMMENT='Host program status';
/*!40101 SET character_set_client = @saved_cs_client */;

--
-- Table structure for table `last_call`
--

/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE IF NOT EXISTS `last_call` (
  `id` int(11) NOT NULL AUTO_INCREMENT,
  `pos` tinyint(4) NOT NULL,
  `modem_name` varchar(10) DEFAULT 'modem1',
  `mode` varchar(15) DEFAULT NULL,
  `type` enum('RF','NET') DEFAULT NULL,
  `source` varchar(10) NOT NULL,
  `suffix` varchar(10) DEFAULT NULL,
  `destination` varchar(10) NOT NULL,
  `ss_message` varchar(80) NOT NULL,
  `isTx` tinyint(1) NOT NULL DEFAULT 0,
  PRIMARY KEY (`id`)
) ENGINE=MyISAM DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci COMMENT='Last call info';
/*!40101 SET character_set_client = @saved_cs_client */;

--
-- Table structure for table `reflectors`
--

/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE IF NOT EXISTS `reflectors` (
  `id` int(11) NOT NULL AUTO_INCREMENT,
  `refl_type` varchar(10) NOT NULL,
  `Nick` varchar(7) NOT NULL,
  `ip4` varchar(80) NOT NULL,
  `ip6` varchar(25) NOT NULL DEFAULT 'none',
  `port` mediumint(9) DEFAULT NULL,
  `dash_address` varchar(80) NOT NULL DEFAULT 'none',
  `Name` varchar(50) NOT NULL DEFAULT 'none',
  `Country` varchar(2) NOT NULL DEFAULT 'na',
  `status` varchar(10) NOT NULL DEFAULT 'Unlinked',
  `has_modules` enum('yes','no') DEFAULT 'no',
  PRIMARY KEY (`id`),
  UNIQUE KEY `refl_type` (`refl_type`,`Nick`)
) ENGINE=MyISAM DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci COMMENT='DV Reflectors';
/*!40101 SET character_set_client = @saved_cs_client */;

--
-- Table structure for table `sms_messages`
--

/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE IF NOT EXISTS `sms_messages` (
  `id` int(11) NOT NULL,
  `source` varchar(10) NOT NULL,
  `message` text NOT NULL,
  `datetime` datetime NOT NULL,
  PRIMARY KEY (`id`)
) ENGINE=MyISAM DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci COMMENT='SMS message';
--
-- service control (Settings, Services tab)
--

CREATE TABLE IF NOT EXISTS services (
  unit        varchar(60)  NOT NULL,
  label       varchar(40)  NOT NULL,
  description varchar(120) NOT NULL DEFAULT '',
  sort_order  int(11)      NOT NULL DEFAULT 100,
  active      varchar(20)  NOT NULL DEFAULT 'unknown' COMMENT 'Written by the service manager',
  enabled     varchar(20)  NOT NULL DEFAULT 'unknown' COMMENT 'Written by the service manager',
  updated     datetime     DEFAULT NULL,
  PRIMARY KEY (unit)
) ENGINE=MyISAM DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci COMMENT='Services the dashboard can manage';

CREATE TABLE IF NOT EXISTS service_commands (
  id        int(11)     NOT NULL AUTO_INCREMENT,
  unit      varchar(60) NOT NULL,
  action    varchar(10) NOT NULL,
  requested datetime    NOT NULL DEFAULT CURRENT_TIMESTAMP,
  result    varchar(80) DEFAULT NULL,
  PRIMARY KEY (id)
) ENGINE=MyISAM DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci COMMENT='Requests from the dashboard for the service manager';

INSERT IGNORE INTO services (unit, label, description, sort_order) VALUES
  ('mmdvm-bridge.service','MMDVM bridge',    'MMDVM / MMDVM_HS modem bridge (only when this bridge is used)', 4),
  ('openmths-bridge.service','OpenMT_HS bridge','OpenMT_HS modem bridge (only when this bridge is used)', 4),
  ('openmt-bridge.service','OpenMT bridge','OpenMT sample modem bridge (only when this bridge is used)', 4),
  ('sx1255-bridge.service','SX1255 bridge',   'SX1255 radio bridge (only with the SX1255 modem)', 5),
  ('modem-host.service',   'Modem host',       'Talks to the modem. Every host needs it.',        10),
  ('dstar-host.service',   'D-STAR host',      'D-STAR protocol host',                            20),
  ('dstar-gateway.service','D-STAR gateway',   'D-STAR reflector gateway',                        21),
  ('dmr-host.service',     'DMR host',         'DMR protocol host',                               30),
  ('dmr-gateway.service',  'DMR gateway',      'DMR network gateway',                             31),
  ('p25-host.service',     'P25 host',         'P25 protocol host',                               40),
  ('p25-gateway.service',  'P25 gateway',      'P25 reflector gateway',                           41),
  ('nxdn-host.service',    'NXDN host',        'NXDN protocol host',                              50),
  ('nxdn-gateway.service', 'NXDN gateway',     'NXDN reflector gateway',                          51),
  ('ysf-host.service',     'YSF host',         'System Fusion protocol host',                     60),
  ('ysf-gateway.service',  'YSF gateway',      'System Fusion reflector gateway',                 61),
  ('m17-host.service',     'M17 host',         'M17 protocol host',                               70),
  ('m17-gateway.service',  'M17 gateway',      'M17 reflector gateway',                           71),
  ('fm-host.service',      'FM host',          'FM repeater host',                                80),
  ('fm-gateway.service',   'FM gateway',       'FM gateway (EchoLink)',                           81);

-- Power actions for the Pi itself. The dashboard builds its Restart / Shut down buttons from these rows.
-- The commands they run are fixed in the service manager (systemctl reboot / poweroff); a row only
-- switches an action on and sets its text, so removing a row removes the button and the permission.
CREATE TABLE IF NOT EXISTS system_actions (
  action      varchar(10)  NOT NULL,
  label       varchar(30)  NOT NULL,
  description varchar(120) NOT NULL DEFAULT '',
  confirm     varchar(200) NOT NULL DEFAULT '',
  sort_order  int(11)      NOT NULL DEFAULT 100,
  PRIMARY KEY (action)
) ENGINE=MyISAM DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci COMMENT='Power actions the dashboard can ask for';

INSERT IGNORE INTO system_actions (action, label, description, confirm, sort_order) VALUES
  ('reboot',   'Restart the Pi',   'Restarts the Pi. The dashboard is back in about a minute.',
   'Restart the Pi now?\n\nAll services stop and start again, and you will need to log in again.', 10),
  ('poweroff', 'Shut down the Pi', 'Shuts the Pi down. It must be switched on by hand again.',
   'Shut the Pi down now?\n\nIt will stay off until the power is switched off and on again.', 20);

--
-- host list updates (Settings, Services tab)
--

CREATE TABLE IF NOT EXISTS host_update_sections (
  section     varchar(10)  NOT NULL,
  label       varchar(40)  NOT NULL,
  description varchar(120) NOT NULL DEFAULT '',
  refl_type   varchar(10)  DEFAULT NULL,
  sort_order  int(11)      NOT NULL DEFAULT 100,
  last_run    datetime     DEFAULT NULL COMMENT 'Written by updateHostsRun.php',
  last_result varchar(120) DEFAULT NULL COMMENT 'Written by updateHostsRun.php',
  PRIMARY KEY (section)
) ENGINE=MyISAM DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci COMMENT='Host lists the dashboard can update';

CREATE TABLE IF NOT EXISTS host_update_runs (
  id       int(11)     NOT NULL AUTO_INCREMENT,
  sections varchar(120) NOT NULL,
  state    varchar(10) NOT NULL DEFAULT 'queued' COMMENT 'queued, running, done or failed',
  started  datetime    DEFAULT NULL,
  updated  datetime    DEFAULT NULL COMMENT 'Heartbeat of the running update',
  finished datetime    DEFAULT NULL,
  log      mediumtext  DEFAULT NULL,
  PRIMARY KEY (id)
) ENGINE=MyISAM DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci COMMENT='Updates started from the dashboard';

INSERT IGNORE INTO host_update_sections (section, label, description, refl_type, sort_order) VALUES
  ('p25',   'P25 reflectors',    'Talk groups and addresses of the P25 reflectors', 'P25',   10),
  ('nxdn',  'NXDN reflectors',   'Talk groups and addresses of the NXDN reflectors','NXDN',  20),
  ('ysf',   'YSF reflectors',    'Yaesu System Fusion reflectors',                  'YSF',   30),
  ('m17',   'M17 reflectors',    'M17 reflectors and their modules',                'M17',   40),
  ('dstar', 'D-Star reflectors', 'REF, XRF, DCS and XLX reflectors',                'DSTAR', 50),
  ('ids',   'DMR and NXDN IDs',  'Call sign look-up for the DMR and NXDN user ids', NULL,    60);

--
-- firmware builds (Settings, Firmware tab): the request, state and log of a build, and the result files (.hex and .bin) so the page can offer them for download
--

CREATE TABLE IF NOT EXISTS firmware_runs (
  id       int(11)      NOT NULL AUTO_INCREMENT,
  firmware varchar(20)  NOT NULL COMMENT 'hs or sample',
  target   varchar(10)  NOT NULL DEFAULT '',
  options  text         DEFAULT NULL COMMENT 'The option values of the request, as JSON',
  state    varchar(10)  NOT NULL DEFAULT 'queued' COMMENT 'queued, running, done or failed',
  started  datetime     DEFAULT NULL,
  updated  datetime     DEFAULT NULL COMMENT 'Heartbeat of the running build',
  finished datetime     DEFAULT NULL,
  log      mediumtext   DEFAULT NULL,
  PRIMARY KEY (id)
) ENGINE=MyISAM DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci COMMENT='Firmware builds started from the dashboard';

CREATE TABLE IF NOT EXISTS firmware_files (
  id      int(11)      NOT NULL AUTO_INCREMENT,
  run_id  int(11)      NOT NULL,
  name    varchar(80)  NOT NULL,
  size    int(11)      NOT NULL DEFAULT 0,
  data    mediumblob   DEFAULT NULL,
  PRIMARY KEY (id),
  KEY run_id (run_id)
) ENGINE=MyISAM DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci COMMENT='Result files of the firmware builds';

--
-- saved option sets of the Firmware tab
--

CREATE TABLE IF NOT EXISTS firmware_presets (
  id       int(11)      NOT NULL AUTO_INCREMENT,
  firmware varchar(20)  NOT NULL COMMENT 'hs or sample',
  name     varchar(40)  NOT NULL,
  options  text         NOT NULL COMMENT 'Option values as JSON',
  updated  datetime     DEFAULT NULL,
  PRIMARY KEY (id),
  UNIQUE KEY firmware_name (firmware, name)
) ENGINE=MyISAM DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci COMMENT='Named option sets for firmware builds';

--
-- flashing a build to the modem from the dashboard: the dashboard writes a request into firmware_flash_runs, the firmware flasher (tools/firmware_flasher, runs as root) does it and writes the log back; firmware_flasher holds the heartbeat the page uses to tell whether the flasher is running
--

CREATE TABLE IF NOT EXISTS firmware_flash_runs (
  id       int(11)      NOT NULL AUTO_INCREMENT,
  file_id  int(11)      NOT NULL COMMENT 'Row of firmware_files',
  mode     varchar(10)  NOT NULL COMMENT 'soft, gpio or seq',
  port     varchar(30)  NOT NULL,
  boot0    int(11)      NOT NULL DEFAULT 20,
  nrst     int(11)      NOT NULL DEFAULT 21,
  state    varchar(10)  NOT NULL DEFAULT 'queued' COMMENT 'queued, running, done, failed or expired',
  started  datetime     DEFAULT NULL,
  updated  datetime     DEFAULT NULL COMMENT 'Heartbeat of the flasher',
  finished datetime     DEFAULT NULL,
  units    text         DEFAULT NULL COMMENT 'Services stopped for the flash (started again afterwards)',
  log      mediumtext   DEFAULT NULL,
  PRIMARY KEY (id)
) ENGINE=MyISAM DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci COMMENT='Flash requests from the dashboard';

CREATE TABLE IF NOT EXISTS firmware_flasher (
  id      int(11)     NOT NULL,
  updated datetime    DEFAULT NULL COMMENT 'Written by the firmware flasher every few seconds',
  PRIMARY KEY (id)
) ENGINE=MyISAM DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci COMMENT='Heartbeat of the firmware flasher';

/*!40101 SET character_set_client = @saved_cs_client */;
/*!40103 SET TIME_ZONE=@OLD_TIME_ZONE */;

/*!40101 SET SQL_MODE=@OLD_SQL_MODE */;
/*!40014 SET FOREIGN_KEY_CHECKS=@OLD_FOREIGN_KEY_CHECKS */;
/*!40014 SET UNIQUE_CHECKS=@OLD_UNIQUE_CHECKS */;
/*!40101 SET CHARACTER_SET_CLIENT=@OLD_CHARACTER_SET_CLIENT */;
/*!40101 SET CHARACTER_SET_RESULTS=@OLD_CHARACTER_SET_RESULTS */;
/*!40101 SET COLLATION_CONNECTION=@OLD_COLLATION_CONNECTION */;
/*M!100616 SET NOTE_VERBOSITY=@OLD_NOTE_VERBOSITY */;

-- Dump completed on 2026-04-05  9:55:20
