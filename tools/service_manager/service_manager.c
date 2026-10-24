/***************************************************************************
 *   Copyright (C) 2026 by Rick KD0OSS                                     *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 3 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, see <http://www.gnu.org/licenses/>   *
 *                                                                         *
 ***************************************************************************/

/*
 * Created by Rick KD0OSS
 *
 * OpenMT service manager.
 *
 * Runs as root and does what the dashboard asks for the systemd units that
 * are listed in the modem_host.services table: start, stop, restart, and
 * enable or disable at boot. The web server never gets any rights of its own.
 * It only writes a request into modem_host.service_commands, and this program
 * carries out the requests it finds there. It also copies the state of every
 * listed unit back into modem_host.services, so the dashboard can show it.
 *
 * A request is only carried out when
 *   - the action is one of start, stop, restart, enable, disable,
 *   - the unit name has the form of a service file name, and
 *   - the unit is listed in modem_host.services.
 * The power actions reboot and poweroff (unit "system") are carried out only
 * when the action is listed in modem_host.system_actions. They are marked
 * done before the Pi goes down, so a request can never run twice.
 * systemctl is started directly, never through a shell. Requests that were
 * not carried out within two minutes (for example because this program was
 * not running) are marked "expired" and are never carried out later.
 *
 * Usage: service_manager [-s path_to_systemctl]
 */

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <mariadb/mysql.h>

#include "tools.h"

#define SM_VERSION        "2026-10-22"
#define SM_UNIT_MAX       60
#define SM_MAX_UNITS      64
#define SM_STATUS_MS      3000
#define SM_POLL_US        500000
#define SM_FAST_STATUS_MS 1000      /* refresh interval for a while after a request, to follow a service that is starting or stopping */
#define SM_FAST_S         15        /* how long that lasts */
#define SM_RUN_TIMEOUT_S  60
#define SM_OUT_MAX        16384

static volatile sig_atomic_t smStop = 0;
static const char*           smSystemctl = "/usr/bin/systemctl";
static time_t                smFastUntil = 0;      /* until then the states are refreshed more often */
static unsigned              smPowerDelayS = 3;    /* seconds between reporting a power request done and doing it */

/*
 * smValidUnit()
 *
 * Checks that a text is shaped like the name of a service file: letters,
 * digits, '-', '_', '.', '@', ending in ".service" and not starting with '-'
 * or '.'. This keeps anything that looks like an option or a path away from
 * systemctl.
 *
 * Parameters:
 *   unit - The text to check.
 *
 * Returns: true if the text can be used as a unit name.
 */
bool smValidUnit(const char* unit)
{
    size_t len = strlen(unit);
    size_t suffix = strlen(".service");

    if (len <= suffix || len >= SM_UNIT_MAX || unit[0] == '-' || unit[0] == '.')
        return false;
    if (strcmp(unit + len - suffix, ".service") != 0)
        return false;
    for (size_t i = 0; i < len; i++)
    {
        char c = unit[i];

        if (!isalnum((unsigned char)c) && c != '-' && c != '_' && c != '.' && c != '@')
            return false;
    }
    return true;
}

/*
 * smValidAction()
 *
 * Checks that an action is one this program carries out.
 *
 * Parameters:
 *   action - The action text.
 *
 * Returns: true for start, stop, restart, enable and disable.
 */
bool smValidAction(const char* action)
{
    static const char* const list[] = { "start", "stop", "restart", "enable", "disable" };

    for (size_t i = 0; i < sizeof(list) / sizeof(list[0]); i++)
    {
        if (strcmp(action, list[i]) == 0)
            return true;
    }
    return false;
}

/*
 * smValidPower()
 *
 * Checks that an action is one of the power actions of the Pi.
 *
 * Parameters:
 *   action - The action text.
 *
 * Returns: true for reboot and poweroff.
 */
bool smValidPower(const char* action)
{
    return strcmp(action, "reboot") == 0 || strcmp(action, "poweroff") == 0;
}

