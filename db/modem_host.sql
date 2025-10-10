/*M!999999\- enable the sandbox mode */ 
-- MariaDB dump 10.19  Distrib 10.5.29-MariaDB, for debian-linux-gnueabihf (armv8l)
--
-- Host: localhost    Database: modem_host
-- ------------------------------------------------------
-- Server version	10.5.29-MariaDB-0+deb11u1

/*!40101 SET @OLD_CHARACTER_SET_CLIENT=@@CHARACTER_SET_CLIENT */;
/*!40101 SET @OLD_CHARACTER_SET_RESULTS=@@CHARACTER_SET_RESULTS */;
/*!40101 SET @OLD_COLLATION_CONNECTION=@@COLLATION_CONNECTION */;
/*!40101 SET NAMES utf8mb4 */;
/*!40103 SET @OLD_TIME_ZONE=@@TIME_ZONE */;
/*!40103 SET TIME_ZONE='+00:00' */;
/*!40014 SET @OLD_UNIQUE_CHECKS=@@UNIQUE_CHECKS, UNIQUE_CHECKS=0 */;
/*!40014 SET @OLD_FOREIGN_KEY_CHECKS=@@FOREIGN_KEY_CHECKS, FOREIGN_KEY_CHECKS=0 */;
/*!40101 SET @OLD_SQL_MODE=@@SQL_MODE, SQL_MODE='NO_AUTO_VALUE_ON_ZERO' */;
/*!40111 SET @OLD_SQL_NOTES=@@SQL_NOTES, SQL_NOTES=0 */;

--
-- Current Database: `modem_host`
--

CREATE DATABASE /*!32312 IF NOT EXISTS*/ `modem_host` /*!40100 DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci */;

USE `modem_host`;

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
-- Dumping data for table `auth`
--

LOCK TABLES `auth` WRITE;
/*!40000 ALTER TABLE `auth` DISABLE KEYS */;
INSERT INTO `auth` VALUES ('admin','*4ACFE3202A5FF5CF467898FC58AAB1D615029441');
/*!40000 ALTER TABLE `auth` ENABLE KEYS */;
UNLOCK TABLES;

--
-- Table structure for table `config`
--

DROP TABLE IF EXISTS `config`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `config` (
  `id` int(11) NOT NULL AUTO_INCREMENT,
  `module` varchar(30) NOT NULL DEFAULT 'main',
  `parameter` varchar(30) NOT NULL,
  `display_type` varchar(25) NOT NULL DEFAULT 'none',
  `value` varchar(255) NOT NULL,
  PRIMARY KEY (`id`),
  UNIQUE KEY `module_params` (`module`,`parameter`) USING BTREE
) ENGINE=MyISAM AUTO_INCREMENT=8 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci COMMENT='Modem host configuration';
/*!40101 SET character_set_client = @saved_cs_client */;

--
-- Dumping data for table `config`
--

LOCK TABLES `config` WRITE;
/*!40000 ALTER TABLE `config` DISABLE KEYS */;
INSERT INTO `config` VALUES (6,'main','gateways','none'),(7,'main','activeModes','none'),(3,'main','username','admin'),(4,'main','callsign','N0CALL'),(5,'main','title','Repeater Dashboard');
/*!40000 ALTER TABLE `config` ENABLE KEYS */;
UNLOCK TABLES;

--
-- Table structure for table `dashb_commands`
--

DROP TABLE IF EXISTS `dashb_commands`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `dashb_commands` (
  `id` int(11) NOT NULL AUTO_INCREMENT,
  `command` varchar(20) NOT NULL,
  `parameter` varchar(40) NOT NULL,
  `result` varchar(80) DEFAULT NULL,
  PRIMARY KEY (`id`)
) ENGINE=MyISAM AUTO_INCREMENT=4 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci COMMENT='Dashboard commands for host program';
/*!40101 SET character_set_client = @saved_cs_client */;

