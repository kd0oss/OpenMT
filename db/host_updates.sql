-- Host lists the dashboard can update (reflectors, DMR/NXDN user ids).
-- Created by Rick KD0OSS
--
-- Run this once on an existing installation:   mysql -u root -p < db/host_updates.sql
-- The dashboard builds the update buttons of its Services tab from these rows, so it needs no knowledge of
-- any RF mode.  "section" is the word given to updateHosts.php (php updateHosts.php p25).  refl_type is the
-- reflector type whose entries are counted after the update; leave it NULL for a list that is not in the
-- reflectors table (the user ids are counted from dmr_ids).

CREATE TABLE IF NOT EXISTS modem_host.host_update_sections (
  section     varchar(10)  NOT NULL,
  label       varchar(40)  NOT NULL,
  description varchar(120) NOT NULL DEFAULT '',
  refl_type   varchar(10)  DEFAULT NULL,
  sort_order  int(11)      NOT NULL DEFAULT 100,
  last_run    datetime     DEFAULT NULL COMMENT 'Written by updateHostsRun.php',
  last_result varchar(120) DEFAULT NULL COMMENT 'Written by updateHostsRun.php',
  PRIMARY KEY (section)
) ENGINE=MyISAM DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci COMMENT='Host lists the dashboard can update';

CREATE TABLE IF NOT EXISTS modem_host.host_update_runs (
  id       int(11)     NOT NULL AUTO_INCREMENT,
  sections varchar(120) NOT NULL,
  state    varchar(10) NOT NULL DEFAULT 'queued' COMMENT 'queued, running, done or failed',
  started  datetime    DEFAULT NULL,
  updated  datetime    DEFAULT NULL COMMENT 'Heartbeat of the running update',
  finished datetime    DEFAULT NULL,
  log      mediumtext  DEFAULT NULL,
  PRIMARY KEY (id)
) ENGINE=MyISAM DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci COMMENT='Updates started from the dashboard';

INSERT IGNORE INTO modem_host.host_update_sections (section, label, description, refl_type, sort_order) VALUES
  ('p25',   'P25 reflectors',    'Talk groups and addresses of the P25 reflectors', 'P25',   10),
  ('nxdn',  'NXDN reflectors',   'Talk groups and addresses of the NXDN reflectors','NXDN',  20),
  ('ysf',   'YSF reflectors',    'Yaesu System Fusion reflectors',                  'YSF',   30),
  ('m17',   'M17 reflectors',    'M17 reflectors and their modules',                'M17',   40),
  ('dstar', 'D-Star reflectors', 'REF, XRF, DCS and XLX reflectors',                'DSTAR', 50),
  ('ids',   'DMR and NXDN IDs',  'Call sign look-up for the DMR and NXDN user ids', NULL,    60);
