OpenMT firmware flasher
=======================

Flashes a firmware file built in the dashboard (Settings, Firmware tab) to the modem.

The dashboard's web server (www-data) has no rights over the modem's serial port, the GPIO pins or systemd, so
the page only writes a request into modem_host.firmware_flash_runs.  flasher.php, running as root
(openmt-flasher.service), does the rest:

  1. takes the oldest waiting request (a request older than 2 minutes is marked "expired", never run later)
  2. loads the file from modem_host.firmware_files and checks the request again with fwFlashPlan() of
     dashboard/www/firmwareBuild.php (only .bin files of a build; method soft/gpio/seq; port /dev/ttyAMA*, ttyS*,
     ttyUSB*, ttyACM*, serial*; GPIO 0-27, BOOT0 and NRST different)
  3. stops the services of modem_host.services that are running (last started first), because they hold the port
  4. runs  /bin/sh openmt_flash.sh <arguments> <file>  (no shell parsing; arguments come from the checked values)
  5. starts the services again in order, whatever the result was, and stores the log
  6. while idle it writes a heartbeat to modem_host.firmware_flasher every second; the page shows a note
     when the heartbeat is older than 15 seconds

If the flasher is killed in the middle of a flash, it marks that request failed and starts the stopped services
at its next start.

Methods (see modem/OPENMT_SAMPLE_SMT32FXXX/tools/openmt_flash.sh)
  seq   pins and stm32flash, like Pi-Star/WPSD         (any build that is flashed to the start of the flash)
  gpio  pins only drive BOOT0/NRST
  soft  the running OPENMT_SAMPLE F105 firmware reboots itself into the ROM boot loader (F105 only)

Not flashed: OPENMT_HS "bl" (linked for the address behind the MMDVM_HS boot loader), .hex files.
The first flash of the F105 loader image (-L option of the build) needs an SWD programmer.

Test without hardware
  php flasher.php --once --systemctl /path/to/fake_systemctl --script /path/to/fake_flash.sh --source /path/to/OpenMT

Requires: php-cli with mysqli, stm32flash, the database tables of db/firmware_builds.sql and db/firmware_flash.sql.
