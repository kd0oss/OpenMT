/***************************************************************************
 *   Copyright (C) 2025 by Rick KD0OSS                                     *
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

#include "tools.h"
#include "../modem/mmdvm.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <errno.h>
#include <strings.h>
#include <time.h>
#include <pthread.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <mariadb/mysql.h>
#include "string_builder.h"

#define MAX_LINE_LENGTH 256

const uint8_t BITS_TABLE[] =
{
#   define B2(n) n,     n+1,     n+1,     n+2
#   define B4(n) B2(n), B2(n+1), B2(n+1), B2(n+2)
#   define B6(n) B4(n), B4(n+1), B4(n+1), B4(n+2)
    B6(0), B6(1), B6(1), B6(2)
};

/*
 * countBits8()
 *
 * Counts the bits that are set in a byte using a lookup table.
 *
 * Parameters:
 *   bits - Value to count.
 *
 * Returns: Number of set bits, 0 to 8.
 */
uint8_t countBits8(uint8_t bits)
{
    return BITS_TABLE[bits];
}

/*
 * countBits16()
 *
 * Counts the bits that are set in a 16 bit value, one byte at a time, using
 * the lookup table.
 *
 * Parameters:
 *   bits - Value to count.
 *
 * Returns: Number of set bits, 0 to 16.
 */
uint8_t countBits16(uint16_t bits)
{
    uint8_t* p = (uint8_t*)&bits;
    uint8_t n = 0U;
    n += BITS_TABLE[p[0U]];
    n += BITS_TABLE[p[1U]];
    return n;
}

/*
 * countBits32()
 *
 * Counts the bits that are set in a 32 bit value, one byte at a time, using
 * the lookup table.
 *
 * Parameters:
 *   bits - Value to count.
 *
 * Returns: Number of set bits, 0 to 32.
 */
uint8_t countBits32(uint32_t bits)
{
    uint8_t* p = (uint8_t*)&bits;
    uint8_t n = 0U;
    n += BITS_TABLE[p[0U]];
    n += BITS_TABLE[p[1U]];
    n += BITS_TABLE[p[2U]];
    n += BITS_TABLE[p[3U]];
    return n;
}

/*
 * countBits64()
 *
 * Counts the bits that are set in a 64 bit value, one byte at a time, using
 * the lookup table.
 *
 * Parameters:
 *   bits - Value to count.
 *
 * Returns: Number of set bits, 0 to 64.
 */
uint8_t countBits64(uint64_t bits)
{
    uint8_t* p = (uint8_t*)&bits;
    uint8_t n = 0U;
    n += BITS_TABLE[p[0U]];
    n += BITS_TABLE[p[1U]];
    n += BITS_TABLE[p[2U]];
    n += BITS_TABLE[p[3U]];
    n += BITS_TABLE[p[4U]];
    n += BITS_TABLE[p[5U]];
    n += BITS_TABLE[p[6U]];
    n += BITS_TABLE[p[7U]];
    return n;
}

/*
 * readConfig()
 *
 * Reads one value from the OpenMT configuration file /etc/openmt.ini.  The
 * file is a simple INI file: [section] headers, key=value lines, and lines
 * starting with ';' or '#' are comments.
 *
 * Parameters:
 *   section - Section name without the brackets.
 *   key     - Key to look up inside the section.
 *   value   - Output buffer that receives the value text.  It is not size
 *             limited here, so it must be large enough for the longest
 *             value.  It is left unchanged if the key is not found.
 *
 * Returns: The value pointer if the key was found, or NULL if the file could
 *   not be opened or the key is not in that section.
 */
char* readConfig(const char* section, const char* key, char* value)
{
    /* command line option or environment variable wins over the file */
    if (dbConfigOverride(section, key, value))
        return value;

    const char* path = configFilePath();
    FILE* file = fopen(path, "r");
    if (file == NULL)
    {
        fprintf(stderr, "Error opening configuration file [%s]: %s\n", path, strerror(errno));
        return NULL;
    }

    char line[MAX_LINE_LENGTH];
    char current_section[MAX_LINE_LENGTH] = "";

    while (fgets(line, MAX_LINE_LENGTH, file) != NULL)
    {
        /* Remove newline character if present */
        line[strcspn(line, "\n")] = 0;

        /* Skip comments and empty lines */
        if (line[0] == ';' || line[0] == '#' || line[0] == '\0')
        {
            continue;
        }

        /* Check for section header */
        if (line[0] == '[' && line[strlen(line) - 1] == ']')
        {
            strncpy(current_section, line + 1, strlen(line) - 2);
            current_section[strlen(line) - 2] = '\0';
        }

        /* Check for key-value pair within the correct section */
        if (strcmp(current_section, section) == 0)
        {
            char* equals_pos = strchr(line, '=');
            if (equals_pos != NULL)
            {
                /* Split the line into key and value */
                char current_key[MAX_LINE_LENGTH];
                strncpy(current_key, line, equals_pos - line);
                current_key[equals_pos - line] = '\0';
                char* current_value = equals_pos + 1;

                /* Compare the key and return the value if it matches */
                if (strcmp(current_key, key) == 0)
                {
                    strcpy(value, current_value);
                    fclose(file);
                    return value;
                }
            }
        }
    }

    fclose(file);
    return NULL; /* Key not found */
}

/*
 * splitString()
 *
 * Splits a string at a delimiter character into a string array.  This is a
 * thin wrapper around split_string().
 *
 * Parameters:
 *   input     - NUL terminated string to split.
 *   delimiter - Character that separates the tokens.
 *   result    - String array that receives the tokens.  The caller frees it
 *               with string_array_free().
 *
 * Returns: nothing.
 */
void splitString(const char* input, char delimiter, StringArray* result)
{
    split_string(input, delimiter, result);
}

/*
 * replace_char()
 *
 * Replaces every occurrence of one character with another, in place.
 *
 * Parameters:
 *   str     - Buffer to modify.
 *   length  - Number of bytes of str to scan.
 *   find    - Character to look for.
 *   replace - Character to put in its place.
 *
 * Returns: nothing.
 */
void replace_char(char* str, int length, char find, char replace)
{
    int i;
    for (i = 0; i < length; i++)
    {
        if (str[i] == find)
        {
            str[i] = replace;  /* Replace the character in place */
        }
    }
}

/*
 * sbAppendEscaped()
 *
 * Appends text to a query string builder with the characters that have a
 * special meaning inside a quoted SQL string escaped (quotes, backslashes
 * and so on), so a value can never end the string and change the query.  It
 * must be used for every text value that goes between quotes in a query.
 * The connection must be open, because the escaping depends on its character
 * set.
 *
 * Parameters:
 *   sb   - String builder holding the query.
 *   conn - Open database connection.
 *   str  - Text to append; NULL is treated as an empty string.
 *
 * Returns: nothing.
 */
static void sbAppendEscaped(StringBuilder* sb, MYSQL* conn, const char* str)
{
    size_t len;
    char* escaped;

    if (str == NULL)
    {
        return;
    }

    len = strlen(str);
    escaped = malloc(len * 2U + 1U);

    if (escaped == NULL)
    {
        return;
    }

    mysql_real_escape_string(conn, escaped, str, (unsigned long)len);
    sb_append(sb, escaped);
    free(escaped);
}

/*
 * saveHistory()
 *
 * Adds one finished call to the modem_host.history database table, stamped
 * with the current time.  The database host and password come from the
 * [database] section of /etc/openmt.ini.  An empty message is stored as
 * 'none'.  The program exits if the database connection cannot even be
 * initialised.
 *
 * Parameters:
 *   modem_name - Name of the modem that handled the call.
 *   mode       - Protocol of the call, such as DMR or YSF.
 *   type       - Call type, such as RF or network.
 *   src        - Source callsign or ID.
 *   suffix     - Callsign suffix.
 *   dst        - Destination callsign, ID or talk group.
 *   loss_BER   - Bit error rate or loss figure, stored with one decimal
 *                place.
 *   message    - Slow speed data text, or an empty string for none.
 *   duration   - Length of the call in seconds.
 *
 * Returns: true if the record was sent, false if the connection to the
 *   database failed.
 */
