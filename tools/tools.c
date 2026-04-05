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

uint8_t countBits8(uint8_t bits)
{
    return BITS_TABLE[bits];
}

uint8_t countBits16(uint16_t bits)
{
    uint8_t* p = (uint8_t*)&bits;
    uint8_t n = 0U;
    n += BITS_TABLE[p[0U]];
    n += BITS_TABLE[p[1U]];
    return n;
}

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

/* Function to read a value from the OpenMT config file */
char* readConfig(const char* section, const char* key, char* value)
{
    FILE* file = fopen("/etc/openmt.ini", "r");
    if (file == NULL)
    {
        perror("Error opening configuration file [/etc/openmt.ini]");
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

/* Function to return a list of tokens separated by a delimiter from a string */
void splitString(const char* input, char delimiter, StringArray* result)
{
    split_string(input, delimiter, result);
}

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

/* Function to add comms history to database */
bool saveHistory(const char* modem_name, const char* mode, const char* type, const char* src, const char* suffix,
                const char* dst, float loss_BER, const char* message, uint16_t duration)
{
    char dbhost[20] = "localhost";
    char passwd[30] = "";
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

    if (mysql_real_connect(mysql_conn, dbhost, "openmt", passwd, "", 0, NULL, 0) == NULL)
    {
        fprintf(stderr, "saveHistory: %s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return false;
    }

    sb_init(&query, 512);
    sb_append(&query, "INSERT INTO modem_host.history SET ");
    sb_append(&query, "modem_name = '");
    sb_append(&query, modem_name);
    sb_append(&query, "', ");
    sb_append(&query, "mode = '");
    sb_append(&query, mode);
    sb_append(&query, "', ");
    sb_append(&query, "type = '");
    sb_append(&query, type);
    sb_append(&query, "', ");
    sb_append(&query, "source = '");
    sb_append(&query, src);
    sb_append(&query, "', ");
    sb_append(&query, "suffix = '");
    sb_append(&query, suffix);
    sb_append(&query, "', ");
    sb_append(&query, "destination = '");
    sb_append(&query, dst);
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
        sb_append(&query, message);
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

/* Function to save last call info to database */
bool saveLastCall(uint8_t pos, const char* modem_name, const char* mode, const char* type, const char* src, const char* suffix,
                 const char* dst, const char* message, const char* sms, const char* gps, bool isTx)
{
    char dbhost[20] = "localhost";
    char passwd[30] = "";
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

    if (mysql_real_connect(mysql_conn, dbhost, "openmt", passwd, "", 0, NULL, 0) == NULL)
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
    sb_append(&query, modem_name);
    sb_append(&query, "', ");
    sb_append(&query, "mode = '");
    sb_append(&query, mode);
    sb_append(&query, "', ");
    sb_append(&query, "type = '");
    sb_append(&query, type);
    sb_append(&query, "', ");
    sb_append(&query, "source = '");
    sb_append(&query, src);
    sb_append(&query, "', ");
    sb_append(&query, "suffix = '");
    sb_append(&query, suffix);
    sb_append(&query, "', ");
    sb_append(&query, "destination = '");
    sb_append(&query, dst);
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
        sb_append(&query, message);
        sb_append(&query, "')");
    }

    mysql_query(mysql_conn, sb_cstr(&query));

    if (!isTx && (sms != NULL || strlen(gps) > 0))
    {
        sb_clear(&query);
        sb_append(&query, "SELECT id FROM modem_host.last_call WHERE modem_name = '");
        sb_append(&query, modem_name);
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
            sb_append(&query, src);
            sb_append(&query, "', ");
            sb_append(&query, "message = '");
            sb_append(&query, sms);
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

/* Function to set or add a record to the modem_host config database */
bool setHostConfig(const char* modem_name, const char* module_name, const char* key, const char* display_type, const char* value)
{
    char dbhost[20] = "localhost";
    char passwd[30] = "";
    StringBuilder query;

    readConfig("database", "host", dbhost);
    readConfig("database", "passwd", passwd);

    MYSQL *mysql_conn = mysql_init(NULL);

    if (mysql_conn == NULL)
    {
        fprintf(stderr, "setHostConfig: mysql_init() failed\n");
        exit(1);
    }

    if (mysql_real_connect(mysql_conn, dbhost, "openmt", passwd, "", 0, NULL, 0) == NULL)
    {
        mysql_close(mysql_conn);
        fprintf(stderr, "%s\n", mysql_error(mysql_conn));
        return false;
    }

    sb_init(&query, 512);
    sb_append(&query, "SELECT * FROM modem_host.config WHERE modem_name = '");
    sb_append(&query, modem_name);
    sb_append(&query, "' AND module = '");
    sb_append(&query, module_name);
    sb_append(&query, "' AND parameter = '");
    sb_append(&query, key);
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
        sb_append(&query, modem_name);
        sb_append(&query, "', module = '");
        sb_append(&query, module_name);
        sb_append(&query, "', parameter = '");
        sb_append(&query, key);
        sb_append(&query, "', ");
        sb_append(&query, "value = '");
        sb_append(&query, value);
        sb_append(&query, "', display_type = '");
        sb_append(&query, display_type);
        sb_append(&query, "'");
        mysql_query(mysql_conn, sb_cstr(&query));
    }
    else
    {
        sb_clear(&query);
        sb_append(&query, "UPDATE modem_host.config SET value = '");
        sb_append(&query, value);
        sb_append(&query, "', display_type = '");
        sb_append(&query, display_type);
        sb_append(&query, "' WHERE modem_name = '");
        sb_append(&query, modem_name);
        sb_append(&query, "' AND module = '");
        sb_append(&query, module_name);
        sb_append(&query, "' AND parameter = '");
        sb_append(&query, key);
        sb_append(&query, "'");
        mysql_query(mysql_conn, sb_cstr(&query));
    }
    
    mysql_free_result(result);
    sb_free(&query);
    mysql_close(mysql_conn);
    return true;
}

/* Function to read a value from modem_host config database */
bool readHostConfig(const char* modem_name, const char* module_name, const char* key, char* value)
{
    char dbhost[20] = "localhost";
    char passwd[30] = "";
    StringBuilder query;

    readConfig("database", "host", dbhost);
    readConfig("database", "passwd", passwd);

    MYSQL *mysql_conn = mysql_init(NULL);

    if (mysql_conn == NULL)
    {
        fprintf(stderr, "readHostConfig: mysql_init() failed\n");
        exit(1);
    }

    if (mysql_real_connect(mysql_conn, dbhost, "openmt", passwd, "", 0, NULL, 0) == NULL)
    {
        fprintf(stderr, "%s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return false;
    }

    sb_init(&query, 512);
    sb_append(&query, "SELECT value FROM modem_host.config WHERE modem_name = '");
    sb_append(&query, modem_name);
    sb_append(&query, "' AND module = '");
    sb_append(&query, module_name);
    sb_append(&query, "' AND parameter = '");
    sb_append(&query, key);
    sb_append(&query, "'");
    
    if (mysql_query(mysql_conn, sb_cstr(&query)))
    {
        fprintf(stderr, "readHostConfig: mysql_query failed: %s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        sb_free(&query);
        return false;
    }

    strcpy(value, "");
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
            strcpy(value, row[0]);
        }
        mysql_free_result(result);
    }
    
    sb_free(&query);
    mysql_close(mysql_conn);
    return true;
}

/* Function to add active mode name to database */
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

/* Function to delete active mode from database */
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

/* Function to add active gateway to database */
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

/* Function to delete active gateway from database */
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

/* Function to read a value from modem_host database */
bool readDashbCommand(const char* modem_name, const char* command, char* value)
{
    char dbhost[20] = "localhost";
    char passwd[30] = "";
    StringBuilder query;

    readConfig("database", "host", dbhost);
    readConfig("database", "passwd", passwd);

    MYSQL *mysql_conn = mysql_init(NULL);

    if (mysql_conn == NULL)
    {
        fprintf(stderr, "DashCom: mysql_init() failed\n");
        exit(1);
    }

    if (mysql_real_connect(mysql_conn, dbhost, "openmt", passwd, "", 0, NULL, 0) == NULL)
    {
        fprintf(stderr, "%s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return false;
    }

    sb_init(&query, 512);
    sb_append(&query, "SELECT parameter FROM modem_host.dashb_commands WHERE modem_name = '");
    sb_append(&query, modem_name);
    sb_append(&query, "' AND command = '");
    sb_append(&query, command);
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
    }
    else
    {
        MYSQL_ROW row;
        if ((row = mysql_fetch_row(result)) != NULL)
        {
            mysql_free_result(result);
            
            sb_clear(&query);
            sb_append(&query, "UPDATE modem_host.dashb_commands SET parameter = 'processing' WHERE modem_name = '");
            sb_append(&query, modem_name);
            sb_append(&query, "' AND command = '");
            sb_append(&query, command);
            sb_append(&query, "'");
            mysql_query(mysql_conn, sb_cstr(&query));
            mysql_close(mysql_conn);
            sb_free(&query);
            strcpy(value, row[0]);
            return true;
        }
    }
    
    mysql_free_result(result);
    mysql_close(mysql_conn);
    sb_free(&query);
    strcpy(value, "");
    return true;
}

/* Function to acknowledge current dashboard command */
bool ackDashbCommand(const char* modem_name, const char* command, const char* result)
{
    char dbhost[20] = "localhost";
    char passwd[30] = "";
    StringBuilder query;

    readConfig("database", "host", dbhost);
    readConfig("database", "passwd", passwd);

    MYSQL *mysql_conn = mysql_init(NULL);
    if (mysql_conn == NULL)
    {
        fprintf(stderr, "AckDash: mysql_init() failed\n");
        exit(1);
    }

    if (mysql_real_connect(mysql_conn, dbhost, "openmt", passwd, "", 0, NULL, 0) == NULL)
    {
        fprintf(stderr, "AckDash: %s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return false;
    }

    sb_init(&query, 512);
    sb_append(&query, "UPDATE modem_host.dashb_commands SET result = '");
    sb_append(&query, result);
    sb_append(&query, "' WHERE modem_name = '");
    sb_append(&query, modem_name);
    sb_append(&query, "' AND command = '");
    sb_append(&query, command);
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

/* Function to clear current dashboard command */
bool clearDashbCommands(const char* modem_name)
{
    char dbhost[20] = "localhost";
    char passwd[30] = "";
    StringBuilder query;

    readConfig("database", "host", dbhost);
    readConfig("database", "passwd", passwd);

    MYSQL *mysql_conn = mysql_init(NULL);
    if (mysql_conn == NULL)
    {
        fprintf(stderr, "ClearDash: mysql_init() failed\n");
        exit(1);
    }

    if (mysql_real_connect(mysql_conn, dbhost, "openmt", passwd, "", 0, NULL, 0) == NULL)
    {
        fprintf(stderr, "%s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return false;
    }

    sb_init(&query, 512);
    sb_append(&query, "DELETE FROM modem_host.dashb_commands WHERE modem_name = '");
    sb_append(&query, modem_name);
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

/* Function to delete all reflector entries for a given mode */
bool clearReflectorList(const char* type)
{
   char dbhost[20] = "localhost";
    char passwd[30] = "";
    StringBuilder query;

    readConfig("database", "host", dbhost);
    readConfig("database", "passwd", passwd);

    MYSQL *mysql_conn = mysql_init(NULL);
    if (mysql_conn == NULL)
    {
        fprintf(stderr, "clearReflectorList: mysql_init() failed\n");
        exit(1);
    }

    if (mysql_real_connect(mysql_conn, dbhost, "openmt", passwd, "", 0, NULL, 0) == NULL)
    {
        fprintf(stderr, "%s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return false;
    }

    sb_init(&query, 512);
    sb_append(&query, "DELETE FROM modem_host.reflectors WHERE refl_type = '");
    sb_append(&query, type);
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

/* Function to delete a given reflector entry */
bool delReflector(const char* type, const char* name)
{
   char dbhost[20] = "localhost";
    char passwd[30] = "";
    StringBuilder query;

    readConfig("database", "host", dbhost);
    readConfig("database", "passwd", passwd);

    MYSQL *mysql_conn = mysql_init(NULL);
    if (mysql_conn == NULL)
    {
        fprintf(stderr, "delReflector: mysql_init() failed\n");
        exit(1);
    }

    if (mysql_real_connect(mysql_conn, dbhost, "openmt", passwd, "", 0, NULL, 0) == NULL)
    {
        fprintf(stderr, "%s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return false;
    }

    sb_init(&query, 512);
    sb_append(&query, "DELETE FROM modem_host.reflectors WHERE refl_type = '");
    sb_append(&query, type);
    sb_append(&query, "' AND Nick = '");
    sb_append(&query, name);
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

/* Function to add a reflector entry */
bool addReflector(const char* type, const char* name, const char* ip4Addr, const char* ip6Addr, uint16_t port,
                  const char* dashboardURL, const char* refl_title, const char* country)
{
   char dbhost[20] = "localhost";
    char passwd[30] = "";
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

    if (mysql_real_connect(mysql_conn, dbhost, "openmt", passwd, "", 0, NULL, 0) == NULL)
    {
        fprintf(stderr, "%s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return false;
    }

    sb_init(&query, 512);
    sb_append(&query, "INSERT INTO modem_host.reflectors SET refl_type = '");
    sb_append(&query, type);
    sb_append(&query, "', ");
    sb_append(&query, "Nick = '");
    sb_append(&query, name);
    sb_append(&query, "', ");
    sb_append(&query, "ip4 = '");
    sb_append(&query, ip4Addr);
    sb_append(&query, "', ");
    sb_append(&query, "ip6 = '");
    sb_append(&query, ip6Addr);
    sb_append(&query, "', ");
    
    sprintf(tmp, "%d", port);
    sb_append(&query, "port = ");
    sb_append(&query, tmp);
    sb_append(&query, ", ");
    sb_append(&query, "dash_address = '");
    sb_append(&query, dashboardURL);
    sb_append(&query, "', ");
    sb_append(&query, "Name = '");
    sb_append(&query, refl_title);
    sb_append(&query, "', ");
    sb_append(&query, "Country = '");
    sb_append(&query, country);
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

/* Function to return address and port of given reflector */
bool findReflector(const char* type, const char* name, char* ip4, uint16_t* port)
{
    char dbhost[20] = "localhost";
    char passwd[30] = "";
    StringBuilder query;

    readConfig("database", "host", dbhost);
    readConfig("database", "passwd", passwd);

    MYSQL *mysql_conn = mysql_init(NULL);
    if (mysql_conn == NULL)
    {
        fprintf(stderr, "findReflector: mysql_init() failed\n");
        exit(1);
    }

    if (mysql_real_connect(mysql_conn, dbhost, "openmt", passwd, "", 0, NULL, 0) == NULL)
    {
        fprintf(stderr, "%s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return false;
    }

    sb_init(&query, 512);
    sb_append(&query, "SELECT ip4, port FROM modem_host.reflectors WHERE refl_type = '");
    sb_append(&query, type);
    sb_append(&query, "' AND Nick = '");
    sb_append(&query, name);
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

/* Function to return Name and Callsign linked to DMR Id */
bool findDMRId(const unsigned int DMRId, char* callsign, char* name)
{
    char dbhost[20] = "localhost";
    char passwd[30] = "";
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

    if (mysql_real_connect(mysql_conn, dbhost, "openmt", passwd, "", 0, NULL, 0) == NULL)
    {
        fprintf(stderr, "%s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return false;
    }

    sprintf(idStr, "%u", DMRId);
    
    sb_init(&query, 512);
    sb_append(&query, "SELECT callsign, name FROM modem_host.dmr_ids WHERE dmr_id = '");
    sb_append(&query, idStr);
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
            strcpy(callsign, row[0]);
            strcpy(name, row[1]);
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

/* Function to set given reflector status */
bool setReflectorStatus(const char* modem_name, const char* module, const char* name, bool linked)
{
    char dbhost[20] = "localhost";
    char passwd[30] = "";
    StringBuilder query;

    readConfig("database", "host", dbhost);
    readConfig("database", "passwd", passwd);

    MYSQL *mysql_conn = mysql_init(NULL);
    if (mysql_conn == NULL)
    {
        fprintf(stderr, "setReflectorStatus: mysql_init() failed\n");
        exit(1);
    }

    if (mysql_real_connect(mysql_conn, dbhost, "openmt", passwd, "", 0, NULL, 0) == NULL)
    {
        fprintf(stderr, "%s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return false;
    }

    sb_init(&query, 512);

    if (linked)
    {
        sb_append(&query, "INSERT INTO modem_host.host_status SET modem_name = '");
        sb_append(&query, modem_name);
        sb_append(&query, "', module = '");
        sb_append(&query, module);
        sb_append(&query, "', ");
        sb_append(&query, "property = 'reflector', value = '");
        sb_append(&query, name);
        sb_append(&query, "'");
    }
    else
    {
        sb_append(&query, "DELETE FROM modem_host.host_status WHERE modem_name = '");
        sb_append(&query, modem_name);
        sb_append(&query, "' AND module = '");
        sb_append(&query, module);
        sb_append(&query, "' AND property = 'reflector' AND value = '");
        sb_append(&query, name);
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

/* Clear any reflector entries for given modem and module */
bool clearReflLinkStatus(const char* modem_name, const char* module)
{
    char dbhost[20] = "localhost";
    char passwd[30] = "";
    StringBuilder query;

    readConfig("database", "host", dbhost);
    readConfig("database", "passwd", passwd);

    MYSQL* mysql_conn = mysql_init(NULL);
    if (mysql_conn == NULL)
    {
        fprintf(stderr, "clearReflectorStatus: mysql_init() failed\n");
        exit(1);
    }

    if (mysql_real_connect(mysql_conn, dbhost, "openmt", passwd, "", 0, NULL, 0) == NULL)
    {
        fprintf(stderr, "%s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return false;
    }

    sb_init(&query, 512);
    sb_append(&query, "DELETE FROM modem_host.host_status WHERE modem_name = '");
    sb_append(&query, modem_name);
    sb_append(&query, "' AND module = '");
    sb_append(&query, module);
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

/* Function to log errors to database */
bool logError(const char* modem_name, const char* module, const char* message)
{
    char dbhost[20] = "localhost";
    char passwd[30] = "";
    StringBuilder query;

    readConfig("database", "host", dbhost);
    readConfig("database", "passwd", passwd);

    MYSQL *mysql_conn = mysql_init(NULL);
    if (mysql_conn == NULL)
    {
        fprintf(stderr, "logError: mysql_init() failed\n");
        exit(1);
    }

    if (mysql_real_connect(mysql_conn, dbhost, "openmt", passwd, "", 0, NULL, 0) == NULL)
    {
        fprintf(stderr, "%s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return false;
    }

    sb_init(&query, 512);
    sb_append(&query, "INSERT INTO modem_host.errors SET modem_name = '");
    sb_append(&query, modem_name);
    sb_append(&query, "', module = '");
    sb_append(&query, module);
    sb_append(&query, "', message = '");
    sb_append(&query, message);
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

/* Function to set a given status */
bool setStatus(const char* modem_name, const char* module, const char* property, const char* value)
{
    char dbhost[20] = "localhost";
    char passwd[30] = "";
    StringBuilder query;

    readConfig("database", "host", dbhost);
    readConfig("database", "passwd", passwd);

    MYSQL *mysql_conn = mysql_init(NULL);
    if (mysql_conn == NULL)
    {
        fprintf(stderr, "setStatus: mysql_init() failed\n");
        exit(1);
    }

    if (mysql_real_connect(mysql_conn, dbhost, "openmt", passwd, "", 0, NULL, 0) == NULL)
    {
        fprintf(stderr, "%s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return false;
    }

    sb_init(&query, 512);
    sb_append(&query, "SELECT * FROM modem_host.host_status WHERE modem_name = '");
    sb_append(&query, modem_name);
    sb_append(&query, "' AND module = '");
    sb_append(&query, module);
    sb_append(&query, "' AND property = '");
    sb_append(&query, property);
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
        sb_append(&query, modem_name);
        sb_append(&query, "', module = '");
        sb_append(&query, module);
        sb_append(&query, "', property = '");
        sb_append(&query, property);
        sb_append(&query, "', value = '");
        sb_append(&query, value);
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
        sb_append(&query, value);
        sb_append(&query, "' WHERE property = '");
        sb_append(&query, property);
        sb_append(&query, "' AND module = '");
        sb_append(&query, module);
        sb_append(&query, "' AND modem_name = '");
        sb_append(&query, modem_name);
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

/* Function to read a given status */
bool readStatus(const char* modem_name, const char* module, const char* property, char* value)
{
    char dbhost[20] = "localhost";
    char passwd[30] = "";
    StringBuilder query;

    readConfig("database", "host", dbhost);
    readConfig("database", "passwd", passwd);

    MYSQL *mysql_conn = mysql_init(NULL);
    if (mysql_conn == NULL)
    {
        fprintf(stderr, "readStatus: mysql_init() failed\n");
        exit(1);
    }

    if (mysql_real_connect(mysql_conn, dbhost, "openmt", passwd, "", 0, NULL, 0) == NULL)
    {
        fprintf(stderr, "%s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return false;
    }

    sb_init(&query, 512);
    sb_append(&query, "SELECT value FROM modem_host.host_status WHERE modem_name = '");
    sb_append(&query, modem_name);
    sb_append(&query, "' AND module = '");
    sb_append(&query, module);
    sb_append(&query, "' AND property = '");
    sb_append(&query, property);
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

/* Function to delete given status */
bool delStatus(const char* modem_name, const char* module, const char* property)
{
    char dbhost[20] = "localhost";
    char passwd[30] = "";
    StringBuilder query;

    readConfig("database", "host", dbhost);
    readConfig("database", "passwd", passwd);

    MYSQL *mysql_conn = mysql_init(NULL);
    if (mysql_conn == NULL)
    {
        fprintf(stderr, "delStatus: mysql_init() failed\n");
        exit(1);
    }

    if (mysql_real_connect(mysql_conn, dbhost, "openmt", passwd, "", 0, NULL, 0) == NULL)
    {
        fprintf(stderr, "%s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return false;
    }

    sb_init(&query, 512);
    sb_append(&query, "DELETE FROM modem_host.host_status WHERE modem_name = '");
    sb_append(&query, modem_name);
    sb_append(&query, "' AND module = '");
    sb_append(&query, module);
    sb_append(&query, "' AND property = '");
    sb_append(&query, property);
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

/* Function to add GPS location to database */
bool addGPS(const int id, const float latitude, const float longitude,
            const uint16_t altitude, const uint16_t bearing, const uint16_t speed)
{
    char dbhost[20] = "localhost";
    char passwd[30] = "";

    readConfig("database", "host", dbhost);
    readConfig("database", "passwd", passwd);

    MYSQL *mysql_conn = mysql_init(NULL);
    if (mysql_conn == NULL)
    {
        fprintf(stderr, "addGPS: mysql_init() failed\n");
        exit(1);
    }

    if (mysql_real_connect(mysql_conn, dbhost, "openmt", passwd, "", 0, NULL, 0) == NULL)
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