--
-- Dumping data for table `dashb_commands`
--

LOCK TABLES `dashb_commands` WRITE;
/*!40000 ALTER TABLE `dashb_commands` DISABLE KEYS */;
/*!40000 ALTER TABLE `dashb_commands` ENABLE KEYS */;
UNLOCK TABLES;

--
-- Table structure for table `errors`
--

DROP TABLE IF EXISTS `errors`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `errors` (
  `id` int(11) NOT NULL AUTO_INCREMENT,
  `module` varchar(30) NOT NULL,
  `message` varchar(255) NOT NULL,
  `datetime` datetime NOT NULL,
  PRIMARY KEY (`id`)
) ENGINE=MyISAM AUTO_INCREMENT=62 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci COMMENT='Error messages';
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
) ENGINE=MyISAM AUTO_INCREMENT=3635 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci COMMENT='GPS data';
/*!40101 SET character_set_client = @saved_cs_client */;

--
-- Table structure for table `history`
--

DROP TABLE IF EXISTS `history`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `history` (
  `id` int(11) NOT NULL AUTO_INCREMENT,
  `type` enum('RF','NET') NOT NULL DEFAULT 'RF',
  `mode` varchar(10) NOT NULL,
  `Source` varchar(10) NOT NULL,
  `Destination` varchar(10) NOT NULL,
  `Loss_BER` float NOT NULL,
  `ss_message` varchar(80) NOT NULL,
  `datetime` datetime NOT NULL,
  `duration` int(11) NOT NULL,
  PRIMARY KEY (`id`)
) ENGINE=MyISAM AUTO_INCREMENT=2070 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci COMMENT='Communicatons history';
/*!40101 SET character_set_client = @saved_cs_client */;

--
-- Table structure for table `host_status`
--

DROP TABLE IF EXISTS `host_status`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `host_status` (
  `id` int(11) NOT NULL AUTO_INCREMENT,
  `module` varchar(30) NOT NULL,
  `property` varchar(20) NOT NULL,
  `value` varchar(50) NOT NULL,
  PRIMARY KEY (`id`)
) ENGINE=MyISAM DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci COMMENT='Host program status';
/*!40101 SET character_set_client = @saved_cs_client */;

--
-- Dumping data for table `host_status`
--

LOCK TABLES `host_status` WRITE;
/*!40000 ALTER TABLE `host_status` DISABLE KEYS */;
/*!40000 ALTER TABLE `host_status` ENABLE KEYS */;
UNLOCK TABLES;

--
-- Table structure for table `last_call`
--

DROP TABLE IF EXISTS `last_call`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `last_call` (
  `id` int(11) NOT NULL AUTO_INCREMENT,
  `mode` varchar(10) NOT NULL,
  `type` enum('RF','NET') DEFAULT NULL,
  `source` varchar(10) NOT NULL,
  `destination` varchar(10) NOT NULL,
  `ss_message` varchar(80) NOT NULL,
  `isTx` tinyint(1) NOT NULL DEFAULT 0,
  PRIMARY KEY (`id`)
) ENGINE=MyISAM AUTO_INCREMENT=4231 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci COMMENT='Last call info';
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
  `port` smallint(6) NOT NULL DEFAULT 0,
  `dash_address` varchar(80) NOT NULL DEFAULT 'none',
  `Name` varchar(50) NOT NULL DEFAULT 'none',
  `Country` varchar(2) NOT NULL DEFAULT 'na',
  `status` varchar(10) NOT NULL DEFAULT 'Unlinked',
  PRIMARY KEY (`id`),
  UNIQUE KEY `refl_type` (`refl_type`,`Nick`)
) ENGINE=MyISAM AUTO_INCREMENT=4 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci COMMENT='DV Reflectors';
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
/*!40111 SET SQL_NOTES=@OLD_SQL_NOTES */;

-- Dump completed on 2025-09-28 14:53:03