bool saveHistory(const char* modem_name, const char* mode, const char* type, const char* src, const char* suffix,
                const char* dst, float loss_BER, const char* message, uint16_t duration)
{
    char dbhost[128] = "localhost";
    char passwd[128] = "";
    char tmp[10];
    StringBuilder query;

    readConfig("database", "host", dbhost);
    readConfig("database", "passwd", passwd);

    MYSQL *mysql_conn = mysql_init(NULL);

    if (mysql_conn == NULL)
    {
        fprintf(stderr, "saveHistory: mysql_init() failed\n");
        exit(1);
    }

    if (mysql_real_connect(mysql_conn, dbhost, dbUser(), passwd, "", 0, NULL, 0) == NULL)
    {
        fprintf(stderr, "saveHistory: %s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return false;
    }

    sb_init(&query, 512);
    sb_append(&query, "INSERT INTO modem_host.history SET ");
    sb_append(&query, "modem_name = '");
    sbAppendEscaped(&query, mysql_conn, modem_name);
    sb_append(&query, "', ");
    sb_append(&query, "mode = '");
    sbAppendEscaped(&query, mysql_conn, mode);
    sb_append(&query, "', ");
    sb_append(&query, "type = '");
    sbAppendEscaped(&query, mysql_conn, type);
    sb_append(&query, "', ");
    sb_append(&query, "source = '");
    sbAppendEscaped(&query, mysql_conn, src);
    sb_append(&query, "', ");
    sb_append(&query, "suffix = '");
    sbAppendEscaped(&query, mysql_conn, suffix);
    sb_append(&query, "', ");
    sb_append(&query, "destination = '");
    sbAppendEscaped(&query, mysql_conn, dst);
    sb_append(&query, "', ");
    
    sprintf(tmp, "%0.1f", loss_BER);
    sb_append(&query, "loss_ber = ");
    sb_append(&query, tmp);
    sb_append(&query, ", ");
    
    if (message[0] == 0)
    {
        sb_append(&query, "ss_message = 'none', ");
    }
    else
    {
        sb_append(&query, "ss_message = TRIM('");
        sbAppendEscaped(&query, mysql_conn, message);
        sb_append(&query, "'), ");
    }
    
    sprintf(tmp, "%d", duration);
    sb_append(&query, "duration = ");
    sb_append(&query, tmp);
    sb_append(&query, ", ");
    sb_append(&query, "datetime = NOW()");

    mysql_query(mysql_conn, sb_cstr(&query));
    sb_free(&query);
    mysql_close(mysql_conn);
    return true;
}

/*
 * saveLastCall()
 *
 * Stores the most recent call for the dashboard in the modem_host.last_call
 * table.  Position 1 holds the latest received (or sent) call and position 2
 * the one before it; the old entry at the chosen position is replaced.  For
 * a received call, any text message is added to modem_host.sms_messages and
 * any GPS position to modem_host.gps, both linked to the new last_call row.
 *
 * Parameters:
 *   pos        - Which slot to write: 1, or anything else for slot 2.
 *   modem_name - Name of the modem that handled the call.
 *   mode       - Protocol of the call, such as DMR or YSF.
 *   type       - Call type, such as RF or network.
 *   src        - Source callsign or ID.
 *   suffix     - Callsign suffix.
 *   dst        - Destination callsign, ID or talk group.
 *   message    - Slow speed data text, or an empty string for none.
 *   sms        - Text message to store, or NULL for none.
 *   gps        - Position as text "latitude longitude altitude bearing
 *                speed", or an empty string for none.  Must not be NULL.
 *   isTx       - true if the call was transmitted by this station, false if
 *                received.
 *
 * Returns: true on success, false if the database connection or a query
 *   failed.
 */
bool saveLastCall(uint8_t pos, const char* modem_name, const char* mode, const char* type, const char* src, const char* suffix,
                 const char* dst, const char* message, const char* sms, const char* gps, bool isTx)
{
    char dbhost[128] = "localhost";
    char passwd[128] = "";
    StringBuilder query;
    char tmp[12];

    readConfig("database", "host", dbhost);
    readConfig("database", "passwd", passwd);

    MYSQL *mysql_conn = mysql_init(NULL);

    if (mysql_conn == NULL)
    {
        fprintf(stderr, "saveLastCall: mysql_init() failed\n");
        exit(1);
    }

    if (mysql_real_connect(mysql_conn, dbhost, dbUser(), passwd, "", 0, NULL, 0) == NULL)
    {
        fprintf(stderr, "%s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return false;
    }

    sb_init(&query, 512);
    sb_append(&query, "DELETE FROM modem_host.last_call WHERE pos = 2 AND isTx = false");
    mysql_query(mysql_conn, sb_cstr(&query));

    sb_clear(&query);
    sb_append(&query, "DELETE FROM modem_host.last_call WHERE pos = ");
    if (pos == 1)
        sb_append(&query, "1");
    else
        sb_append(&query, "2");
    mysql_query(mysql_conn, sb_cstr(&query));

    sb_clear(&query);
    sb_append(&query, "INSERT INTO modem_host.last_call SET ");
    sb_append(&query, "pos = ");
    if (pos == 1)
        sb_append(&query, "1");
    else
        sb_append(&query, "2");
    sb_append(&query, ", ");
    sb_append(&query, "modem_name = '");
    sbAppendEscaped(&query, mysql_conn, modem_name);
    sb_append(&query, "', ");
    sb_append(&query, "mode = '");
    sbAppendEscaped(&query, mysql_conn, mode);
    sb_append(&query, "', ");
    sb_append(&query, "type = '");
    sbAppendEscaped(&query, mysql_conn, type);
    sb_append(&query, "', ");
    sb_append(&query, "source = '");
    sbAppendEscaped(&query, mysql_conn, src);
    sb_append(&query, "', ");
    sb_append(&query, "suffix = '");
    sbAppendEscaped(&query, mysql_conn, suffix);
    sb_append(&query, "', ");
    sb_append(&query, "destination = '");
    sbAppendEscaped(&query, mysql_conn, dst);
    sb_append(&query, "', ");
    
    if (isTx)
    {
        sb_append(&query, "isTx = true, ");
    }
    
    if (message[0] == 0)
    {
        sb_append(&query, "ss_message = 'none'");
    }
    else
    {
        sb_append(&query, "ss_message = TRIM('");
        sbAppendEscaped(&query, mysql_conn, message);
        sb_append(&query, "')");
    }

    mysql_query(mysql_conn, sb_cstr(&query));

    if (!isTx && (sms != NULL || strlen(gps) > 0))
    {
        sb_clear(&query);
        sb_append(&query, "SELECT id FROM modem_host.last_call WHERE modem_name = '");
        sbAppendEscaped(&query, mysql_conn, modem_name);
        sb_append(&query, "'");
        
        if (mysql_query(mysql_conn, sb_cstr(&query)))
        {
            fprintf(stderr, "saveLastCall: mysql_query failed: %s\n", mysql_error(mysql_conn));
            mysql_close(mysql_conn);
            sb_free(&query);
            return false;
        }

        MYSQL_RES *result;
        result = mysql_store_result(mysql_conn);
        if (result == NULL)
        {
            fprintf(stderr, "saveLastCall: failed: %s\n", mysql_error(mysql_conn));
            mysql_close(mysql_conn);
            sb_free(&query);
            return false;
        }

        int id = 0;
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(result)) != NULL)
        {
            id = atoi(row[0]);
        }

        mysql_free_result(result);

        if (sms != NULL)
        {
            sb_clear(&query);
            sb_append(&query, "INSERT INTO modem_host.sms_messages SET id = ");
            sprintf(tmp, "%d", id);
            sb_append(&query, tmp);
            sb_append(&query, ", ");
            sb_append(&query, "source = '");
            sbAppendEscaped(&query, mysql_conn, src);
            sb_append(&query, "', ");
            sb_append(&query, "message = '");
            sbAppendEscaped(&query, mysql_conn, sms);
            sb_append(&query, "', ");
            sb_append(&query, "datetime = NOW()");
            mysql_query(mysql_conn, sb_cstr(&query));
        }

        if (strlen(gps) > 0)
        {
            float latitude = 0.0f;
            float longitude = 0.0f;
            int   altitude = 0;
            int   bearing = 0;
            int   speed = 0;

            sscanf(gps, "%f %f %d %d %d", &latitude, &longitude, &altitude, &bearing, &speed);
            addGPS(id, latitude, longitude, altitude, bearing, speed);
        }
    }
    
    sb_free(&query);
    mysql_close(mysql_conn);
    return true;
}

/*
 * setHostConfig()
 *
 * Creates or updates one setting in the modem_host.config table.  If a row
 * for the modem, module and key already exists its value is updated,
 * otherwise a new row is added.
 *
 * Parameters:
 *   modem_name   - Name of the modem the setting belongs to.
 *   module_name  - Module the setting belongs to.
 *   key          - Setting name (the parameter column).
 *   display_type - How the dashboard shows the setting, used when a new row
 *                  is created.
 *   value        - New value for the setting.
 *
 * Returns: true on success, false if the database connection or a query
 *   failed.
 */