/*
 * smRun()
 *
 * Starts a program without a shell, waits for it to end (at most
 * SM_RUN_TIMEOUT_S seconds, after which it is killed) and returns its exit
 * code. When out is not NULL the standard output of the program is stored
 * there.
 *
 * Parameters:
 *   argv   - NULL terminated argument list. argv[0] is the program path.
 *   out    - Buffer that receives the output text, or NULL.
 *   outLen - Size of out in bytes.
 *
 * Returns: The exit code, or -1 if the program could not be run or was killed.
 */
static int smRun(char* const argv[], char* out, size_t outLen)
{
    int    fds[2] = { -1, -1 };
    size_t used   = 0;
    time_t start  = time(NULL);
    pid_t  pid;
    int    status = 0;

    if (out != NULL)
    {
        out[0] = '\0';
        if (pipe(fds) != 0)
            return -1;
    }

    pid = fork();
    if (pid < 0)
    {
        if (out != NULL)
        {
            close(fds[0]);
            close(fds[1]);
        }
        return -1;
    }
    if (pid == 0)
    {
        if (out != NULL)
        {
            dup2(fds[1], STDOUT_FILENO);
            close(fds[0]);
            close(fds[1]);
        }
        execv(argv[0], argv);
        _exit(127);
    }

    if (out != NULL)
    {
        close(fds[1]);
        /* read until the program closes its output; a program that never ends is killed below */
        for (;;)
        {
            char    buf[512];
            ssize_t n = read(fds[0], buf, sizeof(buf));

            if (n > 0)
            {
                size_t room = outLen - 1 - used;
                size_t take = (size_t)n < room ? (size_t)n : room;

                memcpy(out + used, buf, take);
                used += take;
                out[used] = '\0';
            }
            else if (n == 0 || (errno != EINTR && errno != EAGAIN))
                break;
            if (time(NULL) - start > SM_RUN_TIMEOUT_S)
            {
                kill(pid, SIGKILL);
                break;
            }
        }
        close(fds[0]);
    }

    for (;;)
    {
        pid_t r = waitpid(pid, &status, WNOHANG);

        if (r == pid)
            break;
        if (r < 0 && errno != EINTR)
            return -1;
        if (time(NULL) - start > SM_RUN_TIMEOUT_S)
        {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            return -1;
        }
        usleep(50000);
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/*
 * smEscape()
 *
 * Makes a text safe to put between quotes in an SQL statement.
 *
 * Parameters:
 *   db  - The database connection.
 *   in  - The text.
 *   out - Receives the escaped text.
 *   len - Size of out in bytes (at least 2 * strlen(in) + 1).
 *
 * Returns: nothing.
 */
static void smEscape(MYSQL* db, const char* in, char* out, size_t len)
{
    if (len < 2 * strlen(in) + 1)
    {
        out[0] = '\0';
        return;
    }
    mysql_real_escape_string(db, out, in, (unsigned long)strlen(in));
}

/*
 * smQuery()
 *
 * Runs one SQL statement.
 *
 * Parameters:
 *   db  - The database connection.
 *   sql - The statement.
 *
 * Returns: true if the server accepted it.
 */
static bool smQuery(MYSQL* db, const char* sql)
{
    if (mysql_query(db, sql) != 0)
    {
        fprintf(stderr, "service_manager: %s\n", mysql_error(db));
        return false;
    }
    return true;
}

/*
 * smParseShow()
 *
 * Reads the output of "systemctl show -p Id -p ActiveState -p UnitFileState
 * -p LoadState unit ..." and sets the state of the matching entries. The output has one
 * block of Key=Value lines per unit, with a blank line between blocks.
 *
 * Parameters:
 *   text    - The output of systemctl.
 *   units   - The unit names, as given on the command line.
 *   count   - Number of units.
 *   active  - Receives the active state of each unit (SM_UNIT_MAX bytes each).
 *   enabled - Receives the boot state of each unit (SM_UNIT_MAX bytes each).
 *
 * Returns: nothing. Units that are not in the output keep "unknown"; a unit whose
 *   service file is not installed gets the state "not-installed".
 */
void smParseShow(const char* text, char units[][SM_UNIT_MAX], int count,
                 char active[][SM_UNIT_MAX], char enabled[][SM_UNIT_MAX])
{
    char id[SM_UNIT_MAX]  = "";
    char act[SM_UNIT_MAX] = "";
    char ufs[SM_UNIT_MAX] = "";
    char load[SM_UNIT_MAX] = "";
    const char* p = text;

    for (;;)
    {
        const char* eol = strchr(p, '\n');
        size_t      len = eol != NULL ? (size_t)(eol - p) : strlen(p);

        if (len == 0)
        {
            /* a blank line (or the end) closes a block */
            for (int i = 0; i < count && id[0] != '\0'; i++)
            {
                if (strcmp(units[i], id) == 0)
                {
                    snprintf(active[i], SM_UNIT_MAX, "%s", strcmp(load, "not-found") == 0 ? "not-installed" : (act[0] != '\0' ? act : "unknown"));
                    snprintf(enabled[i], SM_UNIT_MAX, "%s", ufs[0] != '\0' ? ufs : "none");
                }
            }
            id[0] = act[0] = ufs[0] = load[0] = '\0';
            if (eol == NULL)
                break;
        }
        else
        {
            char line[128];

            snprintf(line, sizeof(line), "%.*s", (int)len, p);
            if (strncmp(line, "Id=", 3) == 0)
                snprintf(id, sizeof(id), "%s", line + 3);
            else if (strncmp(line, "ActiveState=", 12) == 0)
                snprintf(act, sizeof(act), "%s", line + 12);
            else if (strncmp(line, "UnitFileState=", 14) == 0)
                snprintf(ufs, sizeof(ufs), "%s", line + 14);
            else if (strncmp(line, "LoadState=", 10) == 0)
                snprintf(load, sizeof(load), "%s", line + 10);
            if (eol == NULL)
            {
                /* last line without a newline: close the block */
                for (int i = 0; i < count && id[0] != '\0'; i++)
                {
                    if (strcmp(units[i], id) == 0)
                    {
                        snprintf(active[i], SM_UNIT_MAX, "%s", strcmp(load, "not-found") == 0 ? "not-installed" : (act[0] != '\0' ? act : "unknown"));
                        snprintf(enabled[i], SM_UNIT_MAX, "%s", ufs[0] != '\0' ? ufs : "none");
                    }
                }
                break;
            }
        }
        p = eol + 1;
    }
}

/*
 * smListUnits()
 *
 * Reads the names of the units listed in modem_host.services that have a
 * valid shape.
 *
 * Parameters:
 *   db    - The database connection.
 *   units - Receives the names.
 *
 * Returns: The number of units, or -1 if the query failed.
 */
static int smListUnits(MYSQL* db, char units[][SM_UNIT_MAX])
{
    int n = 0;

    if (!smQuery(db, "SELECT unit FROM modem_host.services ORDER BY unit"))
        return -1;

    MYSQL_RES* res = mysql_store_result(db);
    MYSQL_ROW  row;

    if (res == NULL)
        return -1;
    while (n < SM_MAX_UNITS && (row = mysql_fetch_row(res)) != NULL)
    {
        if (row[0] != NULL && smValidUnit(row[0]))
            snprintf(units[n++], SM_UNIT_MAX, "%s", row[0]);
    }
    mysql_free_result(res);
    return n;
}

/*
 * smRefreshStatus()
 *
 * Asks systemd about every listed unit (one systemctl call for all of them)
 * and stores the active state and the boot state in modem_host.services.
 *
 * Parameters:
 *   db - The database connection.
 *
 * Returns: true if the database could be read and written.
 */
bool smRefreshStatus(MYSQL* db)
{
    static char units[SM_MAX_UNITS][SM_UNIT_MAX];
    static char active[SM_MAX_UNITS][SM_UNIT_MAX];
    static char enabled[SM_MAX_UNITS][SM_UNIT_MAX];
    static char out[SM_OUT_MAX];
    char*       argv[SM_MAX_UNITS + 8];
    int         argc = 0;
    int         n    = smListUnits(db, units);

    if (n < 0)
        return false;
    if (n == 0)
        return true;

    argv[argc++] = (char*)smSystemctl;
    argv[argc++] = "show";
    argv[argc++] = "-p";
    argv[argc++] = "Id";
    argv[argc++] = "-p";
    argv[argc++] = "ActiveState";
    argv[argc++] = "-p";
    argv[argc++] = "UnitFileState";
    argv[argc++] = "-p";
    argv[argc++] = "LoadState";
    argv[argc++] = "--";
    for (int i = 0; i < n; i++)
    {
        snprintf(active[i], SM_UNIT_MAX, "unknown");
        snprintf(enabled[i], SM_UNIT_MAX, "unknown");
        argv[argc++] = units[i];
    }
    argv[argc] = NULL;

    if (smRun(argv, out, sizeof(out)) != 0)
        fprintf(stderr, "service_manager: systemctl show failed\n");
    else
        smParseShow(out, units, n, active, enabled);

    for (int i = 0; i < n; i++)
    {
        char eu[2 * SM_UNIT_MAX + 1];
        char ea[2 * SM_UNIT_MAX + 1];
        char ee[2 * SM_UNIT_MAX + 1];
        char sql[512];

        smEscape(db, units[i], eu, sizeof(eu));
        smEscape(db, active[i], ea, sizeof(ea));
        smEscape(db, enabled[i], ee, sizeof(ee));
        snprintf(sql, sizeof(sql),
                 "UPDATE modem_host.services SET active = '%s', enabled = '%s', updated = NOW() WHERE unit = '%s'",
                 ea, ee, eu);
        if (!smQuery(db, sql))
            return false;
    }
    return true;
}

/*
 * smUnitListed()
 *
 * Checks that a unit is listed in modem_host.services.
 *
 * Parameters:
 *   db   - The database connection.
 *   unit - The unit name.
 *
 * Returns: true if it is listed.
 */
static bool smUnitListed(MYSQL* db, const char* unit)
{
    char eu[2 * SM_UNIT_MAX + 1];
    char sql[256];
    bool found = false;

    smEscape(db, unit, eu, sizeof(eu));
    snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM modem_host.services WHERE unit = '%s'", eu);
    if (!smQuery(db, sql))
        return false;

    MYSQL_RES* res = mysql_store_result(db);
    MYSQL_ROW  row;

    if (res == NULL)
        return false;
    row = mysql_fetch_row(res);
    found = row != NULL && row[0] != NULL && atoi(row[0]) > 0;
    mysql_free_result(res);
    return found;
}

/*
 * smPowerListed()
 *
 * Checks that a power action is listed in modem_host.system_actions.
 *
 * Parameters:
 *   db     - The database connection.
 *   action - The action (reboot or poweroff).
 *
 * Returns: true if it is listed.
 */
static bool smPowerListed(MYSQL* db, const char* action)
{
    char ea[2 * 16 + 1];
    char sql[256];
    bool found = false;

    smEscape(db, action, ea, sizeof(ea));
    snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM modem_host.system_actions WHERE action = '%s'", ea);
    if (!smQuery(db, sql))
        return false;

    MYSQL_RES* res = mysql_store_result(db);
    MYSQL_ROW  row;

    if (res == NULL)
        return false;
    row = mysql_fetch_row(res);
    found = row != NULL && row[0] != NULL && atoi(row[0]) > 0;
    mysql_free_result(res);
    return found;
}

/*
 * smSetResult()
 *
 * Stores the outcome of a request.
 *
 * Parameters:
 *   db     - The database connection.
 *   id     - The request number.
 *   result - Text for the dashboard ("ok", "refused", ...).
 *
 * Returns: nothing.
 */
static void smSetResult(MYSQL* db, long id, const char* result)
{
    char er[2 * 80 + 1];
    char sql[256];

    smEscape(db, result, er, sizeof(er));
    snprintf(sql, sizeof(sql), "UPDATE modem_host.service_commands SET result = '%s' WHERE id = %ld", er, id);
    smQuery(db, sql);
}

/*
 * smDoCommand()
 *
 * Checks one request and carries it out.
 *
 * Parameters:
 *   db     - The database connection.
 *   id     - The request number.
 *   unit   - The unit name.
 *   action - The action.
 *
 * Returns: nothing. The outcome is stored with the request.
 */
static void smDoCommand(MYSQL* db, long id, const char* unit, const char* action)
{
    char* argv[8];
    char  result[80];
    int   argc = 0;
    int   rc;

    if (smValidPower(action))
    {
        /* The request is marked done before the Pi goes down, so it can never run a second time after the
           restart. The short wait lets the dashboard read the result first. */
        if (strcmp(unit, "system") != 0 || !smPowerListed(db, action))
        {
            fprintf(stderr, "service_manager: refused request %ld (%s %s)\n", id, action, unit);
            smSetResult(db, id, "refused");
            return;
        }
        printf("service_manager: %s the Pi\n", action);
        fflush(stdout);
        smSetResult(db, id, "ok");
        for (unsigned i = 0; i < smPowerDelayS; i++)
            sleep(1);
        argv[argc++] = (char*)smSystemctl;
        argv[argc++] = "--no-ask-password";
        argv[argc++] = (char*)action;
        argv[argc]   = NULL;
        smRun(argv, NULL, 0);
        return;
    }

    if (!smValidAction(action) || !smValidUnit(unit) || !smUnitListed(db, unit))
    {
        fprintf(stderr, "service_manager: refused request %ld (%s %s)\n", id, action, unit);
        smSetResult(db, id, "refused");
        return;
    }

    argv[argc++] = (char*)smSystemctl;
    argv[argc++] = "--no-ask-password";
    argv[argc++] = (char*)action;
    argv[argc++] = "--";
    argv[argc++] = (char*)unit;
    argv[argc]   = NULL;

    printf("service_manager: %s %s\n", action, unit);
    fflush(stdout);
    rc = smRun(argv, NULL, 0);
    if (rc == 0)
        snprintf(result, sizeof(result), "ok");
    else
        snprintf(result, sizeof(result), "failed (code %d)", rc);

    /* The new state is stored before the request is reported as done: the dashboard reads the states as soon
       as it sees the result, and would otherwise show the old state until the next refresh. */
    smFastUntil = time(NULL) + SM_FAST_S;
    smRefreshStatus(db);
    smSetResult(db, id, result);
}

/*
 * smCycle()
 *
 * One pass of the main loop: drops requests that are too old, carries out the
 * new ones, and refreshes the unit states when something was done or the
 * refresh time has come.
 *
 * Parameters:
 *   db      - The database connection.
 *   refresh - True when the unit states should be refreshed in any case.
 *
 * Returns: true if the database could be used; false means reconnect.
 */
bool smCycle(MYSQL* db, bool refresh)
{
    bool done = false;

    if (!smQuery(db, "UPDATE modem_host.service_commands SET result = 'expired' "
                     "WHERE result IS NULL AND requested < NOW() - INTERVAL 2 MINUTE"))
        return false;
    smQuery(db, "DELETE FROM modem_host.service_commands WHERE requested < NOW() - INTERVAL 1 DAY");

    if (!smQuery(db, "SELECT id, unit, action FROM modem_host.service_commands WHERE result IS NULL ORDER BY id LIMIT 10"))
        return false;

    MYSQL_RES* res = mysql_store_result(db);
    MYSQL_ROW  row;

    if (res == NULL)
        return false;

    /* the rows are copied first: running systemctl can take a while and the result set stays open */
    static long id[10];
    static char unit[10][SM_UNIT_MAX];
    static char action[10][16];
    int         n = 0;

    while (n < 10 && (row = mysql_fetch_row(res)) != NULL)
    {
        id[n] = row[0] != NULL ? atol(row[0]) : 0;
        snprintf(unit[n], SM_UNIT_MAX, "%s", row[1] != NULL ? row[1] : "");
        snprintf(action[n], sizeof(action[n]), "%s", row[2] != NULL ? row[2] : "");
        n++;
    }
    mysql_free_result(res);

    for (int i = 0; i < n; i++)
    {
        smDoCommand(db, id[i], unit[i], action[i]);
        done = true;
    }
    if (done || refresh)
        return smRefreshStatus(db);
    return true;
}

/*
 * smSetSystemctl()
 *
 * Sets the path of the systemctl program (used by the tests).
 *
 * Parameters:
 *   path - The path.
 *
 * Returns: nothing.
 */
void smSetSystemctl(const char* path)
{
    smSystemctl = path;
}

/*
 * smSetPowerDelay()
 *
 * Sets how many seconds pass between reporting a power request as done and
 * carrying it out (used by the tests).
 *
 * Parameters:
 *   seconds - The delay.
 *
 * Returns: nothing.
 */
void smSetPowerDelay(unsigned seconds)
{
    smPowerDelayS = seconds;
}

#ifndef SM_TEST
/*
 * smSignal()
 *
 * Signal handler for SIGINT and SIGTERM: asks the main loop to finish.
 *
 * Parameters:
 *   sig - The signal number (not used).
 *
 * Returns: nothing.
 */
static void smSignal(int sig)
{
    (void)sig;
    smStop = 1;
}

/*
 * smConnect()
 *
 * Opens the database connection with the settings of /etc/openmt.ini.
 *
 * Parameters: none.
 *
 * Returns: The connection, or NULL.
 */
static MYSQL* smConnect(void)
{
    char dbhost[128] = "localhost";
    char passwd[128] = "";

    readConfig("database", "host", dbhost);
    readConfig("database", "passwd", passwd);

    MYSQL* db = mysql_init(NULL);

    if (db == NULL)
        return NULL;
    if (mysql_real_connect(db, dbhost, dbUser(), passwd, "", 0, NULL, 0) == NULL)
    {
        fprintf(stderr, "service_manager: cannot connect to the database: %s\n", mysql_error(db));
        mysql_close(db);
        return NULL;
    }
    return db;
}

/*
 * main()
 *
 * Runs the service manager until it is stopped.
 *
 * Parameters:
 *   argc, argv - "-s path" sets the systemctl program.
 *
 * Returns: 0.
 */
int main(int argc, char** argv)
{
    MYSQL*   db = NULL;
    time_t   lastRefresh = 0;
    struct sigaction sa;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc)
            smSystemctl = argv[++i];
    }

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = smSignal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    printf("OpenMT service manager %s\n", SM_VERSION);
    fflush(stdout);

    while (!smStop)
    {
        if (db == NULL)
        {
            db = smConnect();
            if (db == NULL)
            {
                for (int i = 0; i < 50 && !smStop; i++)
                    usleep(100000);
                continue;
            }
            lastRefresh = 0;
        }

        bool refresh = time(NULL) - lastRefresh >= (time(NULL) < smFastUntil ? SM_FAST_STATUS_MS : SM_STATUS_MS) / 1000;

        if (!smCycle(db, refresh))
        {
            mysql_close(db);
            db = NULL;
            continue;
        }
        if (refresh)
            lastRefresh = time(NULL);
        usleep(SM_POLL_US);
    }
    if (db != NULL)
        mysql_close(db);
    return 0;
}
#endif
