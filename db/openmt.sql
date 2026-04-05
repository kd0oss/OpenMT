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

--
-- Table structure for table `auth`
--

DROP TABLE IF EXISTS `auth`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `auth` (
  `username` varchar(10) NOT NULL,
  `password` varchar(50) NOT NULL
) ENGINE=MyISAM DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci COMMENT='Dashboard authorization';
/*!40101 SET character_set_client = @saved_cs_client */;

--
-- Table structure for table `config`
--

DROP TABLE IF EXISTS `config`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `config` (
  `id` int(11) NOT NULL AUTO_INCREMENT,
  `modem_name` varchar(10) DEFAULT 'modem1',
  `module` varchar(30) NOT NULL DEFAULT 'main',
  `parameter` varchar(30) NOT NULL,
  `display_type` varchar(25) NOT NULL DEFAULT 'none',
  `value` varchar(255) NOT NULL,
  PRIMARY KEY (`id`),
  UNIQUE KEY `module_params` (`module`,`parameter`) USING BTREE
) ENGINE=MyISAM AUTO_INCREMENT=55 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci COMMENT='Modem host configuration';
/*!40101 SET character_set_client = @saved_cs_client */;

--
-- Table structure for table `dashb_commands`
--

DROP TABLE IF EXISTS `dashb_commands`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `dashb_commands` (
  `id` int(11) NOT NULL AUTO_INCREMENT,
  `modem_name` varchar(10) DEFAULT 'modem1',
  `command` varchar(20) NOT NULL,
  `parameter` varchar(40) NOT NULL,
  `result` varchar(80) DEFAULT NULL,
  PRIMARY KEY (`id`)
) ENGINE=MyISAM AUTO_INCREMENT=46 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci COMMENT='Dashboard commands for host program';
/*!40101 SET character_set_client = @saved_cs_client */;

--
-- Table structure for table `dmr_ids`
--

DROP TABLE IF EXISTS `dmr_ids`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `dmr_ids` (
  `id` int(11) NOT NULL AUTO_INCREMENT,
  `DMR_Id` int(11) DEFAULT NULL,
  `Callsign` varchar(10) DEFAULT NULL,
  `Name` varchar(40) DEFAULT NULL,
  PRIMARY KEY (`id`),
  KEY `idx_dmr_id` (`DMR_Id`)
) ENGINE=InnoDB AUTO_INCREMENT=302009 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_uca1400_ai_ci;
/*!40101 SET character_set_client = @saved_cs_client */;

--
-- Table structure for table `errors`
--

DROP TABLE IF EXISTS `errors`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `errors` (
  `id` int(11) NOT NULL AUTO_INCREMENT,
  `modem_name` varchar(10) DEFAULT 'modem1',
  `module` varchar(30) NOT NULL,
  `message` varchar(255) NOT NULL,
  `datetime` datetime NOT NULL,
  PRIMARY KEY (`id`)
) ENGINE=MyISAM AUTO_INCREMENT=2 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci COMMENT='Error messages';
/*!40101 SET character_set_client = @saved_cs_client */;

--
-- Table structure for table `gps`
--

DROP TABLE IF EXISTS `gps`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `gps` (
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

DROP TABLE IF EXISTS `history`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `history` (
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

DROP TABLE IF EXISTS `host_status`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `host_status` (
  `id` int(11) NOT NULL AUTO_INCREMENT,
  `modem_name` varchar(10) DEFAULT 'modem1',
  `module` varchar(30) NOT NULL,
  `property` varchar(20) NOT NULL,
  `value` varchar(50) NOT NULL,
  PRIMARY KEY (`id`)
) ENGINE=MyISAM AUTO_INCREMENT=30 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci COMMENT='Host program status';
/*!40101 SET character_set_client = @saved_cs_client */;

--
-- Table structure for table `last_call`
--

DROP TABLE IF EXISTS `last_call`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `last_call` (
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
) ENGINE=MyISAM AUTO_INCREMENT=55302 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci COMMENT='Last call info';
/*!40101 SET character_set_client = @saved_cs_client */;

--
-- Table structure for table `reflectors`
--

DROP TABLE IF EXISTS `reflectors`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `reflectors` (
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
) ENGINE=MyISAM AUTO_INCREMENT=10781 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci COMMENT='DV Reflectors';
/*!40101 SET character_set_client = @saved_cs_client */;

--
-- Table structure for table `sms_messages`
--

DROP TABLE IF EXISTS `sms_messages`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `sms_messages` (
  `id` int(11) NOT NULL,
  `source` varchar(10) NOT NULL,
  `message` text NOT NULL,
  `datetime` datetime NOT NULL,
  PRIMARY KEY (`id`)
) ENGINE=MyISAM DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci COMMENT='SMS message';
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