bool setHostConfig(const char* modem_name, const char* module_name, const char* key, const char* display_type, const char* value)
{
    char dbhost[128] = "localhost";
    char passwd[128] = "";
    StringBuilder query;

    readConfig("database", "host", dbhost);
    readConfig("database", "passwd", passwd);

    MYSQL *mysql_conn = mysql_init(NULL);

    if (mysql_conn == NULL)
    {
        fprintf(stderr, "setHostConfig: mysql_init() failed\n");
        exit(1);
    }

    if (mysql_real_connect(mysql_conn, dbhost, dbUser(), passwd, "", 0, NULL, 0) == NULL)
    {
        fprintf(stderr, "%s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return false;
    }

    sb_init(&query, 512);
    sb_append(&query, "SELECT * FROM modem_host.config WHERE modem_name = '");
    sbAppendEscaped(&query, mysql_conn, modem_name);
    sb_append(&query, "' AND module = '");
    sbAppendEscaped(&query, mysql_conn, module_name);
    sb_append(&query, "' AND parameter = '");
    sbAppendEscaped(&query, mysql_conn, key);
    sb_append(&query, "'");
    
    if (mysql_query(mysql_conn, sb_cstr(&query)))
    {
        fprintf(stderr, "setHostConfig: mysql_query failed: %s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        sb_free(&query);
        return false;
    }

    MYSQL_RES *result;
    result = mysql_store_result(mysql_conn);
    if (result == NULL)
    {
        fprintf(stderr, "setHostConfig: failed: %s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        sb_free(&query);
        return false;
    }

    if (mysql_num_rows(result) == 0)
    {
        sb_clear(&query);
        sb_append(&query, "INSERT INTO modem_host.config SET modem_name = '");
        sbAppendEscaped(&query, mysql_conn, modem_name);
        sb_append(&query, "', module = '");
        sbAppendEscaped(&query, mysql_conn, module_name);
        sb_append(&query, "', parameter = '");
        sbAppendEscaped(&query, mysql_conn, key);
        sb_append(&query, "', ");
        sb_append(&query, "value = '");
        sbAppendEscaped(&query, mysql_conn, value);
        sb_append(&query, "', display_type = '");
        sbAppendEscaped(&query, mysql_conn, display_type);
        sb_append(&query, "'");
        mysql_query(mysql_conn, sb_cstr(&query));
    }
    else
    {
        sb_clear(&query);
        sb_append(&query, "UPDATE modem_host.config SET value = '");
        sbAppendEscaped(&query, mysql_conn, value);
        sb_append(&query, "', display_type = '");
        sbAppendEscaped(&query, mysql_conn, display_type);
        sb_append(&query, "' WHERE modem_name = '");
        sbAppendEscaped(&query, mysql_conn, modem_name);
        sb_append(&query, "' AND module = '");
        sbAppendEscaped(&query, mysql_conn, module_name);
        sb_append(&query, "' AND parameter = '");
        sbAppendEscaped(&query, mysql_conn, key);
        sb_append(&query, "'");
        mysql_query(mysql_conn, sb_cstr(&query));
    }
    
    mysql_free_result(result);
    sb_free(&query);
    mysql_close(mysql_conn);
    return true;
}

/*
 * readHostConfigN()
 *
 * Reads one setting from the modem_host.config table into a buffer of known
 * size.  A value that is too long is cut to fit and ends with a zero byte.
 *
 * Parameters:
 *   modem_name  - Name of the modem the setting belongs to.
 *   module_name - Module the setting belongs to.
 *   key         - Setting name (the parameter column).
 *   value       - Output buffer that receives the value.  It is set to an
 *                 empty string first, so it stays empty if the setting does
 *                 not exist.
 *   size        - Size of the value buffer in bytes.
 *
 * Returns: true if the query ran, false if the database connection or the
 *   query failed.
 */
bool readHostConfigN(const char* modem_name, const char* module_name, const char* key, char* value, size_t size)
{
    char dbhost[128] = "localhost";
    char passwd[128] = "";
    StringBuilder query;

    readConfig("database", "host", dbhost);
    readConfig("database", "passwd", passwd);

    MYSQL *mysql_conn = mysql_init(NULL);

    if (mysql_conn == NULL)
    {
        fprintf(stderr, "readHostConfig: mysql_init() failed\n");
        exit(1);
    }

    if (mysql_real_connect(mysql_conn, dbhost, dbUser(), passwd, "", 0, NULL, 0) == NULL)
    {
        fprintf(stderr, "%s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return false;
    }

    sb_init(&query, 512);
    sb_append(&query, "SELECT value FROM modem_host.config WHERE modem_name = '");
    sbAppendEscaped(&query, mysql_conn, modem_name);
    sb_append(&query, "' AND module = '");
    sbAppendEscaped(&query, mysql_conn, module_name);
    sb_append(&query, "' AND parameter = '");
    sbAppendEscaped(&query, mysql_conn, key);
    sb_append(&query, "'");
    
    if (mysql_query(mysql_conn, sb_cstr(&query)))
    {
        fprintf(stderr, "readHostConfig: mysql_query failed: %s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        sb_free(&query);
        return false;
    }

    if (size > 0)
        value[0] = '\0';
    MYSQL_RES *result;
    result = mysql_store_result(mysql_conn);
    if (result == NULL)
    {
        fprintf(stderr, "readHostConfig: failed: %s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
    }
    else
    {
        MYSQL_ROW row;
        if ((row = mysql_fetch_row(result)) != NULL)
        {
            if (size > 0)
                snprintf(value, size, "%s", row[0] != NULL ? row[0] : "");
        }
        mysql_free_result(result);
    }
    
    sb_free(&query);
    mysql_close(mysql_conn);
    return true;
}

/*
 * readHostConfig()
 *
 * Reads one setting from the modem_host.config table into a buffer whose
 * size the caller does not give.  Use readHostConfigN() where the size is
 * known.
 *
 * Parameters:
 *   modem_name  - Name of the modem the setting belongs to.
 *   module_name - Module the setting belongs to.
 *   key         - Setting name (the parameter column).
 *   value       - Output buffer that receives the value.  It is set to an
 *                 empty string first, so it stays empty if the setting does
 *                 not exist.
 *
 * Returns: true if the query ran, false if the database connection or the
 *   query failed.
 */
bool readHostConfig(const char* modem_name, const char* module_name, const char* key, char* value)
{
    return readHostConfigN(modem_name, module_name, key, value, (size_t)0x7fffffff);
}

/*
 * hostConfigDigest()
 *
 * Gives one number that changes whenever any setting of a module of the
 * modem is added, removed or changed.  Modem_Host compares it from time to
 * time to find out that the bridge has to read its settings again.  It is
 * the sum of a CRC of every "parameter=value" row, so the order of the rows
 * does not matter and the length of the result does not depend on how many
 * settings there are.
 *
 * Parameters:
 *   modem_name  - Name of the modem.
 *   module_name - Module to watch, normally "config".
 *   digest      - Receives the number.
 *
 * Returns: true if the number was read, false if the database could not be
 *   reached (digest is left alone then).
 */
bool hostConfigDigest(const char* modem_name, const char* module_name, uint64_t* digest)
{
    char dbhost[128] = "localhost";
    char passwd[128] = "";
    StringBuilder query;
    bool ok = false;

    readConfig("database", "host", dbhost);
    readConfig("database", "passwd", passwd);

    MYSQL *mysql_conn = mysql_init(NULL);

    if (mysql_conn == NULL)
        return false;

    if (mysql_real_connect(mysql_conn, dbhost, dbUser(), passwd, "", 0, NULL, 0) == NULL)
    {
        mysql_close(mysql_conn);
        return false;
    }

    sb_init(&query, 512);
    sb_append(&query, "SELECT COALESCE(SUM(CRC32(CONCAT(parameter, '=', COALESCE(value, '')))), 0) FROM modem_host.config WHERE modem_name = '");
    sbAppendEscaped(&query, mysql_conn, modem_name);
    sb_append(&query, "' AND module = '");
    sbAppendEscaped(&query, mysql_conn, module_name);
    sb_append(&query, "'");

    if (mysql_query(mysql_conn, sb_cstr(&query)) == 0)
    {
        MYSQL_RES *result = mysql_store_result(mysql_conn);

        if (result != NULL)
        {
            MYSQL_ROW row = mysql_fetch_row(result);

            if (row != NULL && row[0] != NULL)
            {
                *digest = strtoull(row[0], NULL, 10);
                ok = true;
            }
            mysql_free_result(result);
        }
    }

    sb_free(&query);
    mysql_close(mysql_conn);
    return ok;
}

/*
 * addMode()
 *
 * Adds a protocol mode to the comma separated activeModes setting of a
 * module, unless it is already listed.  A setting of "none" or an empty one
 * is replaced by the mode.
 *
 * Parameters:
 *   modem_name  - Name of the modem.
 *   module_name - Module whose activeModes setting is changed.
 *   mode        - Mode name to add, such as DMR.
 *
 * Returns: Always true.  The result of the database update is not checked.
 */
bool addMode(const char* modem_name, const char* module_name, const char* mode)
{
    char tmp[256];
    StringArray modeList;
    size_t i;
    StringBuilder newModes;

    readHostConfig(modem_name, module_name, "activeModes", tmp);
    
    if (strcmp(tmp, "none") == 0 || strlen(tmp) == 0)
    {
        setHostConfig(modem_name, module_name, "activeModes", "none", mode);
    }
    else
    {
        splitString(tmp, ',', &modeList);
        
        for (i = 0; i < modeList.count; i++)
        {
            if (strcmp(modeList.tokens[i], mode) == 0)
            {
                string_array_free(&modeList);
                return true;
            }
        }
        
        sb_init(&newModes, 256);
        sb_append(&newModes, tmp);
        sb_append(&newModes, ",");
        sb_append(&newModes, mode);
        
        setHostConfig(modem_name, module_name, "activeModes", "none", sb_cstr(&newModes));
        
        sb_free(&newModes);
        string_array_free(&modeList);
    }
    return true;
}

/*
 * delMode()
 *
 * Removes a protocol mode from the comma separated activeModes setting of a
 * module.  If no modes are left the setting becomes "none".
 *
 * Parameters:
 *   modem_name  - Name of the modem.
 *   module_name - Module whose activeModes setting is changed.
 *   mode        - Mode name to remove.
 *
 * Returns: Always true.  The result of the database update is not checked.
 */
bool delMode(const char* modem_name, const char* module_name, const char* mode)
{
    char tmp[256];
    StringArray modeList;
    StringBuilder newModes;
    size_t i;
    bool first = true;

    readHostConfig(modem_name, module_name, "activeModes", tmp);
    
    if (strcmp(tmp, "none") == 0 || strlen(tmp) == 0)
    {
        return true;
    }

    splitString(tmp, ',', &modeList);
    sb_init(&newModes, 256);
    
    for (i = 0; i < modeList.count; i++)
    {
        if (strcmp(modeList.tokens[i], mode) != 0)
        {
            if (!first)
            {
                sb_append(&newModes, ",");
            }
            sb_append(&newModes, modeList.tokens[i]);
            first = false;
        }
    }
    
    if (sb_length(&newModes) == 0)
    {
        setHostConfig(modem_name, module_name, "activeModes", "none", "none");
    }
    else
    {
        setHostConfig(modem_name, module_name, "activeModes", "none", sb_cstr(&newModes));
    }
    
    sb_free(&newModes);
    string_array_free(&modeList);
    return true;
}

/*
 * addGateway()
 *
 * Adds a gateway to the comma separated gateways setting of a module, unless
 * it is already listed.  A setting of "none" or an empty one is replaced by
 * the gateway.
 *
 * Parameters:
 *   modem_name  - Name of the modem.
 *   module_name - Module whose gateways setting is changed.
 *   mode        - Gateway (protocol) name to add.
 *
 * Returns: Always true.  The result of the database update is not checked.
 */
bool addGateway(const char* modem_name, const char* module_name, const char* mode)
{
    char tmp[256];
    StringArray modeList;
    size_t i;
    StringBuilder newModes;

    readHostConfig(modem_name, module_name, "gateways", tmp);
    
    if (strcmp(tmp, "none") == 0 || strlen(tmp) == 0)
    {
        setHostConfig(modem_name, module_name, "gateways", "none", mode);
    }
    else
    {
        splitString(tmp, ',', &modeList);
        
        for (i = 0; i < modeList.count; i++)
        {
            if (strcmp(modeList.tokens[i], mode) == 0)
            {
                string_array_free(&modeList);
                return true;
            }
        }
        
        sb_init(&newModes, 256);
        sb_append(&newModes, tmp);
        sb_append(&newModes, ",");
        sb_append(&newModes, mode);
        
        setHostConfig(modem_name, module_name, "gateways", "none", sb_cstr(&newModes));
        
        sb_free(&newModes);
        string_array_free(&modeList);
    }
    return true;
}

/*
 * delGateway()
 *
 * Removes a gateway from the comma separated gateways setting of a module.
 * If none are left the setting becomes "none".
 *
 * Parameters:
 *   modem_name  - Name of the modem.
 *   module_name - Module whose gateways setting is changed.
 *   mode        - Gateway (protocol) name to remove.
 *
 * Returns: Always true.  The result of the database update is not checked.
 */
bool delGateway(const char* modem_name, const char* module_name, const char* mode)
{
    char tmp[256];
    StringArray modeList;
    StringBuilder newModes;
    size_t i;
    bool first = true;

    readHostConfig(modem_name, module_name, "gateways", tmp);
    
    if (strcmp(tmp, "none") == 0 || strlen(tmp) == 0)
    {
        return true;
    }

    splitString(tmp, ',', &modeList);
    sb_init(&newModes, 256);
    
    for (i = 0; i < modeList.count; i++)
    {
        if (strcmp(modeList.tokens[i], mode) != 0)
        {
            if (!first)
            {
                sb_append(&newModes, ",");
            }
            sb_append(&newModes, modeList.tokens[i]);
            first = false;
        }
    }
    
    if (sb_length(&newModes) == 0)
    {
        setHostConfig(modem_name, module_name, "gateways", "none", "none");
    }
    else
    {
        setHostConfig(modem_name, module_name, "gateways", "none", sb_cstr(&newModes));
    }
    
    sb_free(&newModes);
    string_array_free(&modeList);
    return true;
}

/*
 * addHostConfigDefault()
 *
 * Adds one setting to the modem_host.config table only if it is not there
 * yet.  An existing value is never changed, so it can be called at every
 * start-up to give older installations the settings added later.
 *
 * Parameters:
 *   modem_name   - Name of the modem the setting belongs to.
 *   module_name  - Module the setting belongs to.
 *   key          - Setting name (the parameter column).
 *   display_type - How the dashboard shows the setting.
 *   value        - Value to store when the setting does not exist.
 *
 * Returns: true if the setting exists afterwards (added or already there),
 *          false if the database connection or the query failed.
 */
bool addHostConfigDefault(const char* modem_name, const char* module_name, const char* key, const char* display_type, const char* value)
{
    char dbhost[128] = "localhost";
    char passwd[128] = "";
    StringBuilder query;
    bool ok;

    readConfig("database", "host", dbhost);
    readConfig("database", "passwd", passwd);

    MYSQL *mysql_conn = mysql_init(NULL);

    if (mysql_conn == NULL)
    {
        fprintf(stderr, "addHostConfigDefault: mysql_init() failed\n");
        exit(1);
    }

    if (mysql_real_connect(mysql_conn, dbhost, dbUser(), passwd, "", 0, NULL, 0) == NULL)
    {
        fprintf(stderr, "%s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return false;
    }

    sb_init(&query, 512);
    sb_append(&query, "INSERT INTO modem_host.config (modem_name, module, parameter, value, display_type) SELECT '");
    sbAppendEscaped(&query, mysql_conn, modem_name);
    sb_append(&query, "', '");
    sbAppendEscaped(&query, mysql_conn, module_name);
    sb_append(&query, "', '");
    sbAppendEscaped(&query, mysql_conn, key);
    sb_append(&query, "', '");
    sbAppendEscaped(&query, mysql_conn, value);
    sb_append(&query, "', '");
    sbAppendEscaped(&query, mysql_conn, display_type);
    sb_append(&query, "' FROM DUAL WHERE NOT EXISTS (SELECT 1 FROM modem_host.config WHERE modem_name = '");
    sbAppendEscaped(&query, mysql_conn, modem_name);
    sb_append(&query, "' AND module = '");
    sbAppendEscaped(&query, mysql_conn, module_name);
    sb_append(&query, "' AND parameter = '");
    sbAppendEscaped(&query, mysql_conn, key);
    sb_append(&query, "')");

    ok = (mysql_query(mysql_conn, sb_cstr(&query)) == 0);
    if (!ok)
        fprintf(stderr, "addHostConfigDefault: mysql_query failed: %s\n", mysql_error(mysql_conn));

    sb_free(&query);
    mysql_close(mysql_conn);
    return ok;
}

/*
 * readDashbCommand()
 *
 * Fetches the parameter of a dashboard command from the
 * modem_host.dashb_commands table.  When the command is found its parameter
 * is changed to 'processing' in the table, so the dashboard can see it was
 * picked up (a second read returns the text 'processing').
 *
 * Parameters:
 *   modem_name - Name of the modem whose commands are read.
 *   command    - Command name to look for.
 *   value      - Output buffer that receives the command's parameter, or an
 *                empty string if there is no such command waiting.
 *
 * Returns: true if the query ran, with value empty when the command is not
 *          present; false if the database connection or the query failed.
 */
bool readDashbCommand(const char* modem_name, const char* command, char* value)
{
    char dbhost[128] = "localhost";
    char passwd[128] = "";
    StringBuilder query;

    readConfig("database", "host", dbhost);
    readConfig("database", "passwd", passwd);

    MYSQL *mysql_conn = mysql_init(NULL);

    if (mysql_conn == NULL)
    {
        fprintf(stderr, "DashCom: mysql_init() failed\n");
        exit(1);
    }

    if (mysql_real_connect(mysql_conn, dbhost, dbUser(), passwd, "", 0, NULL, 0) == NULL)
    {
        fprintf(stderr, "%s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return false;
    }

    sb_init(&query, 512);
    sb_append(&query, "SELECT parameter FROM modem_host.dashb_commands WHERE modem_name = '");
    sbAppendEscaped(&query, mysql_conn, modem_name);
    sb_append(&query, "' AND command = '");
    sbAppendEscaped(&query, mysql_conn, command);
    sb_append(&query, "'");
    
    if (mysql_query(mysql_conn, sb_cstr(&query)))
    {
        fprintf(stderr, "DashCom: mysql_query failed: %s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        sb_free(&query);
        return false;
    }

    MYSQL_RES *result;
    result = mysql_store_result(mysql_conn);
    if (result == NULL)
    {
        fprintf(stderr, "DashCom: failed: %s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        sb_free(&query);
        return false;
    }

    MYSQL_ROW row;
    if ((row = mysql_fetch_row(result)) != NULL)
    {
        /* Copy the text out before the result set is released. */
        strcpy(value, (row[0] != NULL) ? row[0] : "");
        mysql_free_result(result);

        sb_clear(&query);
        sb_append(&query, "UPDATE modem_host.dashb_commands SET parameter = 'processing' WHERE modem_name = '");
        sbAppendEscaped(&query, mysql_conn, modem_name);
        sb_append(&query, "' AND command = '");
        sbAppendEscaped(&query, mysql_conn, command);
        sb_append(&query, "'");
        mysql_query(mysql_conn, sb_cstr(&query));
        mysql_close(mysql_conn);
        sb_free(&query);
        return true;
    }

    mysql_free_result(result);
    mysql_close(mysql_conn);
    sb_free(&query);
    strcpy(value, "");
    return true;
}

/*
 * ackDashbCommand()
 *
 * Records the result of a dashboard command so the dashboard can show it.
 *
 * Parameters:
 *   modem_name - Name of the modem the command was for.
 *   command    - Command name being acknowledged.
 *   result     - Result text to store.
 *
 * Returns: true on success, false if the database connection or the update
 *   failed.
 */
bool ackDashbCommand(const char* modem_name, const char* command, const char* result)
{
    char dbhost[128] = "localhost";
    char passwd[128] = "";
    StringBuilder query;

    readConfig("database", "host", dbhost);
    readConfig("database", "passwd", passwd);

    MYSQL *mysql_conn = mysql_init(NULL);
    if (mysql_conn == NULL)
    {
        fprintf(stderr, "AckDash: mysql_init() failed\n");
        exit(1);
    }

    if (mysql_real_connect(mysql_conn, dbhost, dbUser(), passwd, "", 0, NULL, 0) == NULL)
    {
        fprintf(stderr, "AckDash: %s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return false;
    }

    /* the result column holds 80 characters, a longer text makes the update fail */
    char shortResult[80];
    strncpy(shortResult, result, sizeof(shortResult) - 1);
    shortResult[sizeof(shortResult) - 1] = '\0';

    sb_init(&query, 512);
    sb_append(&query, "UPDATE modem_host.dashb_commands SET result = '");
    sbAppendEscaped(&query, mysql_conn, shortResult);
    sb_append(&query, "' WHERE modem_name = '");
    sbAppendEscaped(&query, mysql_conn, modem_name);
    sb_append(&query, "' AND command = '");
    sbAppendEscaped(&query, mysql_conn, command);
    sb_append(&query, "'");

    if (mysql_query(mysql_conn, sb_cstr(&query)))
    {
        fprintf(stderr, "ackDashbCommand: mysql_query failed: %s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        sb_free(&query);
        return false;
    }

    sb_free(&query);
    mysql_close(mysql_conn);
    return true;
}

/*
 * clearDashbCommands()
 *
 * Deletes every pending dashboard command for a modem.
 *
 * Parameters:
 *   modem_name - Name of the modem whose commands are cleared.
 *
 * Returns: true on success, false if the database connection or the delete
 *   failed.
 */
bool clearDashbCommands(const char* modem_name)
{
    char dbhost[128] = "localhost";
    char passwd[128] = "";
    StringBuilder query;

    readConfig("database", "host", dbhost);
    readConfig("database", "passwd", passwd);

    MYSQL *mysql_conn = mysql_init(NULL);
    if (mysql_conn == NULL)
    {
        fprintf(stderr, "ClearDash: mysql_init() failed\n");
        exit(1);
    }

    if (mysql_real_connect(mysql_conn, dbhost, dbUser(), passwd, "", 0, NULL, 0) == NULL)
    {
        fprintf(stderr, "%s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return false;
    }

    sb_init(&query, 512);
    sb_append(&query, "DELETE FROM modem_host.dashb_commands WHERE modem_name = '");
    sbAppendEscaped(&query, mysql_conn, modem_name);
    sb_append(&query, "'");
    
    if (mysql_query(mysql_conn, sb_cstr(&query)))
    {
        fprintf(stderr, "clearDashbCommands: mysql_query failed: %s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        sb_free(&query);
        return false;
    }

    sb_free(&query);
    mysql_close(mysql_conn);
    return true;
}

/*
 * sendDashbCommand()
 *
 * Puts a command into the modem_host.dashb_commands table, the same way the
 * dashboard does, waits for the program that owns the command to
 * acknowledge it, and removes the row again.
 *
 * Parameters:
 *   modem_name  - Name of the modem the command is for.
 *   command     - Command name ("modemHost" for Modem_Host).
 *   parameter   - Command parameter.
 *   result      - Output buffer for the result text the receiver stored.
 *   size        - Size of result in bytes.
 *   timeoutSec  - How long to wait for the acknowledgement.
 *
 * Returns: true if the command was acknowledged (result holds the text), false
 *   if the database failed or there was no answer within the time (result is
 *   then empty).
 */
bool sendDashbCommand(const char* modem_name, const char* command, const char* parameter, char* result, size_t size,
                      int timeoutSec)
{
    char dbhost[128] = "localhost";
    char passwd[128] = "";
    StringBuilder query;
    bool answered = false;

    if (size > 0)
        result[0] = '\0';

    readConfig("database", "host", dbhost);
    readConfig("database", "passwd", passwd);

    MYSQL *mysql_conn = mysql_init(NULL);
    if (mysql_conn == NULL)
    {
        fprintf(stderr, "SendDash: mysql_init() failed\n");
        return false;
    }

    if (mysql_real_connect(mysql_conn, dbhost, dbUser(), passwd, "", 0, NULL, 0) == NULL)
    {
        fprintf(stderr, "SendDash: %s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return false;
    }

    sb_init(&query, 512);
    sb_append(&query, "INSERT INTO modem_host.dashb_commands SET modem_name = '");
    sbAppendEscaped(&query, mysql_conn, modem_name);
    sb_append(&query, "', command = '");
    sbAppendEscaped(&query, mysql_conn, command);
    sb_append(&query, "', parameter = '");
    sbAppendEscaped(&query, mysql_conn, parameter);
    sb_append(&query, "'");

    if (mysql_query(mysql_conn, sb_cstr(&query)))
    {
        fprintf(stderr, "sendDashbCommand: mysql_query failed: %s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        sb_free(&query);
        return false;
    }

    unsigned long long id = mysql_insert_id(mysql_conn);

    for (int waited = 0; waited < timeoutSec && !answered; waited++)
    {
        sb_clear(&query);
        sb_append(&query, "SELECT result FROM modem_host.dashb_commands WHERE id = ");
        char num[32];
        snprintf(num, sizeof(num), "%llu", id);
        sb_append(&query, num);

        if (mysql_query(mysql_conn, sb_cstr(&query)) == 0)
        {
            MYSQL_RES *res = mysql_store_result(mysql_conn);

            if (res != NULL)
            {
                MYSQL_ROW row = mysql_fetch_row(res);

                if (row != NULL && row[0] != NULL)
                {
                    snprintf(result, size, "%s", row[0]);
                    answered = true;
                }
                mysql_free_result(res);
            }
        }

        if (!answered)
            sleep(1);
    }

    sb_clear(&query);
    sb_append(&query, "DELETE FROM modem_host.dashb_commands WHERE id = ");
    {
        char num[32];
        snprintf(num, sizeof(num), "%llu", id);
        sb_append(&query, num);
    }
    mysql_query(mysql_conn, sb_cstr(&query));

    sb_free(&query);
    mysql_close(mysql_conn);
    return answered;
}

/*
 * clearReflectorList()
 *
 * Deletes every reflector of one type from the modem_host.reflectors table,
 * ready for the list to be reloaded.
 *
 * Parameters:
 *   type - Reflector type to clear, such as DMR, YSF or NXDN.
 *
 * Returns: true on success, false if the database connection or the delete
 *   failed.
 */
bool clearReflectorList(const char* type)
{
   char dbhost[128] = "localhost";
    char passwd[128] = "";
    StringBuilder query;

    readConfig("database", "host", dbhost);
    readConfig("database", "passwd", passwd);

    MYSQL *mysql_conn = mysql_init(NULL);
    if (mysql_conn == NULL)
    {
        fprintf(stderr, "clearReflectorList: mysql_init() failed\n");
        exit(1);
    }

    if (mysql_real_connect(mysql_conn, dbhost, dbUser(), passwd, "", 0, NULL, 0) == NULL)
    {
        fprintf(stderr, "%s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return false;
    }

    sb_init(&query, 512);
    sb_append(&query, "DELETE FROM modem_host.reflectors WHERE refl_type = '");
    sbAppendEscaped(&query, mysql_conn, type);
    sb_append(&query, "'");

    if (mysql_query(mysql_conn, sb_cstr(&query)))
    {
        fprintf(stderr, "clearReflectorList: mysql_query failed: %s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        sb_free(&query);
        return false;
    }

    sb_free(&query);
    mysql_close(mysql_conn);
    return true;
}

/*
 * delReflector()
 *
 * Deletes one reflector from the modem_host.reflectors table.
 *
 * Parameters:
 *   type - Reflector type, such as DMR, YSF or NXDN.
 *   name - Name of the reflector to delete.
 *
 * Returns: true on success, false if the database connection or the delete
 *   failed.
 */
bool delReflector(const char* type, const char* name)
{
   char dbhost[128] = "localhost";
    char passwd[128] = "";
    StringBuilder query;

    readConfig("database", "host", dbhost);
    readConfig("database", "passwd", passwd);

    MYSQL *mysql_conn = mysql_init(NULL);
    if (mysql_conn == NULL)
    {
        fprintf(stderr, "delReflector: mysql_init() failed\n");
        exit(1);
    }

    if (mysql_real_connect(mysql_conn, dbhost, dbUser(), passwd, "", 0, NULL, 0) == NULL)
    {
        fprintf(stderr, "%s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return false;
    }

    sb_init(&query, 512);
    sb_append(&query, "DELETE FROM modem_host.reflectors WHERE refl_type = '");
    sbAppendEscaped(&query, mysql_conn, type);
    sb_append(&query, "' AND Nick = '");
    sbAppendEscaped(&query, mysql_conn, name);
    sb_append(&query, "'");

    if (mysql_query(mysql_conn, sb_cstr(&query)))
    {
        fprintf(stderr, "delReflector: mysql_query failed: %s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        sb_free(&query);
        return false;
    }

    sb_free(&query);
    mysql_close(mysql_conn);
    return true;
}

/*
 * addReflector()
 *
 * Adds one reflector to the modem_host.reflectors table.
 *
 * Parameters:
 *   type         - Reflector type, such as DMR, YSF or NXDN.
 *   name         - Reflector name or number.
 *   ip4Addr      - IPv4 address of the reflector.
 *   ip6Addr      - IPv6 address of the reflector, or an empty string.
 *   port         - UDP port of the reflector.
 *   dashboardURL - Web address of the reflector's dashboard.
 *   refl_title   - Descriptive title of the reflector.
 *   country      - Country the reflector is in.
 *
 * Returns: true on success, false if the database connection or the insert
 *   failed.
 */
bool addReflector(const char* type, const char* name, const char* ip4Addr, const char* ip6Addr, uint16_t port,
                  const char* dashboardURL, const char* refl_title, const char* country)
{
   char dbhost[128] = "localhost";
    char passwd[128] = "";
    char tmp[6];
    StringBuilder query;

    readConfig("database", "host", dbhost);
    readConfig("database", "passwd", passwd);

    MYSQL *mysql_conn = mysql_init(NULL);
    if (mysql_conn == NULL)
    {
        fprintf(stderr, "addReflector: mysql_init() failed\n");
        exit(1);
    }

    if (mysql_real_connect(mysql_conn, dbhost, dbUser(), passwd, "", 0, NULL, 0) == NULL)
    {
        fprintf(stderr, "%s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return false;
    }

    sb_init(&query, 512);
    sb_append(&query, "INSERT INTO modem_host.reflectors SET refl_type = '");
    sbAppendEscaped(&query, mysql_conn, type);
    sb_append(&query, "', ");
    sb_append(&query, "Nick = '");
    sbAppendEscaped(&query, mysql_conn, name);
    sb_append(&query, "', ");
    sb_append(&query, "ip4 = '");
    sbAppendEscaped(&query, mysql_conn, ip4Addr);
    sb_append(&query, "', ");
    sb_append(&query, "ip6 = '");
    sbAppendEscaped(&query, mysql_conn, ip6Addr);
    sb_append(&query, "', ");
    
    sprintf(tmp, "%d", port);
    sb_append(&query, "port = ");
    sb_append(&query, tmp);
    sb_append(&query, ", ");
    sb_append(&query, "dash_address = '");
    sbAppendEscaped(&query, mysql_conn, dashboardURL);
    sb_append(&query, "', ");
    sb_append(&query, "Name = '");
    sbAppendEscaped(&query, mysql_conn, refl_title);
    sb_append(&query, "', ");
    sb_append(&query, "Country = '");
    sbAppendEscaped(&query, mysql_conn, country);
    sb_append(&query, "'");

    if (mysql_query(mysql_conn, sb_cstr(&query)))
    {
        fprintf(stderr, "addReflector: mysql_query failed: %s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        sb_free(&query);
        return false;
    }

    sb_free(&query);
    mysql_close(mysql_conn);
    return true;
}

/*
 * findReflector()
 *
 * Looks up the address of a reflector in the modem_host.reflectors table.
 *
 * Parameters:
 *   type - Reflector type, such as DMR, YSF or NXDN.
 *   name - Name of the reflector to find.
 *   ip4  - Output buffer that receives the IPv4 address.
 *   port - Receives the UDP port.
 *
 * Returns: true if the reflector was found, false if it was not found or the
 *   database could not be used.
 */
bool findReflector(const char* type, const char* name, char* ip4, uint16_t* port)
{
    char dbhost[128] = "localhost";
    char passwd[128] = "";
    StringBuilder query;

    readConfig("database", "host", dbhost);
    readConfig("database", "passwd", passwd);

    MYSQL *mysql_conn = mysql_init(NULL);
    if (mysql_conn == NULL)
    {
        fprintf(stderr, "findReflector: mysql_init() failed\n");
        exit(1);
    }

    if (mysql_real_connect(mysql_conn, dbhost, dbUser(), passwd, "", 0, NULL, 0) == NULL)
    {
        fprintf(stderr, "%s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return false;
    }

    sb_init(&query, 512);
    sb_append(&query, "SELECT ip4, port FROM modem_host.reflectors WHERE refl_type = '");
    sbAppendEscaped(&query, mysql_conn, type);
    sb_append(&query, "' AND Nick = '");
    sbAppendEscaped(&query, mysql_conn, name);
    sb_append(&query, "'");
    
    if (mysql_query(mysql_conn, sb_cstr(&query)))
    {
        fprintf(stderr, "findReflector: mysql_query failed: %s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        sb_free(&query);
        return false;
    }

    MYSQL_RES *result;
    result = mysql_store_result(mysql_conn);
    if (result == NULL)
    {
        fprintf(stderr, "findReflector: failed: %s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        sb_free(&query);
        return false;
    }
    else
    {
        MYSQL_ROW row;
        if ((row = mysql_fetch_row(result)) != NULL)
        {
            strcpy(ip4, row[0]);
            *port = atoi(row[1]);
        }
        else
        {
            mysql_free_result(result);
            mysql_close(mysql_conn);
            sb_free(&query);
            return false;
        }
        mysql_free_result(result);
    }

    sb_free(&query);
    mysql_close(mysql_conn);
    return true;
}

/*
 * findDMRId()
 *
 * Looks up a DMR ID in the modem_host.dmr_ids table.
 *
 * Parameters:
 *   DMRId        - DMR radio ID to look up.
 *   callsign     - Output buffer that receives the callsign.
 *   callsignSize - Size of the callsign buffer; a longer callsign is cut.
 *   name         - Output buffer that receives the operator name.
 *   nameSize     - Size of the name buffer; a longer name is cut.
 *
 * Returns: true if the ID was found, false if it was not found or the
 *   database could not be used.
 */
bool findDMRId(const unsigned int DMRId, char* callsign, size_t callsignSize, char* name, size_t nameSize)
{
    char dbhost[128] = "localhost";
    char passwd[128] = "";
    char idStr[12];
    StringBuilder query;

    readConfig("database", "host", dbhost);
    readConfig("database", "passwd", passwd);

    MYSQL *mysql_conn = mysql_init(NULL);
    if (mysql_conn == NULL)
    {
        fprintf(stderr, "findReflector: mysql_init() failed\n");
        exit(1);
    }

    if (mysql_real_connect(mysql_conn, dbhost, dbUser(), passwd, "", 0, NULL, 0) == NULL)
    {
        fprintf(stderr, "%s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return false;
    }

    sprintf(idStr, "%u", DMRId);
    
    sb_init(&query, 512);
    sb_append(&query, "SELECT callsign, name FROM modem_host.dmr_ids WHERE dmr_id = '");
    sbAppendEscaped(&query, mysql_conn, idStr);
    sb_append(&query, "'");
    
    if (mysql_query(mysql_conn, sb_cstr(&query)))
    {
        fprintf(stderr, "findDMRId: mysql_query failed: %s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        sb_free(&query);
        return false;
    }

    MYSQL_RES *result;
    result = mysql_store_result(mysql_conn);
    if (result == NULL)
    {
        fprintf(stderr, "findDMRId: failed: %s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        sb_free(&query);
        return false;
    }
    else
    {
        MYSQL_ROW row;
        if ((row = mysql_fetch_row(result)) != NULL)
        {
            snprintf(callsign, callsignSize, "%s", row[0] != NULL ? row[0] : "");
            snprintf(name, nameSize, "%s", row[1] != NULL ? row[1] : "");
        }
        else
        {
            mysql_free_result(result);
            mysql_close(mysql_conn);
            sb_free(&query);
            return false;
        }
        mysql_free_result(result);
    }

    sb_free(&query);
    mysql_close(mysql_conn);
    return true;
}

/*
 * setReflectorStatus()
 *
 * Records in modem_host.host_status that a module is linked to a reflector,
 * or removes that record when it is unlinked.
 *
 * Parameters:
 *   modem_name - Name of the modem.
 *   module     - Module that the link belongs to.
 *   name       - Name of the reflector.
 *   linked     - true to record the link, false to remove it.
 *
 * Returns: true on success, false if the database connection or the query
 *   failed.
 */
bool setReflectorStatus(const char* modem_name, const char* module, const char* name, bool linked)
{
    char dbhost[128] = "localhost";
    char passwd[128] = "";
    StringBuilder query;

    readConfig("database", "host", dbhost);
    readConfig("database", "passwd", passwd);

    MYSQL *mysql_conn = mysql_init(NULL);
    if (mysql_conn == NULL)
    {
        fprintf(stderr, "setReflectorStatus: mysql_init() failed\n");
        exit(1);
    }

    if (mysql_real_connect(mysql_conn, dbhost, dbUser(), passwd, "", 0, NULL, 0) == NULL)
    {
        fprintf(stderr, "%s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return false;
    }

    sb_init(&query, 512);

    if (linked)
    {
        sb_append(&query, "INSERT INTO modem_host.host_status SET modem_name = '");
        sbAppendEscaped(&query, mysql_conn, modem_name);
        sb_append(&query, "', module = '");
        sbAppendEscaped(&query, mysql_conn, module);
        sb_append(&query, "', ");
        sb_append(&query, "property = 'reflector', value = '");
        sbAppendEscaped(&query, mysql_conn, name);
        sb_append(&query, "'");
    }
    else
    {
        sb_append(&query, "DELETE FROM modem_host.host_status WHERE modem_name = '");
        sbAppendEscaped(&query, mysql_conn, modem_name);
        sb_append(&query, "' AND module = '");
        sbAppendEscaped(&query, mysql_conn, module);
        sb_append(&query, "' AND property = 'reflector' AND value = '");
        sbAppendEscaped(&query, mysql_conn, name);
        sb_append(&query, "'");
    }

    if (mysql_query(mysql_conn, sb_cstr(&query)))
    {
        fprintf(stderr, "setReflectorStatus: mysql_query failed: %s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        sb_free(&query);
        return false;
    }

    sb_free(&query);
    mysql_close(mysql_conn);
    return true;
}

/*
 * clearReflLinkStatus()
 *
 * Removes every reflector link recorded for a module from
 * modem_host.host_status, for example when a gateway starts or stops.
 *
 * Parameters:
 *   modem_name - Name of the modem.
 *   module     - Module whose reflector links are removed.
 *
 * Returns: true on success, false if the database connection or the delete
 *   failed.
 */
bool clearReflLinkStatus(const char* modem_name, const char* module)
{
    char dbhost[128] = "localhost";
    char passwd[128] = "";
    StringBuilder query;

    readConfig("database", "host", dbhost);
    readConfig("database", "passwd", passwd);

    MYSQL* mysql_conn = mysql_init(NULL);
    if (mysql_conn == NULL)
    {
        fprintf(stderr, "clearReflectorStatus: mysql_init() failed\n");
        exit(1);
    }

    if (mysql_real_connect(mysql_conn, dbhost, dbUser(), passwd, "", 0, NULL, 0) == NULL)
    {
        fprintf(stderr, "%s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return false;
    }

    sb_init(&query, 512);
    sb_append(&query, "DELETE FROM modem_host.host_status WHERE modem_name = '");
    sbAppendEscaped(&query, mysql_conn, modem_name);
    sb_append(&query, "' AND module = '");
    sbAppendEscaped(&query, mysql_conn, module);
    sb_append(&query, "' AND property = 'reflector'");

    if (mysql_query(mysql_conn, sb_cstr(&query)))
    {
        fprintf(stderr, "clearReflectorStatus: mysql_query failed: %s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        sb_free(&query);
        return false;
    }

    sb_free(&query);
    mysql_close(mysql_conn);
    return true;
}

/*
 * logError()
 *
 * Adds an error message to the modem_host.errors table, stamped with the
 * current time, so the dashboard can show it.
 *
 * Parameters:
 *   modem_name - Name of the modem that had the error.
 *   module     - Module that had the error.
 *   message    - Error text.
 *
 * Returns: true on success, false if the database connection or the insert
 *   failed.
 */
bool logError(const char* modem_name, const char* module, const char* message)
{
    char dbhost[128] = "localhost";
    char passwd[128] = "";
    StringBuilder query;

    readConfig("database", "host", dbhost);
    readConfig("database", "passwd", passwd);

    MYSQL *mysql_conn = mysql_init(NULL);
    if (mysql_conn == NULL)
    {
        fprintf(stderr, "logError: mysql_init() failed\n");
        exit(1);
    }

    if (mysql_real_connect(mysql_conn, dbhost, dbUser(), passwd, "", 0, NULL, 0) == NULL)
    {
        fprintf(stderr, "%s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return false;
    }

    sb_init(&query, 512);
    sb_append(&query, "INSERT INTO modem_host.errors SET modem_name = '");
    sbAppendEscaped(&query, mysql_conn, modem_name);
    sb_append(&query, "', module = '");
    sbAppendEscaped(&query, mysql_conn, module);
    sb_append(&query, "', message = '");
    sbAppendEscaped(&query, mysql_conn, message);
    sb_append(&query, "', ");
    sb_append(&query, "datetime = NOW()");

    if (mysql_query(mysql_conn, sb_cstr(&query)))
    {
        fprintf(stderr, "logError: mysql_query failed: %s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        sb_free(&query);
        return false;
    }

    sb_free(&query);
    mysql_close(mysql_conn);
    return true;
}

#define ERROR_QUEUE_SIZE      32
#define ERROR_RECENT_SIZE     16
#define ERROR_REPEAT_SECONDS  10
#define ERROR_TEXT_LENGTH     256

/* One error waiting to be written to the database */
typedef struct
{
    char modem[64];
    char module[32];
    char message[ERROR_TEXT_LENGTH];
} ErrorEntry;

/* An error sent recently, to leave out repeats */
typedef struct
{
    char   module[32];
    char   message[ERROR_TEXT_LENGTH];
    time_t when;
} ErrorRecent;

static ErrorEntry      errorQueue[ERROR_QUEUE_SIZE];
static int             errorHead          = 0;
static int             errorCount         = 0;
static bool            errorBusy          = false;
static bool            errorThreadStarted = false;
static pid_t           errorThreadPid     = 0;
static unsigned int    errorDropped       = 0U;
static ErrorRecent     errorRecent[ERROR_RECENT_SIZE];
static int             errorRecentNext    = 0;
static pthread_mutex_t errorMutex         = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  errorWake          = PTHREAD_COND_INITIALIZER;
static pthread_cond_t  errorIdle          = PTHREAD_COND_INITIALIZER;

/*
 * errorThread()
 *
 * Writes the queued errors to the database, one after the other, with
 * logError().  It runs for the life of the program, so no caller ever waits
 * for the database.
 *
 * Parameters:
 *   arg - Not used.
 *
 * Returns: NULL (never returns).
 */
static void* errorThread(void* arg)
{
    (void)arg;

    for (;;)
    {
        ErrorEntry entry;

        pthread_mutex_lock(&errorMutex);
        while (errorCount == 0)
        {
            errorBusy = false;
            pthread_cond_broadcast(&errorIdle);
            pthread_cond_wait(&errorWake, &errorMutex);
        }

        entry      = errorQueue[errorHead];
        errorHead  = (errorHead + 1) % ERROR_QUEUE_SIZE;
        errorCount--;
        errorBusy  = true;
        pthread_mutex_unlock(&errorMutex);

        logError(entry.modem, entry.module, entry.message);
    }

    return NULL;
}

/*
 * reportError()
 *
 * Reports an error the same way everywhere: it is printed to stderr (the
 * system log) and queued to be written to the modem_host.errors table by a
 * background thread, so the caller never waits for the database.  The same
 * message from the same module is written to the database only once in
 * ERROR_REPEAT_SECONDS seconds, so an error that repeats for every frame does
 * not flood the table; it is still printed every time.  When the queue is
 * full the message is printed but not stored.
 *
 * Parameters:
 *   modem_name - Name of the modem that had the error.
 *   module     - Module that had the error, such as "P25".
 *   format     - printf format of the message.
 *   ...        - Values for the format.
 *
 * Returns: nothing.
 */
void reportError(const char* modem_name, const char* module, const char* format, ...)
{
    char    message[ERROR_TEXT_LENGTH];
    va_list args;

    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    size_t len = strlen(message);
    while (len > 0U && (message[len - 1U] == '\n' || message[len - 1U] == '\r'))
        message[--len] = '\0';

    if (modem_name == NULL)
        modem_name = "";
    if (module == NULL)
        module = "";

    fprintf(stderr, "ERROR [%s]: %s\n", module, message);

    time_t now = time(NULL);

    pthread_mutex_lock(&errorMutex);

    for (int i = 0; i < ERROR_RECENT_SIZE; i++)
    {
        if (errorRecent[i].when != 0 && (now - errorRecent[i].when) < ERROR_REPEAT_SECONDS &&
            strcmp(errorRecent[i].module, module) == 0 && strcmp(errorRecent[i].message, message) == 0)
        {
            pthread_mutex_unlock(&errorMutex);
            return;
        }
    }

    if (errorCount >= ERROR_QUEUE_SIZE)
    {
        errorDropped++;
        pthread_mutex_unlock(&errorMutex);
        return;
    }

    snprintf(errorRecent[errorRecentNext].module, sizeof(errorRecent[errorRecentNext].module), "%s", module);
    snprintf(errorRecent[errorRecentNext].message, sizeof(errorRecent[errorRecentNext].message), "%s", message);
    errorRecent[errorRecentNext].when = now;
    errorRecentNext = (errorRecentNext + 1) % ERROR_RECENT_SIZE;

    ErrorEntry* e = &errorQueue[(errorHead + errorCount) % ERROR_QUEUE_SIZE];
    snprintf(e->modem, sizeof(e->modem), "%s", modem_name);
    snprintf(e->module, sizeof(e->module), "%s", module);
    snprintf(e->message, sizeof(e->message), "%s", message);
    errorCount++;
    errorBusy = true;

    /* A forked child has none of its parent's threads, so the thread is
     * started again when the process id has changed. */
    if (!errorThreadStarted || errorThreadPid != getpid())
    {
        pthread_t tid;

        if (pthread_create(&tid, NULL, errorThread, NULL) == 0)
        {
            pthread_detach(tid);
            errorThreadStarted = true;
            errorThreadPid     = getpid();
            atexit(errorFlush);
        }
    }

    pthread_cond_signal(&errorWake);
    pthread_mutex_unlock(&errorMutex);
}

/*
 * errorFlush()
 *
 * Waits (up to about 3 seconds) until every error that reportError() queued
 * has been written to the database.  Call it before the program ends.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
void errorFlush(void)
{
    struct timespec until;

    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += 3;

    pthread_mutex_lock(&errorMutex);
    while ((errorCount > 0 || errorBusy) && errorThreadStarted && errorThreadPid == getpid())
    {
        if (pthread_cond_timedwait(&errorIdle, &errorMutex, &until) != 0)
            break;
    }
    pthread_mutex_unlock(&errorMutex);
}

/*
 * setStatus()
 *
 * Creates or updates one status property in the modem_host.host_status
 * table.  If the modem, module and property already have a row its value is
 * updated, otherwise a new row is added.
 *
 * Parameters:
 *   modem_name - Name of the modem.
 *   module     - Module the property belongs to.
 *   property   - Property name, such as active_mode.
 *   value      - New value of the property.
 *
 * Returns: true on success, false if the database connection or a query
 *   failed.
 */
bool setStatus(const char* modem_name, const char* module, const char* property, const char* value)
{
    char dbhost[128] = "localhost";
    char passwd[128] = "";
    StringBuilder query;

    readConfig("database", "host", dbhost);
    readConfig("database", "passwd", passwd);

    MYSQL *mysql_conn = mysql_init(NULL);
    if (mysql_conn == NULL)
    {
        fprintf(stderr, "setStatus: mysql_init() failed\n");
        exit(1);
    }

    if (mysql_real_connect(mysql_conn, dbhost, dbUser(), passwd, "", 0, NULL, 0) == NULL)
    {
        fprintf(stderr, "%s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return false;
    }

    sb_init(&query, 512);
    sb_append(&query, "SELECT * FROM modem_host.host_status WHERE modem_name = '");
    sbAppendEscaped(&query, mysql_conn, modem_name);
    sb_append(&query, "' AND module = '");
    sbAppendEscaped(&query, mysql_conn, module);
    sb_append(&query, "' AND property = '");
    sbAppendEscaped(&query, mysql_conn, property);
    sb_append(&query, "'");

    if (mysql_query(mysql_conn, sb_cstr(&query)))
    {
        fprintf(stderr, "setStatus: mysql_query failed: %s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        sb_free(&query);
        return -1;
    }

    MYSQL_RES *result;
    result = mysql_store_result(mysql_conn);
    if (result == NULL)
    {
        fprintf(stderr, "setStatus: failed: %s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        sb_free(&query);
        return -2;
    }

    if (mysql_num_rows(result) == 0)
    {
        sb_clear(&query);
        sb_append(&query, "INSERT INTO modem_host.host_status SET modem_name = '");
        sbAppendEscaped(&query, mysql_conn, modem_name);
        sb_append(&query, "', module = '");
        sbAppendEscaped(&query, mysql_conn, module);
        sb_append(&query, "', property = '");
        sbAppendEscaped(&query, mysql_conn, property);
        sb_append(&query, "', value = '");
        sbAppendEscaped(&query, mysql_conn, value);
        sb_append(&query, "'");

        if (mysql_query(mysql_conn, sb_cstr(&query)))
        {
            fprintf(stderr, "setStatus: mysql_query failed: %s\n", mysql_error(mysql_conn));
            mysql_free_result(result);
            mysql_close(mysql_conn);
            sb_free(&query);
            return false;
        }
    }
    else
    {
        sb_clear(&query);
        sb_append(&query, "UPDATE modem_host.host_status SET value = '");
        sbAppendEscaped(&query, mysql_conn, value);
        sb_append(&query, "' WHERE property = '");
        sbAppendEscaped(&query, mysql_conn, property);
        sb_append(&query, "' AND module = '");
        sbAppendEscaped(&query, mysql_conn, module);
        sb_append(&query, "' AND modem_name = '");
        sbAppendEscaped(&query, mysql_conn, modem_name);
        sb_append(&query, "'");
        
        if (mysql_query(mysql_conn, sb_cstr(&query)))
        {
            fprintf(stderr, "setStatus: mysql_query failed: %s\n", mysql_error(mysql_conn));
            mysql_free_result(result);
            mysql_close(mysql_conn);
            sb_free(&query);
            return false;
        }
    }
    
    mysql_free_result(result);
    sb_free(&query);
    mysql_close(mysql_conn);
    return true;
}

/*
 * readStatus()
 *
 * Reads one status property from the modem_host.host_status table.
 *
 * Parameters:
 *   modem_name - Name of the modem.
 *   module     - Module the property belongs to.
 *   property   - Property name.
 *   value      - Output buffer that receives the value.  It is left
 *                unchanged if the property does not exist.
 *
 * Returns: true if the query ran, false if the database connection or the
 *   query failed.
 */
bool readStatus(const char* modem_name, const char* module, const char* property, char* value)
{
    char dbhost[128] = "localhost";
    char passwd[128] = "";
    StringBuilder query;

    readConfig("database", "host", dbhost);
    readConfig("database", "passwd", passwd);

    MYSQL *mysql_conn = mysql_init(NULL);
    if (mysql_conn == NULL)
    {
        fprintf(stderr, "readStatus: mysql_init() failed\n");
        exit(1);
    }

    if (mysql_real_connect(mysql_conn, dbhost, dbUser(), passwd, "", 0, NULL, 0) == NULL)
    {
        fprintf(stderr, "%s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return false;
    }

    sb_init(&query, 512);
    sb_append(&query, "SELECT value FROM modem_host.host_status WHERE modem_name = '");
    sbAppendEscaped(&query, mysql_conn, modem_name);
    sb_append(&query, "' AND module = '");
    sbAppendEscaped(&query, mysql_conn, module);
    sb_append(&query, "' AND property = '");
    sbAppendEscaped(&query, mysql_conn, property);
    sb_append(&query, "'");

    if (mysql_query(mysql_conn, sb_cstr(&query)))
    {
        fprintf(stderr, "readStatus: mysql_query failed: %s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        sb_free(&query);
        return -1;
    }

    MYSQL_RES *result;
    result = mysql_store_result(mysql_conn);
    if (result == NULL)
    {
        fprintf(stderr, "readStatus: failed: %s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        sb_free(&query);
        return -2;
    }

    if (mysql_num_rows(result) > 0)
    {
        MYSQL_ROW row;
        if ((row = mysql_fetch_row(result)) != NULL)
        {
            strcpy(value, row[0]);
        }
    }

    mysql_free_result(result);
    sb_free(&query);
    mysql_close(mysql_conn);
    return true;
}

/*
 * delStatus()
 *
 * Deletes one status property from the modem_host.host_status table.
 *
 * Parameters:
 *   modem_name - Name of the modem.
 *   module     - Module the property belongs to.
 *   property   - Property name to delete.
 *
 * Returns: true on success, false if the database connection or the delete
 *   failed.
 */
bool delStatus(const char* modem_name, const char* module, const char* property)
{
    char dbhost[128] = "localhost";
    char passwd[128] = "";
    StringBuilder query;

    readConfig("database", "host", dbhost);
    readConfig("database", "passwd", passwd);

    MYSQL *mysql_conn = mysql_init(NULL);
    if (mysql_conn == NULL)
    {
        fprintf(stderr, "delStatus: mysql_init() failed\n");
        exit(1);
    }

    if (mysql_real_connect(mysql_conn, dbhost, dbUser(), passwd, "", 0, NULL, 0) == NULL)
    {
        fprintf(stderr, "%s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return false;
    }

    sb_init(&query, 512);
    sb_append(&query, "DELETE FROM modem_host.host_status WHERE modem_name = '");
    sbAppendEscaped(&query, mysql_conn, modem_name);
    sb_append(&query, "' AND module = '");
    sbAppendEscaped(&query, mysql_conn, module);
    sb_append(&query, "' AND property = '");
    sbAppendEscaped(&query, mysql_conn, property);
    sb_append(&query, "'");

    if (mysql_query(mysql_conn, sb_cstr(&query)))
    {
        fprintf(stderr, "delStatus: mysql_query failed: %s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        sb_free(&query);
        return false;
    }

    sb_free(&query);
    mysql_close(mysql_conn);
    return true;
}

/*
 * addGPS()
 *
 * Stores a GPS position, received with a call, in the modem_host.gps table.
 *
 * Parameters:
 *   id        - Id of the modem_host.last_call row the position belongs to.
 *   latitude  - Latitude in degrees.
 *   longitude - Longitude in degrees.
 *   altitude  - Altitude in meters.
 *   bearing   - Heading in degrees.
 *   speed     - Speed in kilometers per hour.
 *
 * Returns: true on success, false if the database connection or the insert
 *   failed.
 */
bool addGPS(const int id, const float latitude, const float longitude,
            const uint16_t altitude, const uint16_t bearing, const uint16_t speed)
{
    char dbhost[128] = "localhost";
    char passwd[128] = "";

    readConfig("database", "host", dbhost);
    readConfig("database", "passwd", passwd);

    MYSQL *mysql_conn = mysql_init(NULL);
    if (mysql_conn == NULL)
    {
        fprintf(stderr, "addGPS: mysql_init() failed\n");
        exit(1);
    }

    if (mysql_real_connect(mysql_conn, dbhost, dbUser(), passwd, "", 0, NULL, 0) == NULL)
    {
        fprintf(stderr, "%s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return false;
    }

    char query[200];
    sprintf(query,"INSERT INTO modem_host.gps SET id = %d, latitude = %f, longitude = %f, altitude_meters = %d,\
                   bearing = %d, speed_kph = %d", id, latitude, longitude, altitude, bearing, speed);

    if (mysql_query(mysql_conn, query))
    {
        fprintf(stderr, "addGPS: mysql_query failed: %s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return false;
    }

    mysql_close(mysql_conn);
    return true;
}

/*
 * get_pi_model()
 *
 * Reads the hardware model name from /proc/device-tree/model.  The text
 * "Unknown Hardware" or "Unknown Model" is stored if it cannot be read.
 *
 * Parameters:
 *   dest      - Output buffer for the model name.
 *   dest_size - Size of the dest buffer in bytes.
 *
 * Returns: nothing.
 */
void get_pi_model(char* dest, size_t dest_size)
{
    FILE* fp = fopen("/proc/device-tree/model", "r");
    if (fp == NULL)
    {
        strncpy(dest, "Unknown Hardware", dest_size);
        return;
    }

    if (fgets(dest, dest_size, fp) == NULL)
    {
        strncpy(dest, "Unknown Model", dest_size);
    }

    // Remove potential trailing newline or garbage character
    dest[strcspn(dest, "\n")] = '\0';
    fclose(fp);
}

/*
 * get_os_release()
 *
 * Reads the PRETTY_NAME value from /etc/os-release.  Stores "Unknown Linux
 * OS" if the file is missing and "Unknown OS Line" if there is no
 * PRETTY_NAME line.
 *
 * Parameters:
 *   dest      - Output buffer for the operating system name.
 *   dest_size - Size of the dest buffer in bytes.
 *
 * Returns: nothing.
 */
void get_os_release(char* dest, size_t dest_size)
{
    FILE* fp = fopen("/etc/os-release", "r");
    if (fp == NULL)
    {
        strncpy(dest, "Unknown Linux OS", dest_size);
        return;
    }

    char line[256];
    int found = 0;

    while (fgets(line, sizeof(line), fp))
    {
        // Look for the PRETTY_NAME configuration line
        if (strncmp(line, "PRETTY_NAME=", 12) == 0)
        {
            // Extract the value inside the quotes
            char* start = strchr(line, '"');
            if (start != NULL)
            {
                start++;  // Move past the first quote
                char* end = strchr(start, '"');
                if (end != NULL)
                {
                    *end = '\0';  // Terminate string at the closing quote
                    strncpy(dest, start, dest_size - 1);
                    dest[dest_size - 1] = '\0';  // Ensure null-termination
                    found               = 1;
                }
            }
            break;
        }
    }

    if (!found)
    {
        strncpy(dest, "Unknown OS Line", dest_size);
    }
    fclose(fp);
}

/*
 * interrogate_pi()
 *
 * Collects the Raspberry Pi hardware model and operating system name.
 *
 * Parameters: none.
 *
 * Returns: A PiSystemInfo structure holding the model and OS name strings.
 */
PiSystemInfo interrogate_pi()
{
    PiSystemInfo info;

    get_pi_model(info.model, sizeof(info.model));
    get_os_release(info.os_name, sizeof(info.os_name));

    return info;
}

/*
 * Timing helpers, socket helpers and software timers.
 *
 * These were previously copied into every protocol host and gateway.
 */

TIMERS          timer[TIMER_COUNT];
pthread_mutex_t timerMutex = PTHREAD_MUTEX_INITIALIZER;

/*
 * delay()
 *
 * Sleeps for the requested number of microseconds.
 *
 * Parameters:
 *   us - Time to sleep in microseconds (any value; seconds are split out).
 *
 * Returns: nothing.
 */
void delay(uint32_t us)
{
    struct timespec req;

    req.tv_sec  = us / 1000000U;
    req.tv_nsec = (long)(us % 1000000U) * 1000L;
    nanosleep(&req, NULL);
}

/*
 * nowMs()
 *
 * Reads the monotonic clock.
 *
 * Parameters: none.
 *
 * Returns: Milliseconds since an arbitrary fixed point, unaffected by
 *          changes to the wall clock.
 */
uint64_t nowMs(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}

/*
 * setNoDelay()
 *
 * Turns on TCP_NODELAY so small frames are sent immediately.  The programs
 * talk over loopback TCP in small frames (200 bytes of samples, 3 byte
 * requests).  With Nagle on, a small write waits for the ACK of the previous
 * one, and the peer delays that ACK for about 40 ms, so the stream arrives in
 * bursts with 40 ms gaps.
 *
 * Parameters:
 *   fd - Connected TCP socket.
 *
 * Returns: nothing.
 */
void setNoDelay(int fd)
{
    int one = 1;

    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
}

/*
 * resolveAddress()
 *
 * Fills in an IPv4 socket address from a dotted address or a host name.
 *
 * Parameters:
 *   host - Dotted IPv4 address or DNS name.
 *   port - Port number in host byte order.
 *   addr - Output socket address.
 *
 * Returns: true if the address was resolved, false otherwise.
 */
bool resolveAddress(const char* host, uint16_t port, struct sockaddr_in* addr)
{
    struct hostent* server;

    memset(addr, 0x00, sizeof(*addr));
    addr->sin_family = AF_INET;
    addr->sin_port   = htons(port);

    if (inet_pton(AF_INET, host, &addr->sin_addr) == 1)
        return true;

    server = gethostbyname(host);
    if (server == NULL || server->h_addrtype != AF_INET)
        return false;

    memcpy(&addr->sin_addr, server->h_addr_list[0], sizeof(addr->sin_addr));
    return true;
}

/*
 * logMsg()
 *
 * Prints a printf style message to stdout with a local date and time stamp.
 *
 * Parameters:
 *   fmt - printf format string, followed by its arguments.
 *
 * Returns: nothing.
 */
void logMsg(const char* fmt, ...)
{
    char stamp[32];
    time_t t = time(NULL);
    struct tm tmv;
    va_list ap;

    localtime_r(&t, &tmv);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tmv);

    printf("%s ", stamp);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

/*
 * initTimers()
 *
 * Clears every software timer slot.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
void initTimers(void)
{
    for (uint8_t i = 0; i < TIMER_COUNT; i++)
    {
        bzero(timer[i].name, 20);
        timer[i].valid     = false;
        timer[i].duration  = 0;
        timer[i].count     = 0;
        timer[i].enabled   = false;
        timer[i].triggered = false;
    }
}

/*
 * getTimer()
 *
 * Claims a free timer slot and starts it.
 *
 * Parameters:
 *   name     - Timer name (up to 19 characters), used to find it later.
 *   duration - Timeout in timer ticks.
 *
 * Returns: The slot number, or -1 if every slot is in use.
 */
int8_t getTimer(const char* name, uint32_t duration)
{
    for (uint8_t i = 0; i < TIMER_COUNT; i++)
    {
        pthread_mutex_lock(&timerMutex);
        if (!timer[i].valid)
        {
            strcpy(timer[i].name, name);
            timer[i].valid     = true;
            timer[i].enabled   = true;
            timer[i].triggered = false;
            timer[i].count     = 0;
            timer[i].duration  = duration;
            pthread_mutex_unlock(&timerMutex);
            return i;
        }
        pthread_mutex_unlock(&timerMutex);
    }
    return -1;
}

/*
 * getTimerDuration()
 *
 * Looks up a timer's configured timeout.
 *
 * Parameters:
 *   name - Timer name (case insensitive).
 *
 * Returns: The timeout in ticks, or 0 if no such timer exists.
 */
uint32_t getTimerDuration(const char* name)
{
    for (uint8_t i = 0; i < TIMER_COUNT; i++)
    {
        pthread_mutex_lock(&timerMutex);
        if (strcasecmp(timer[i].name, name) == 0)
        {
            pthread_mutex_unlock(&timerMutex);
            return timer[i].duration;
        }
        pthread_mutex_unlock(&timerMutex);
    }
    return 0;
}

/*
 * isTimerTriggered()
 *
 * Tests whether a timer has reached its timeout.
 *
 * Parameters:
 *   name - Timer name (case insensitive).
 *
 * Returns: true if the timer exists and has triggered, false otherwise.
 */
bool isTimerTriggered(const char* name)
{
    for (uint8_t i = 0; i < TIMER_COUNT; i++)
    {
        pthread_mutex_lock(&timerMutex);
        if (timer[i].valid && timer[i].triggered && strcasecmp(timer[i].name, name) == 0)
        {
            pthread_mutex_unlock(&timerMutex);
            return true;
        }
        pthread_mutex_unlock(&timerMutex);
    }
    return false;
}

/*
 * resetTimer()
 *
 * Restarts a timer, or changes its timeout.
 *
 * Parameters:
 *   name     - Timer name (case insensitive).
 *   duration - New timeout in ticks if greater than zero.  If zero the
 *              count and triggered flag are cleared and the timeout is
 *              left unchanged.
 *
 * Returns: nothing.
 */
void resetTimer(const char* name, const uint32_t duration)
{
    pthread_mutex_lock(&timerMutex);
    for (uint8_t i = 0; i < TIMER_COUNT; i++)
    {
        if (timer[i].valid && strcasecmp(timer[i].name, name) == 0)
        {
            /* Set new duration if greater than zero. */
            if (duration > 0)
                timer[i].duration = duration;
            else
            {
                timer[i].count     = 0;
                timer[i].triggered = false;
            }
            break;
        }
    }
    pthread_mutex_unlock(&timerMutex);
}

/*
 * disableTimer()
 *
 * Releases a timer slot.
 *
 * Parameters:
 *   id - Slot number returned by getTimer().
 *
 * Returns: nothing.
 */
void disableTimer(uint8_t id)
{
    pthread_mutex_lock(&timerMutex);
    bzero(timer[id].name, 20);
    timer[id].valid     = false;
    timer[id].enabled   = false;
    timer[id].triggered = false;
    timer[id].duration  = 0;
    timer[id].count     = 0;
    pthread_mutex_unlock(&timerMutex);
}

/*
 * timerTick()
 *
 * Advances one timer slot by one tick.  The host's timer thread calls this
 * for every slot at each tick.
 *
 * Parameters:
 *   id - Slot number (0 to TIMER_COUNT - 1).
 *
 * Returns: nothing.
 */
void timerTick(uint8_t id)
{
    pthread_mutex_lock(&timerMutex);
    if (timer[id].valid && timer[id].enabled)
    {
        if (timer[id].count >= timer[id].duration)
        {
            timer[id].triggered = true;
            timer[id].count     = 0;
        }
        else if (!timer[id].triggered)
        {
            timer[id].count++;
        }
    }
    pthread_mutex_unlock(&timerMutex);
}

/*
 * timerTickN()
 *
 * Advances one timer slot by several ticks at once.  The result is the same
 * as calling timerTick() that many times, so a timer thread that sleeps
 * longer between passes can still keep the timers exact.
 *
 * Parameters:
 *   id    - Slot number (0 to TIMER_COUNT - 1).
 *   ticks - Number of ticks (milliseconds) to add.
 *
 * Returns: nothing.
 */
void timerTickN(uint8_t id, uint32_t ticks)
{
    pthread_mutex_lock(&timerMutex);
    if (timer[id].valid && timer[id].enabled)
    {
        for (uint32_t i = 0; i < ticks; i++)
        {
            if (timer[id].count >= timer[id].duration)
            {
                timer[id].triggered = true;
                timer[id].count     = 0;
            }
            else if (!timer[id].triggered)
            {
                timer[id].count++;
            }
            else
                break;      /* triggered and waiting to be handled: more ticks change nothing */
        }
    }
    pthread_mutex_unlock(&timerMutex);
}

/*
 * timerElapsedMs()
 *
 * Returns the whole milliseconds that have passed since the previous call
 * with the same marker and moves the marker forward by that amount, so no
 * time is lost between passes.  The first call (marker 0) sets the marker
 * and returns 0.
 *
 * Parameters:
 *   last - Marker (a clock reading in milliseconds), kept by the caller.
 *
 * Returns: The elapsed milliseconds.
 */
uint32_t timerElapsedMs(uint64_t* last)
{
    uint64_t now = nowMs();

    if (*last == 0U)
    {
        *last = now;
        return 0U;
    }

    uint64_t ms = now - *last;

    *last += ms;
    return (uint32_t)ms;
}

/*
 * sleepUntilNext()
 *
 * Sleeps until a fixed schedule is due and moves the schedule one period on.
 * The wait ends at an exact multiple of the period after the start of the
 * schedule, so a thread that does a little work each time keeps an even pace
 * and wakes only once per period.  A schedule that has fallen behind is
 * restarted from now (no burst to catch up).
 *
 * Parameters:
 *   next     - When the thread is next due (a clock reading in milliseconds),
 *              kept by the caller; 0 starts a new schedule.
 *   periodMs - Period in milliseconds.
 *
 * Returns: nothing.
 */
void sleepUntilNext(uint64_t* next, uint32_t periodMs)
{
    uint64_t now = nowMs();

    if (*next == 0U)
        *next = now + periodMs;
    if (*next > now)
        delay((uint32_t)(*next - now) * 1000U);
    now = nowMs();
    *next += periodMs;
    if (*next + periodMs < now)
        *next = now + periodMs;
}

/*
 * hexDump()
 *
 * Prints a block of data on stdout as a hex dump, 16 bytes per line with a
 * printable character column, after a heading line with the current time in
 * milliseconds.  Based on the dump routine of the MMDVM project by Jonathan
 * Naylor G4KLX.
 *
 * Parameters:
 *   text   - Heading text.
 *   data   - Bytes to print.
 *   length - Number of bytes to print.
 *
 * Returns: nothing.
 */
void hexDump(const char* text, const uint8_t* data, unsigned int length)
{
    struct timespec ts;
    uint64_t milliseconds;
    unsigned int offset = 0U;
    unsigned int i;

    if (clock_gettime(CLOCK_REALTIME, &ts) == -1)
    {
        perror("clock_gettime");
        return;
    }

    milliseconds = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;

    fprintf(stdout, "%s: %llu (ms)\n", text, (unsigned long long)milliseconds);

    while (length > 0U)
    {
        unsigned int bytes = (length > 16U) ? 16U : length;

        fprintf(stdout, "%04X:  ", offset);

        for (i = 0U; i < bytes; i++)
            fprintf(stdout, "%02X ", data[offset + i]);

        for (i = bytes; i < 16U; i++)
            fputs("   ", stdout);

        fputs("   *", stdout);

        for (i = 0U; i < bytes; i++)
        {
            unsigned char c = data[offset + i];

            if (isprint(c))
                fputc(c, stdout);
            else
                fputc('.', stdout);
        }

        fputs("*\n", stdout);

        offset += 16U;

        if (length >= 16U)
            length -= 16U;
        else
            length = 0U;
    }

    fflush(stdout);
}

/*
 * modemWriteBits()
 *
 * Queues one byte of transmit bits for the modem host.  Bytes are collected
 * in 200 byte BITS packets (8 byte header, 192 bytes of bits), which are
 * written to the modem host socket when full.  The packet being filled is
 * kept in this function, so all calls in one process must use the same
 * socket.
 *
 * Parameters:
 *   fd    - Socket connected to the modem host.
 *   c     - Byte of transmit bits.
 *   isEOT - true to send the partly filled packet now (end of transmission);
 *           the rest of the packet is filled with zeros.
 *
 * Returns: nothing.
 */
void modemWriteBits(int fd, uint8_t c, bool isEOT)
{
    static uint8_t bytes[200] = {0x61, 0x00, 0xc8, 0x04, 'B', 'I', 'T', 'S'}; /* 8 byte header */
    static uint8_t bytePos = 8; /* first position after the header */

    bytes[bytePos++] = c;
    if (bytePos == 200)
    {
        write(fd, bytes, 200);
        bytePos = 8;
    }

    if (bytePos > 8 && isEOT) /* end of transmission: pad the packet with zeros */
    {
        memset(bytes + bytePos, 0, 200 - bytePos);
        write(fd, bytes, 200);
        bytePos = 8;
    }
}

/*
 * modemQueueSamples()
 *
 * Queues transmit samples for the modem host.  The samples are collected in
 * 200 byte SAMP packets (8 byte header, 96 samples of 16 bits, low byte
 * first), which are written to the modem host socket when full.  The packet
 * being filled is kept in this function, so all calls in one process must use
 * the same socket.
 *
 * On end of transmission the partly filled packet is padded with zeros and
 * sent with the header marked EOT.  This is done even if the packet is empty,
 * so every transmission ends with one EOT packet (unlike modemWriteBits(),
 * which sends nothing when there is nothing to flush).
 *
 * Parameters:
 *   fd            - Socket connected to the modem host.
 *   samples       - The samples to queue.
 *   count         - Number of samples in samples.
 *   isEOT         - true to send the partly filled packet now as the end of
 *                   the transmission.
 *   packetDelayUs - Microseconds to wait after each full packet is written,
 *                   to pace the modem host; 0 for no wait.  The wait is not
 *                   done after the EOT packet.
 *
 * Returns: nothing.
 */
void modemQueueSamples(int fd, const int16_t* samples, unsigned int count,
                       bool isEOT, uint32_t packetDelayUs)
{
    static uint8_t bytes[200] = {0x61, 0x00, 0xc8, 0x04, 'S', 'A', 'M', 'P'}; /* 8 byte header */
    static uint8_t bytePos = 8; /* first position after the header */

    bytes[5] = 'A';
    bytes[6] = 'M';
    bytes[7] = 'P';

    for (unsigned int i = 0U; i < count; i++)
    {
        bytes[bytePos++] = (uint8_t)((uint16_t)samples[i] & 0x00FFU);
        bytes[bytePos++] = (uint8_t)(((uint16_t)samples[i] & 0xFF00U) >> 8);
        if (bytePos == 200)
        {
            write(fd, bytes, 200);
            bytePos = 8;
            if (packetDelayUs > 0U)
                delay(packetDelayUs);
        }
    }

    if (isEOT) /* pad out the packet with zeros, even if it is empty */
    {
        bytes[5] = 'E';
        bytes[6] = 'O';
        bytes[7] = 'T';
        memset(bytes + bytePos, 0, 200 - bytePos);
        write(fd, bytes, 200);
        bytePos = 8;
    }
}

/*
 * hostToAddress()
 *
 * Turns the address given with -a into an IPv4 address.  A dotted address such as 192.168.1.20 is used as it is;
 * anything else (for example "localhost" or "radio.local") is looked up by name.
 *
 * host - Dotted IPv4 address or host name.
 * out  - Receives the address.
 *
 * Returns true if the address could be used.
 */
bool hostToAddress(const char* host, struct in_addr* out)
{
    struct addrinfo hints;
    struct addrinfo* res = NULL;

    if (host == NULL || host[0] == '\0' || out == NULL)
        return false;

    if (inet_pton(AF_INET, host, out) == 1)
        return true;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || res == NULL)
        return false;

    *out = ((struct sockaddr_in*)res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return true;
}  /** end hostToAddress **/

/*
 * Database settings given on the command line.  A setting given there wins over the environment variable of the
 * same purpose, and that wins over the [database] section of the configuration file.
 */
static char dbOptHost[128]   = "";
static char dbOptUser[64]    = "";
static char dbOptPasswd[128] = "";
static char dbOptConfig[256] = "";
static char dbUserBuf[64]    = "";

/*
 * dbOption()
 *
 * Handles one of the database command line options of the hosts, gateways and Modem_Host.  Call it from the
 * option switch with the option letter and its argument:
 *     -s  database server (address or name)     -u  database user name
 *     -p  database password                     -c  path of the configuration file
 * The password is overwritten in the argument list after it is copied, so it does not stay visible in ps.
 *
 * opt - The option letter.
 * arg - The option's argument.
 *
 * Returns true if the option was one of these and was taken.
 */
bool dbOption(int opt, char* arg)
{
    if (arg == NULL)
        return false;

    switch (opt)
    {
        case 's':
            snprintf(dbOptHost, sizeof(dbOptHost), "%s", arg);
            return true;
        case 'u':
            snprintf(dbOptUser, sizeof(dbOptUser), "%s", arg);
            return true;
        case 'p':
            snprintf(dbOptPasswd, sizeof(dbOptPasswd), "%s", arg);
            memset(arg, '*', strlen(arg));
            return true;
        case 'c':
            snprintf(dbOptConfig, sizeof(dbOptConfig), "%s", arg);
            return true;
    }
    return false;
}  /** end dbOption **/

/*
 * configFilePath()
 *
 * Gives the path of the configuration file: the -c option, else the OPENMT_INI environment variable, else
 * /etc/openmt.ini.
 *
 * Returns the path.
 */
const char* configFilePath(void)
{
    const char* env;

    if (dbOptConfig[0] != '\0')
        return dbOptConfig;
    env = getenv("OPENMT_INI");
    if (env != NULL && env[0] != '\0')
        return env;
    return "/etc/openmt.ini";
}  /** end configFilePath **/

/*
 * dbConfigOverride()
 *
 * Used by readConfig() before it opens the file.  For the database host, user and password it looks for a
 * command line option, then an environment variable (OPENMT_DB_HOST, OPENMT_DB_USER, OPENMT_DB_PASSWD).
 *
 * section - Section name given to readConfig().
 * key     - Key name given to readConfig().
 * value   - Receives the setting when one is found.  It must be large enough for the value, as for readConfig().
 *
 * Returns true if a setting was found and stored in value.
 */
bool dbConfigOverride(const char* section, const char* key, char* value)
{
    const char* opt = NULL;
    const char* env = NULL;

    if (strcmp(section, "database") != 0)
        return false;

    if (strcmp(key, "host") == 0)
    {
        opt = dbOptHost;
        env = getenv("OPENMT_DB_HOST");
    }
    else if (strcmp(key, "user") == 0)
    {
        opt = dbOptUser;
        env = getenv("OPENMT_DB_USER");
    }
    else if (strcmp(key, "passwd") == 0)
    {
        opt = dbOptPasswd;
        env = getenv("OPENMT_DB_PASSWD");
    }
    else
        return false;

    if (opt[0] != '\0')
        snprintf(value, 128, "%s", opt);
    else if (env != NULL && env[0] != '\0')
        snprintf(value, 128, "%s", env);
    else
        return false;
    return true;
}  /** end dbConfigOverride **/

/*
 * dbUser()
 *
 * Gives the database user name: the -u option, the OPENMT_DB_USER environment variable, "user=" in the [database]
 * section of the configuration file, else "openmt".
 *
 * Returns the user name.
 */
const char* dbUser(void)
{
    if (dbUserBuf[0] == '\0')
    {
        char user[128] = "openmt";

        readConfig("database", "user", user);
        snprintf(dbUserBuf, sizeof(dbUserBuf), "%s", user);
    }
    return dbUserBuf;
}  /** end dbUser **/
