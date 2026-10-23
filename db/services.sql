-- Services the dashboard may start, stop and enable at boot.
-- Created by Rick KD0OSS
--
-- Run this once on an existing installation:   mysql -u root -p < db/services.sql
-- The dashboard builds its Services tab from these rows, so it needs no knowledge of any RF mode.
-- To manage another systemd unit add a row here (the unit name must end in .service).

CREATE TABLE IF NOT EXISTS modem_host.services (
  unit        varchar(60)  NOT NULL,
  label       varchar(40)  NOT NULL,
  description varchar(120) NOT NULL DEFAULT '',
  sort_order  int(11)      NOT NULL DEFAULT 100,
  active      varchar(20)  NOT NULL DEFAULT 'unknown' COMMENT 'Written by the service manager',
  enabled     varchar(20)  NOT NULL DEFAULT 'unknown' COMMENT 'Written by the service manager',
  updated     datetime     DEFAULT NULL,
  PRIMARY KEY (unit)
) ENGINE=MyISAM DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci COMMENT='Services the dashboard can manage';

CREATE TABLE IF NOT EXISTS modem_host.service_commands (
  id        int(11)     NOT NULL AUTO_INCREMENT,
  unit      varchar(60) NOT NULL,
  action    varchar(10) NOT NULL,
  requested datetime    NOT NULL DEFAULT CURRENT_TIMESTAMP,
  result    varchar(80) DEFAULT NULL,
  PRIMARY KEY (id)
) ENGINE=MyISAM DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci COMMENT='Requests from the dashboard for the service manager';

INSERT IGNORE INTO modem_host.services (unit, label, description, sort_order) VALUES
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
CREATE TABLE IF NOT EXISTS modem_host.system_actions (
  action      varchar(10)  NOT NULL,
  label       varchar(30)  NOT NULL,
  description varchar(120) NOT NULL DEFAULT '',
  confirm     varchar(200) NOT NULL DEFAULT '',
  sort_order  int(11)      NOT NULL DEFAULT 100,
  PRIMARY KEY (action)
) ENGINE=MyISAM DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci COMMENT='Power actions the dashboard can ask for';

INSERT IGNORE INTO modem_host.system_actions (action, label, description, confirm, sort_order) VALUES
  ('reboot',   'Restart the Pi',   'Restarts the Pi. The dashboard is back in about a minute.',
   'Restart the Pi now?\n\nAll services stop and start again, and you will need to log in again.', 10),
  ('poweroff', 'Shut down the Pi', 'Shuts the Pi down. It must be switched on by hand again.',
   'Shut the Pi down now?\n\nIt will stay off until the power is switched off and on again.', 20);
