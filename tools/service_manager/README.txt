OpenMT service manager
Created by Rick KD0OSS

What it does
------------
Lets a logged in dashboard user start, stop and restart the OpenMT hosts and
gateways, and choose which of them start when the Pi boots (Settings ->
Services tab). The dashboard knows nothing about any RF mode: the list of
services comes from the table modem_host.services.

How it is built
---------------
  dashboard (browser)  --POST-->  data_query.php  --INSERT-->  modem_host.service_commands
                                                                         |
  modem_host.services  <--state--  service_manager (root)  <--polls (0.5 s)
                                         |
                                  systemctl start|stop|restart|enable|disable <unit>

The web server has no rights over systemd. Only service_manager, running as
root, does, and it checks every request again:
  - the action must be start, stop, restart, enable or disable,
  - the unit must look like a service file name (letters, digits - _ . @ and
    ending in .service),
  - the unit must be listed in modem_host.services.
systemctl is run directly, never through a shell. Requests that are not done
within 2 minutes are marked "expired" and never run later, so a request left
over from before a reboot cannot start or stop anything.
The dashboard side requires the login session, a POST, and the header
X-OpenMT: 1 (so another web site cannot send the request for you).

Install
-------
  mysql -u root -p < db/services.sql            (creates the tables, lists the OpenMT services)
  cd tools/service_manager && mkdir build && cd build && cmake .. && make
  sudo cp ../openmt-service-manager.service /etc/systemd/system/
  (edit the paths in it if OpenMT is not in /home/pi/OpenMT)
  sudo systemctl daemon-reload
  sudo systemctl enable --now openmt-service-manager

Adding a service to the list
----------------------------
  INSERT INTO modem_host.services (unit, label, description, sort_order)
  VALUES ('my-thing.service', 'My thing', 'What it does', 90);
The service file must be installed in /etc/systemd/system. Services that the
dashboard enables at boot must have an [Install] section (the OpenMT ones do).

Restart and shut down the Pi
----------------------------
The Services tab also shows buttons for the power actions listed in
modem_host.system_actions (db/services.sql adds "reboot" and "poweroff"). The
commands are fixed in the service manager (systemctl reboot / poweroff); a row
only switches the action on and sets its label and confirmation text. Delete a
row to remove the button and the permission.
  - The request uses the unit name "system" and is refused for anything else.
  - It is marked ok before the Pi goes down, then run 3 seconds later, so it
    can never run a second time after the restart.
  - After a restart the page waits for the Pi to go down and come back, then
    reloads (log in again). After a shut down the Pi stays off until the power
    is switched off and on again.

Notes
-----
  - Starting a gateway also starts its host, and the modem host, because the
    service files say Requires=. Stopping a host stops its gateway and
    stopping the modem host stops all of them.
  - The Start at boot box runs systemctl enable / disable.
  - The tab warns when the service manager is not running (it refreshes the
    state of every listed unit every 3 seconds).
  - Use -s /path/to/systemctl to run it against another program (tests).

Test
----
  SHIM=<dir holding mariadb/mysql.h> tools/service_manager/test/run_test.sh
