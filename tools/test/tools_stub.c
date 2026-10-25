/* Created by Rick KD0OSS */
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
 * Replacement for the dashboard database functions of tools.c so that a
 * protocol host or gateway can be run on its own against its simulator.
 * One file for every protocol; the protocol is chosen when it is compiled:
 *
 *   -DSTUB_UC=YSF -DSTUB_LC=ysf      names of the environment variables and /tmp files
 *   -DSTUB_DB                        also stub the MySQL calls (needed by the YSF gateway)
 *   -DSTUB_REAL_TOOLS                leave out the hexDump, modemWriteBits and modemQueueSamples
 *                                    stubs; link tools_real.c (made by mk_tools_real.sh) instead
 *
 * With those example settings:
 *   Configuration: every key starts empty and takes its default from the host.
 *                  YSF_STUB_<key>=value in the environment overrides a key,
 *                  YSF_STUB_MODEM selects the modem type (openmt, mmdvm, ...).
 *   active_mode:   read from /tmp/ysf_stub_mode (written by the simulator).
 *   Dashboard:     a command is read from /tmp/ysf_stub_cmd_<command> and
 *                  removed when it is acknowledged.
 *   Database:      calls and history are printed on stdout as "[db] ...".
 */

#ifndef STUB_UC
#error "tools_stub.c: compile with -DSTUB_UC=<PROTOCOL> -DSTUB_LC=<protocol>, e.g. -DSTUB_UC=YSF -DSTUB_LC=ysf"
#endif
#ifndef STUB_LC
#error "tools_stub.c: compile with -DSTUB_UC=<PROTOCOL> -DSTUB_LC=<protocol>, e.g. -DSTUB_UC=YSF -DSTUB_LC=ysf"
#endif

#define STUB_STR2(x) #x
#define STUB_STR(x)  STUB_STR2(x)
#define STUB_ENV     STUB_STR(STUB_UC) "_STUB_"          /* environment variable prefix */
#define STUB_TMP     "/tmp/" STUB_STR(STUB_LC) "_stub_"  /* file name prefix            */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

/*
 * saveHistory()
 *
 * Stub that prints the call record to stdout as a "[db] history" line
 * instead of writing to the database.
 *
 * Parameters:
 *   modem_name - Unused in this stub.
 *   mode       - Protocol of the call.
 *   type       - Call type.
 *   src        - Source callsign or ID.
 *   suffix     - Callsign suffix.
 *   dst        - Destination callsign, ID or talk group.
 *   loss_BER   - Bit error rate or loss figure.
 *   message    - Unused in this stub.
 *   duration   - Length of the call in seconds.
 *
 * Returns: Always true.
 */
bool saveHistory(const char* modem_name, const char* mode, const char* type, const char* src, const char* suffix,
                 const char* dst, float loss_BER, const char* message, uint16_t duration)
{
    printf("[db] history %s %s %s %s %s -> %s ber %.4f %us\n", modem_name, mode, type, src, suffix, dst,
           loss_BER, duration);
    fflush(stdout);
    return true;
}

/*
 * saveLastCall()
 *
 * Stub that prints a "[db] lastcall" line, showing "active" for a
 * transmission and "ended" for a received call, instead of writing to the
 * database.
 *
 * Parameters:
 *   pos        - Unused in this stub.
 *   modem_name - Unused in this stub.
 *   mode       - Protocol of the call.
 *   type       - Call type.
 *   src        - Source callsign or ID.
 *   suffix     - Callsign suffix.
 *   dst        - Destination callsign, ID or talk group.
 *   message    - Unused in this stub.
 *   sms        - Unused in this stub.
 *   gps        - Unused in this stub.
 *   isTx       - true for a transmission, false for a received call.
 *
 * Returns: Always true.
 */
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

