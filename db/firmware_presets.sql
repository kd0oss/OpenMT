-- Saved option sets of the dashboard's Firmware tab (Settings, Firmware).
-- Created by Rick KD0OSS
--
--     mysql -u openmt -p < db/firmware_presets.sql

CREATE TABLE IF NOT EXISTS modem_host.firmware_presets (
  id       int(11)      NOT NULL AUTO_INCREMENT,
  firmware varchar(20)  NOT NULL COMMENT 'hs or sample',
  name     varchar(40)  NOT NULL,
  options  text         NOT NULL COMMENT 'Option values as JSON',
  updated  datetime     DEFAULT NULL,
  PRIMARY KEY (id),
  UNIQUE KEY firmware_name (firmware, name)
) ENGINE=MyISAM DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci COMMENT='Named option sets for firmware builds';
