-- NXDN radio ids for the dashboard (nxdn_host looks here first, then in dmr_ids).
-- updateHosts.php empties dmr_ids every time it runs, so ids added there by hand are lost;
-- this table is not touched by it.
--
--   mysql -u root -p < nxdn_ids.sql
CREATE TABLE IF NOT EXISTS modem_host.nxdn_ids (
  `id` int(11) NOT NULL AUTO_INCREMENT,
  `NXDN_Id` int(11) DEFAULT NULL,
  `Callsign` varchar(10) DEFAULT NULL,
  `Name` varchar(40) DEFAULT NULL,
  PRIMARY KEY (`id`),
  KEY `idx_nxdn_id` (`NXDN_Id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- the "openmt" database user needs to read it (skip if it already has rights on modem_host.*)
GRANT SELECT ON modem_host.nxdn_ids TO 'openmt'@'localhost';

-- an example row
-- INSERT INTO modem_host.nxdn_ids SET NXDN_Id = 9999, Callsign = 'KD0OSS', Name = 'Rick';
