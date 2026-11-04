-- Flashing a build to the modem from the dashboard (Settings, Firmware tab).
-- Created by Rick KD0OSS
--
-- Run this once on an existing installation (after db/firmware_builds.sql):
--     mysql -u openmt -p < db/firmware_flash.sql
-- The dashboard writes a request into firmware_flash_runs; the firmware flasher (tools/firmware_flasher, runs as
-- root) does it and writes the log back.  firmware_flasher holds the heartbeat the page uses to tell whether the
-- flasher is running.

CREATE TABLE IF NOT EXISTS modem_host.firmware_flash_runs (
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

CREATE TABLE IF NOT EXISTS modem_host.firmware_flasher (
  id      int(11)     NOT NULL,
  updated datetime    DEFAULT NULL COMMENT 'Written by the firmware flasher every few seconds',
  PRIMARY KEY (id)
) ENGINE=MyISAM DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci COMMENT='Heartbeat of the firmware flasher';