/*
 * readHostConfig()
 *
 * Stub for the host configuration table.  The modem type comes from the
 * environment variable <PROTOCOL>_STUB_MODEM (default "openmt"), the
 * callsign is always N0CALL, any other key comes from <PROTOCOL>_STUB_<key>
 * in the environment (cut to 31 characters), and then from values saved
 * earlier with setHostConfig().
 *
 * Parameters:
 *   modem_name  - Unused in this stub.
 *   module_name - Module name; the stub looks values up by module and key.
 *   key         - Setting name.
 *   value       - Output buffer for the value; it is set to an empty string
 *                 first.
 *
 * Returns: true if a value was found, false if the key has no value (the
 *   host then uses its default).
 */
bool readHostConfig(const char* modem_name, const char* module_name, const char* key, char* value)
{
    char name[80];
    const char* env;

    value[0] = '\0';

    if (strcmp(module_name, "config") == 0 && strcmp(key, "modem") == 0)
    {
        env = getenv(STUB_ENV "MODEM");
        strcpy(value, env != NULL ? env : "openmt");
        return true;
    }

    if (strcmp(module_name, "main") == 0 && strcmp(key, "callsign") == 0)
    {
        strcpy(value, "N0CALL");
        return true;
    }

    /* a value that a test changes while the program runs: the file /tmp/<protocol>_stub_cfg_<key> (first line) */
    {
        char path[100];
        FILE* f;

        snprintf(path, sizeof(path), STUB_TMP "cfg_%s", key);
        f = fopen(path, "r");
        if (f != NULL)
        {
            char line[40] = "";

            if (fgets(line, sizeof(line), f) != NULL)
            {
                line[strcspn(line, "\r\n")] = '\0';
                strncpy(value, line, 31);
                value[31] = '\0';
                fclose(f);
                return true;
            }
            fclose(f);
        }
    }

    snprintf(name, sizeof(name), STUB_ENV "%s", key);
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

/*
 * readHostConfigN()
 *
 * Stub for readHostConfig() with a buffer size.  The value is read as
 * readHostConfig() reads it and cut to the size of the buffer.
 *
 * Parameters:
 *   modem_name  - Unused in this stub.
 *   module_name - Module name.
 *   key         - Setting name.
 *   value       - Output buffer for the value.
 *   size        - Size of the value buffer in bytes.
 *
 * Returns: true if a value was found, false if not.
 */
bool readHostConfigN(const char* modem_name, const char* module_name, const char* key, char* value, size_t size)
{
    char tmp[128];
    bool ret = readHostConfig(modem_name, module_name, key, tmp);

    if (size > 0)
        snprintf(value, size, "%s", tmp);

    return ret;
}

/*
 * setHostConfig()
 *
 * Stub that remembers a setting in memory, up to 64 of them, so that
 * readHostConfig() can return it later.  Values are cut to 39 characters.
 *
 * Parameters:
 *   modem_name   - Unused in this stub.
 *   module_name  - Module name; the stub looks values up by module and key.
 *   key          - Setting name.
 *   display_type - Unused in this stub.
 *   value        - Value to remember.
 *
 * Returns: Always true.
 */
bool setHostConfig(const char* modem_name, const char* module_name, const char* key, const char* display_type,
                   const char* value)
{
    /* with <PROTOCOL>_STUB_PERSIST set the value goes to the file /tmp/<protocol>_stub_cfg_<key> as well, so that a
     * second program (Modem_Host and the calibration tool) sees it */
    if (getenv(STUB_ENV "PERSIST") != NULL)
    {
        char path[100];
        FILE* f;

        snprintf(path, sizeof(path), STUB_TMP "cfg_%s", key);
        f = fopen(path, "w");
        if (f != NULL)
        {
            fprintf(f, "%s\n", value);
            fclose(f);
        }
    }

    for (int i = 0; i < storeCount; i++)
    {
        if (strcmp(store[i][0], module_name) == 0 && strcmp(store[i][1], key) == 0)
        {
            strncpy(store[i][2], value, 39);
            return true;
        }
    }

    if (storeCount < 64)
    {
        strncpy(store[storeCount][0], module_name, 39);
        strncpy(store[storeCount][1], key, 39);
        strncpy(store[storeCount][2], value, 39);
        storeCount++;
    }

    return true;
}

/*
 * addHostConfigDefault()
 *
 * Stub that remembers a setting only if it is not already known, from the
 * environment or from an earlier call, like the real function that adds a
 * missing setting to the database.
 *
 * Parameters:
 *   modem_name   - Unused in this stub.
 *   module_name  - Module name.
 *   key          - Setting name.
 *   display_type - Unused in this stub.
 *   value        - Value to remember when there is none.
 *
 * Returns: Always true.
 */
bool addHostConfigDefault(const char* modem_name, const char* module_name, const char* key, const char* display_type,
                          const char* value)
{
    char tmp[128];

    if (readHostConfig(modem_name, module_name, key, tmp) && strlen(tmp) > 0)
        return true;
    return setHostConfig(modem_name, module_name, key, display_type, value);
}

/*
 * addGateway()
 *
 * Stub that prints a "[db] gateway added" line.
 *
 * Parameters:
 *   modem_name  - Unused in this stub.
 *   module_name - Unused in this stub.
 *   mode        - Gateway name.
 *
 * Returns: Always true.
 */
bool addGateway(const char* modem_name, const char* module_name, const char* mode)
{
    printf("[db] gateway added %s\n", mode);
    fflush(stdout);
    return true;
}

/*
 * delGateway()
 *
 * Stub that prints a "[db] gateway removed" line.
 *
 * Parameters:
 *   modem_name  - Unused in this stub.
 *   module_name - Unused in this stub.
 *   mode        - Gateway name.
 *
 * Returns: Always true.
 */
bool delGateway(const char* modem_name, const char* module_name, const char* mode)
{
    printf("[db] gateway removed %s\n", mode);
    fflush(stdout);
    return true;
}

/*
 * readDashbCommand()
 *
 * Stub for the dashboard command table.  A command is present if the
 * simulator created the file /tmp/<protocol>_stub_cmd_<command>; its first
 * line is the parameter.
 *
 * Parameters:
 *   modem_name - Unused in this stub.
 *   command    - Command name.
 *   value      - Output buffer (at least 30 bytes) for the parameter; empty
 *                if there is no such file.
 *
 * Returns: Always true.
 */
bool readDashbCommand(const char* modem_name, const char* command, char* value)
{
    char path[100];
    FILE* f;

    value[0] = '\0';
    snprintf(path, sizeof(path), STUB_TMP "cmd_%s", command);
    f = fopen(path, "r");

    if (f != NULL)
    {
        if (fgets(value, 30, f) != NULL)
            value[strcspn(value, "\r\n")] = '\0';
        fclose(f);
    }

    return true;
}

/*
 * sendDashbCommand()
 *
 * Stub for the sender side of the dashboard command table: writes the command file that readDashbCommand() looks
 * for, waits until the receiver has acknowledged it (the file is gone) and returns the result that
 * ackDashbCommand() left in /tmp/<protocol>_stub_res_<command>.
 *
 * Returns: true if the command was acknowledged within timeoutSec seconds.
 */
bool sendDashbCommand(const char* modem_name, const char* command, const char* parameter, char* result, size_t size,
                      int timeoutSec)
{
    char path[100];
    char res[100];
    FILE* f;

    if (size > 0)
        result[0] = '\0';

    snprintf(res, sizeof(res), STUB_TMP "res_%s", command);
    unlink(res);
    snprintf(path, sizeof(path), STUB_TMP "cmd_%s", command);
    f = fopen(path, "w");
    if (f == NULL)
        return false;
    fprintf(f, "%s\n", parameter);
    fclose(f);

    for (int i = 0; i < timeoutSec * 10; i++)
    {
        usleep(100000);
        if (access(path, F_OK) != 0)
        {
            f = fopen(res, "r");
            if (f != NULL)
            {
                if (fgets(result, (int)size, f) != NULL)
                    result[strcspn(result, "\r\n")] = '\0';
                fclose(f);
            }
            return true;
        }
    }

    unlink(path);
    return false;
}

/*
 * ackDashbCommand()
 *
 * Stub that removes the command file and prints a "[db] ack" line.
 *
 * Parameters:
 *   modem_name - Unused in this stub.
 *   command    - Command name being acknowledged.
 *   result     - Result text, printed.
 *
 * Returns: Always true.
 */
bool ackDashbCommand(const char* modem_name, const char* command, const char* result)
{
    char path[100];
    FILE* f;

    /* the result, for sendDashbCommand() */
    snprintf(path, sizeof(path), STUB_TMP "res_%s", command);
    f = fopen(path, "w");
    if (f != NULL)
    {
        fprintf(f, "%s\n", result);
        fclose(f);
    }

    snprintf(path, sizeof(path), STUB_TMP "cmd_%s", command);
    unlink(path);
    printf("[db] ack %s %s\n", command, result);
    fflush(stdout);
    return true;
}

/*
 * setReflectorStatus()
 *
 * Stub that prints a "[db] reflector" line saying whether the reflector is
 * linked or unlinked.
 *
 * Parameters:
 *   modem_name - Unused in this stub.
 *   module     - Unused in this stub.
 *   name       - Reflector name.
 *   linked     - true if linked, false if unlinked.
 *
 * Returns: Always true.
 */
bool setReflectorStatus(const char* modem_name, const char* module, const char* name, bool linked)
{
    printf("[db] reflector %s %s\n", name, linked ? "linked" : "unlinked");
    fflush(stdout);
    return true;
}

/*
 * clearReflLinkStatus()
 *
 * Stub that does nothing.
 *
 * Parameters:
 *   modem_name - Unused in this stub.
 *   module     - Unused in this stub.
 *
 * Returns: Always true.
 */
bool clearReflLinkStatus(const char* modem_name, const char* module)
{
    return true;
}

/*
 * findDMRId()
 *
 * Stub DMR ID lookup that knows a single ID, 1234, which is N0TEST, "Test
 * User".
 *
 * Parameters:
 *   DMRId        - DMR radio ID to look up.
 *   callsign     - Receives the callsign for ID 1234.
 *   callsignSize - Size of the callsign buffer.
 *   name         - Receives the operator name for ID 1234.
 *   nameSize     - Size of the name buffer.
 *
 * Returns: true for ID 1234, false for any other ID.
 */
bool findDMRId(const unsigned int DMRId, char* callsign, size_t callsignSize, char* name, size_t nameSize)
{
    if (DMRId == 1234)
    {
        snprintf(callsign, callsignSize, "%s", "N0TEST");
        snprintf(name, nameSize, "%s", "Test User");
        return true;
    }

    return false;
}

/*
 * logError()
 *
 * Stub that prints a "[db] error" line.
 *
 * Parameters:
 *   modem_name - Unused in this stub.
 *   module     - Unused in this stub.
 *   message    - Error text.
 *
 * Returns: Always true.
 */
bool logError(const char* modem_name, const char* module, const char* message)
{
    printf("[db] error %s\n", message);
    fflush(stdout);
    return true;
}

/*
 * reportError()
 *
 * Stub that prints the error to stderr and a "[db] error" line to stdout, as
 * the real one does for the log and the database.
 *
 * Parameters:
 *   modem_name - Unused in this stub.
 *   module     - Module that had the error.
 *   format     - printf format of the message.
 *   ...        - Values for the format.
 *
 * Returns: nothing.
 */
void reportError(const char* modem_name, const char* module, const char* format, ...)
{
    char    message[256];
    va_list args;

    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    fprintf(stderr, "ERROR [%s]: %s\n", module != NULL ? module : "", message);
    printf("[db] error %s\n", message);
    fflush(stdout);
}

/*
 * errorFlush()
 *
 * Stub that does nothing: the stub writes errors at once.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
void errorFlush(void)
{
}

/*
 * readStatus()
 *
 * Stub status read.  The value is the first line of
 * /tmp/<protocol>_stub_mode, which the simulator writes to say what the
 * active mode is.  The buffer is left unchanged if the file does not exist.
 *
 * Parameters:
 *   modem_name - Unused in this stub.
 *   module     - Unused in this stub.
 *   property   - Unused in this stub.
 *   value      - Output buffer (at least 10 bytes) for the value.
 *
 * Returns: Always true.
 */
bool readStatus(const char* modem_name, const char* module, const char* property, char* value)
{
    FILE* f = fopen(STUB_TMP "mode", "r");

    if (f != NULL)
    {
        if (fgets(value, 10, f) != NULL)
            value[strcspn(value, "\r\n")] = '\0';
        fclose(f);
    }

    return true;
}

/*
 * findReflector()
 *
 * Stub reflector lookup.  The database holds nothing, so the gateway falls
 * back to its hosts file.
 *
 * Parameters:
 *   type - Unused in this stub.
 *   name - Unused in this stub.
 *   ip4  - Unused in this stub.
 *   port - Unused in this stub.
 *
 * Returns: Always false.
 */
bool findReflector(const char* type, const char* name, char* ip4, uint16_t* port)
{
    (void)type;
    (void)name;
    (void)ip4;
    (void)port;
    return false;
}

/* --- database stubs: <PROTOCOL>_STUB_IDS=1 gives dmr_ids a row for the callsign W1ABC --- */
#ifdef STUB_DB
#include <mariadb/mysql.h>

static MYSQL stubConn;
static int stubMatch;

/*
 * mysql_init()
 *
 * Stub that returns a static connection object.
 *
 * Parameters:
 *   m - Unused in this stub.
 *
 * Returns: Pointer to the static stub connection.
 */
MYSQL* mysql_init(MYSQL* m)
{
    (void)m;
    return &stubConn;
}

/*
 * mysql_real_connect()
 *
 * Stub connection.  It only succeeds if the environment variable
 * <PROTOCOL>_STUB_IDS is set, which makes the dmr_ids lookup available.
 *
 * Parameters:
 *   m     - Connection object, returned on success.
 *   h     - Unused in this stub.
 *   u     - Unused in this stub.
 *   p     - Unused in this stub.
 *   d     - Unused in this stub.
 *   port  - Unused in this stub.
 *   sock  - Unused in this stub.
 *   flags - Unused in this stub.
 *
 * Returns: m if <PROTOCOL>_STUB_IDS is set, otherwise NULL.
 */
MYSQL* mysql_real_connect(MYSQL* m, const char* h, const char* u, const char* p, const char* d, unsigned int port,
                          const char* sock, unsigned long flags)
{
    (void)h; (void)u; (void)p; (void)d; (void)port; (void)sock; (void)flags;
    return getenv(STUB_ENV "IDS") != NULL ? m : NULL;
}

/*
 * mysql_real_escape_string()
 *
 * Stub that copies the text without escaping anything.
 *
 * Parameters:
 *   m      - Unused in this stub.
 *   to     - Output buffer, which must hold length + 1 bytes.
 *   from   - Text to copy.
 *   length - Number of bytes to copy.
 *
 * Returns: length.
 */
unsigned long mysql_real_escape_string(MYSQL* m, char* to, const char* from, unsigned long length)
{
    (void)m;
    memcpy(to, from, length);
    to[length] = '\0';
    return length;
}

/*
 * mysql_query()
 *
 * Stub query.  It does not run the query; it only records whether the query
 * text contains "callsign = 'W1ABC'", which decides if mysql_fetch_row()
 * returns a row.
 *
 * Parameters:
 *   m - Unused in this stub.
 *   q - Query text to look at.
 *
 * Returns: Always 0 (success).
 */
int mysql_query(MYSQL* m, const char* q)
{
    (void)m;
    stubMatch = strstr(q, "callsign = 'W1ABC'") != NULL;
    return 0;
}

static MYSQL_RES stubRes;

/*
 * mysql_store_result()
 *
 * Stub that returns a static result object.
 *
 * Parameters:
 *   m - Unused in this stub.
 *
 * Returns: Pointer to the static stub result.
 */
MYSQL_RES* mysql_store_result(MYSQL* m)
{
    (void)m;
    return &stubRes;
}

/*
 * mysql_fetch_row()
 *
 * Stub row fetch.  It returns a single row holding the name "Ann Test" if
 * the last query matched W1ABC, otherwise no row.
 *
 * Parameters:
 *   r - Unused in this stub.
 *
 * Returns: The row, or NULL if the last query did not match.
 */
MYSQL_ROW mysql_fetch_row(MYSQL_RES* r)
{
    static char nm[] = "Ann Test";
    static char* row[1] = { nm };

    (void)r;
    return stubMatch ? row : NULL;
}

/*
 * mysql_free_result()
 *
 * Stub that does nothing.
 *
 * Parameters:
 *   r - Unused in this stub.
 *
 * Returns: nothing.
 */
void mysql_free_result(MYSQL_RES* r) { (void)r; }
/*
 * mysql_close()
 *
 * Stub that does nothing.
 *
 * Parameters:
 *   m - Unused in this stub.
 *
 * Returns: nothing.
 */
void mysql_close(MYSQL* m) { (void)m; }
/*
 * mysql_error()
 *
 * Stub error text.
 *
 * Parameters:
 *   m - Unused in this stub.
 *
 * Returns: The text "stub".
 */
const char* mysql_error(MYSQL* m) { (void)m; return "stub"; }
#endif /* STUB_DB */

/*
 * readConfig()
 *
 * Stub for the configuration file reader.  It leaves the value buffer
 * untouched.
 *
 * Parameters:
 *   section - Unused in this stub.
 *   key     - Unused in this stub.
 *   value   - Output buffer, returned unchanged.
 *
 * Returns: The value pointer.
 */
char* readConfig(const char* section, const char* key, char* value)
{
    (void)section;
    (void)key;
    return value;
}

#ifndef STUB_REAL_TOOLS /* with STUB_REAL_TOOLS the real ones from tools.c are linked (see mk_tools_real.sh) */

/*
 * hexDump()
 *
 * Stub that prints nothing.
 *
 * Parameters:
 *   text   - Unused in this stub.
 *   data   - Unused in this stub.
 *   length - Unused in this stub.
 *
 * Returns: nothing.
 */
void hexDump(const char* text, const uint8_t* data, unsigned int length)
{
    (void)text;
    (void)data;
    (void)length;
}

/*
 * modemWriteBits()
 *
 * Stub that sends nothing.
 *
 * Parameters:
 *   fd    - Unused in this stub.
 *   c     - Unused in this stub.
 *   isEOT - Unused in this stub.
 *
 * Returns: nothing.
 */
void modemWriteBits(int fd, uint8_t c, bool isEOT)
{
    (void)fd;
    (void)c;
    (void)isEOT;
}

/*
 * modemQueueSamples()
 *
 * Stub that sends nothing.
 *
 * Parameters:
 *   fd            - Unused in this stub.
 *   samples       - Unused in this stub.
 *   count         - Unused in this stub.
 *   isEOT         - Unused in this stub.
 *   packetDelayUs - Unused in this stub.
 *
 * Returns: nothing.
 */
void modemQueueSamples(int fd, const int16_t* samples, unsigned int count,
                       bool isEOT, uint32_t packetDelayUs)
{
    (void)fd;
    (void)samples;
    (void)count;
    (void)isEOT;
    (void)packetDelayUs;
}

#endif /* STUB_REAL_TOOLS */
