/*
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/*
 * Replacement for the dashboard database functions of tools.c so that
 * ysf_host can be run on its own against ysf_sim.
 *
 *   Configuration: every key starts empty and takes its default from the host.
 *                  YSF_STUB_<key>=value in the environment overrides a key,
 *                  YSF_STUB_MODEM selects the modem type (openmt, mmdvm, ...).
 *   active_mode:   read from /tmp/ysf_stub_mode (written by nxdn_sim).
 *   Dashboard:     a command is read from /tmp/ysf_stub_cmd_<command> and
 *                  removed when it is acknowledged.
 *   Database:      calls and history are printed on stdout as "[db] ...".
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

bool saveHistory(const char* modem_name, const char* mode, const char* type, const char* src, const char* suffix,
                 const char* dst, float loss_BER, const char* message, uint16_t duration)
{
    printf("[db] history %s %s %s %s %s -> %s ber %.4f %us\n", modem_name, mode, type, src, suffix, dst,
           loss_BER, duration);
    fflush(stdout);
    return true;
}

bool saveLastCall(uint8_t pos, const char* modem_name, const char* mode, const char* type, const char* src,
                  const char* suffix, const char* dst, const char* message, const char* sms, const char* gps,
                  bool isTx)
{
    printf("[db] lastcall %s %s %s %s %s -> %s %s\n", modem_name, mode, type, src, suffix, dst,
           isTx ? "active" : "ended");
    fflush(stdout);
    return true;
}

static char store[64][3][40];
static int  storeCount = 0;

bool readHostConfig(const char* modem_name, const char* module_name, const char* key, char* value)
{
    char name[80];
    const char* env;

    value[0] = '\0';

    if (strcmp(module_name, "config") == 0 && strcmp(key, "modem") == 0)
    {
        env = getenv("YSF_STUB_MODEM");
        strcpy(value, env != NULL ? env : "openmt");
        return true;
    }

    if (strcmp(module_name, "main") == 0 && strcmp(key, "callsign") == 0)
    {
        strcpy(value, "N0CALL");
        return true;
    }

    snprintf(name, sizeof(name), "YSF_STUB_%s", key);
    env = getenv(name);

    if (env != NULL)
    {
        /* a host's value buffers are as small as 32 bytes: copy only what is there */
        size_t len = strlen(env);

        if (len > 31)
            len = 31;
        memcpy(value, env, len);
        value[len] = '\0';
        return true;
    }

    for (int i = 0; i < storeCount; i++)
    {
        if (strcmp(store[i][0], module_name) == 0 && strcmp(store[i][1], key) == 0)
        {
            strcpy(value, store[i][2]);
            return true;
        }
    }

    return false;
}

bool setHostConfig(const char* modem_name, const char* module_name, const char* key, const char* display_type,
                   const char* value)
{
    if (storeCount < 64)
    {
        strncpy(store[storeCount][0], module_name, 39);
        strncpy(store[storeCount][1], key, 39);
        strncpy(store[storeCount][2], value, 39);
        storeCount++;
    }

    return true;
}

bool addGateway(const char* modem_name, const char* module_name, const char* mode)
{
    printf("[db] gateway added %s\n", mode);
    fflush(stdout);
    return true;
}

bool delGateway(const char* modem_name, const char* module_name, const char* mode)
{
    printf("[db] gateway removed %s\n", mode);
    fflush(stdout);
    return true;
}

bool readDashbCommand(const char* modem_name, const char* command, char* value)
{
    char path[100];
    FILE* f;

    value[0] = '\0';
    snprintf(path, sizeof(path), "/tmp/ysf_stub_cmd_%s", command);
    f = fopen(path, "r");

    if (f != NULL)
    {
        if (fgets(value, 30, f) != NULL)
            value[strcspn(value, "\r\n")] = '\0';
        fclose(f);
    }

    return true;
}

bool ackDashbCommand(const char* modem_name, const char* command, const char* result)
{
    char path[100];

    snprintf(path, sizeof(path), "/tmp/ysf_stub_cmd_%s", command);
    unlink(path);
    printf("[db] ack %s %s\n", command, result);
    fflush(stdout);
    return true;
}

bool setReflectorStatus(const char* modem_name, const char* module, const char* name, bool linked)
{
    printf("[db] reflector %s %s\n", name, linked ? "linked" : "unlinked");
    fflush(stdout);
    return true;
}

bool clearReflLinkStatus(const char* modem_name, const char* module)
{
    return true;
}

bool findDMRId(const unsigned int DMRId, char* callsign, char* name)
{
    if (DMRId == 1234)
    {
        strcpy(callsign, "N0TEST");
        strcpy(name, "Test User");
        return true;
    }

    return false;
}

bool logError(const char* modem_name, const char* module, const char* message)
{
    printf("[db] error %s\n", message);
    fflush(stdout);
    return true;
}

bool readStatus(const char* modem_name, const char* module, const char* property, char* value)
{
    FILE* f = fopen("/tmp/ysf_stub_mode", "r");

    if (f != NULL)
    {
        if (fgets(value, 10, f) != NULL)
            value[strcspn(value, "\r\n")] = '\0';
        fclose(f);
    }

    return true;
}

/* Reflector lookup: nothing in the database, the gateway falls back to its hosts file */
bool findReflector(const char* type, const char* name, char* ip4, uint16_t* port)
{
    (void)type;
    (void)name;
    (void)ip4;
    (void)port;
    return false;
}

/* --- database stubs: YSF_STUB_IDS=1 gives dmr_ids a row for the callsign W1ABC --- */
#include <mariadb/mysql.h>

static MYSQL stubConn;
static int stubMatch;

MYSQL* mysql_init(MYSQL* m)
{
    (void)m;
    return &stubConn;
}

MYSQL* mysql_real_connect(MYSQL* m, const char* h, const char* u, const char* p, const char* d, unsigned int port,
                          const char* sock, unsigned long flags)
{
    (void)h; (void)u; (void)p; (void)d; (void)port; (void)sock; (void)flags;
    return getenv("YSF_STUB_IDS") != NULL ? m : NULL;
}

unsigned long mysql_real_escape_string(MYSQL* m, char* to, const char* from, unsigned long length)
{
    (void)m;
    memcpy(to, from, length);
    to[length] = '\0';
    return length;
}

int mysql_query(MYSQL* m, const char* q)
{
    (void)m;
    stubMatch = strstr(q, "callsign = 'W1ABC'") != NULL;
    return 0;
}

static MYSQL_RES stubRes;

MYSQL_RES* mysql_store_result(MYSQL* m)
{
    (void)m;
    return &stubRes;
}

MYSQL_ROW mysql_fetch_row(MYSQL_RES* r)
{
    static char nm[] = "Ann Test";
    static char* row[1] = { nm };

    (void)r;
    return stubMatch ? row : NULL;
}

void mysql_free_result(MYSQL_RES* r) { (void)r; }
void mysql_close(MYSQL* m) { (void)m; }
const char* mysql_error(MYSQL* m) { (void)m; return "stub"; }

char* readConfig(const char* section, const char* key, char* value)
{
    (void)section;
    (void)key;
    return value;
}
