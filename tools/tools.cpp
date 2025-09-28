/***************************************************************************
 *   Copyright (C) 2025 by Rick KD0OSS             *
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
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <cstdint>
#include <string>
#include <vector>
#include <sstream>
#include "../modem/mmdvm.h"
#include <ctype.h>
#include <mariadb/mysql.h>

using namespace std;

#define MAX_LINE_LENGTH 256

// Function to read a value from the OpenMT config file
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
        // Remove newline character if present
        line[strcspn(line, "\n")] = 0;

        // Skip comments and empty lines
        if (line[0] == ';' || line[0] == '#' || line[0] == '\0')
        {
            continue;
        }

        // Check for section header
        if (line[0] == '[' && line[strlen(line) - 1] == ']')
        {
            strncpy(current_section, line + 1, strlen(line) - 2);
            current_section[strlen(line) - 2] = '\0';
        }

        // Check for key-value pair within the correct section
        if (strcmp(current_section, section) == 0)
        {
            char* equals_pos = strchr(line, '=');
            if (equals_pos != NULL)
            {
                // Split the line into key and value
                char current_key[MAX_LINE_LENGTH];
                strncpy(current_key, line, equals_pos - line);
                current_key[equals_pos - line] = '\0';
                char* current_value = equals_pos + 1;

                // Compare the key and return the value if it matches
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
    return NULL; // Key not found
}

std::vector<std::string> splitString(const std::string& input, char delimiter)
{
    std::vector<std::string> tokens;
    std::istringstream iss(input); // Create an input string stream from the input string
    std::string token;

    // Read tokens from the string stream, separated by the delimiter
    while (std::getline(iss, token, delimiter))
    {
        tokens.push_back(token); // Add each extracted token to the vector
    }
    return tokens;
}

int saveHistory(const char* mode, const char* type, const char* src,
                const char* dst, float loss_BER, const char* message, uint16_t duration)
{
    char dbhost[20] = "localhost";
    char passwd[30] = "";

    readConfig("database", "host", dbhost);
    readConfig("database", "passwd", passwd);

    MYSQL *mysql_conn = mysql_init(NULL); // Initialize a MySQL connection handle

    if (mysql_conn == NULL)
    {
        fprintf(stderr, "saveHistory: mysql_init() failed\n");
        exit(1);
    }

    // Establish a connection to the MariaDB server
    if (mysql_real_connect(mysql_conn, dbhost, "openmt", passwd, "", 0, NULL, 0) == NULL)
    {
        fprintf(stderr, "saveHistory: %s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return 1;
    }

    std::string query;
    query = "INSERT INTO modem_host.history SET ";
    query.append("mode = '").append(mode).append("', ");
    query.append("type = '").append(type).append("', ");
    query.append("source = '").append(src).append("', ");
    query.append("destination = '").append(dst).append("', ");
    char tmp[10];
    sprintf(tmp, "%0.1f", loss_BER);
    query.append("loss_ber = ").append(tmp).append(", ");
    if (message[0] == 0)
        query.append("ss_message = 'none', ");
    else
        query.append("ss_message = TRIM('").append(message).append("'), ");
    sprintf(tmp, "%d", duration);
    query.append("duration = ").append(tmp).append(", ");
    query.append("datetime = NOW()");

    mysql_query(mysql_conn, query.c_str());
    mysql_close(mysql_conn);
    return 0;
}

int saveLastCall(const char* mode, const char* type, const char* src,
                 const char* dst, const char* message, const char* sms, const char* gps, bool isTx)
{
    char dbhost[20] = "localhost";
    char passwd[30] = "";

    readConfig("database", "host", dbhost);
    readConfig("database", "passwd", passwd);

    MYSQL *mysql_conn = mysql_init(NULL); // Initialize a MySQL connection handle

    if (mysql_conn == NULL)
    {
        fprintf(stderr, "saveLastCall: mysql_init() failed\n");
        exit(1);
    }

    // Establish a connection to the MariaDB server
    if (mysql_real_connect(mysql_conn, dbhost, "openmt", passwd, "", 0, NULL, 0) == NULL)
    {
        fprintf(stderr, "%s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return 1;
    }

    mysql_query(mysql_conn, "DELETE FROM modem_host.last_call");
    std::string query;
    query = "INSERT INTO modem_host.last_call SET ";
    query.append("mode = '").append(mode).append("', ");
    query.append("type = '").append(type).append("', ");
    query.append("source = '").append(src).append("', ");
    query.append("destination = '").append(dst).append("', ");
    if (isTx)
        query.append("isTx = true, ");
    if (message[0] == 0)
        query.append("ss_message = 'none'");
    else
        query.append("ss_message = TRIM('").append(message).append("')");

    mysql_query(mysql_conn, query.c_str());

    if (!isTx && (sms != NULL || strlen(gps) > 0))
    {
        if (mysql_query(mysql_conn, "SELECT id FROM modem_host.last_call"))
        { // Execute a query
            fprintf(stderr, "saveLastCall: mysql_query failed: %s\n", mysql_error(mysql_conn));
            mysql_close(mysql_conn);
            return 1;
        }

        MYSQL_RES *result; // Result set
        result = mysql_store_result(mysql_conn); // Store the result set
        if (result == NULL)
        {
            fprintf(stderr, "saveLastCall: failed: %s\n", mysql_error(mysql_conn));
            mysql_close(mysql_conn);
            return 1;
        }

        int id = 0;
        MYSQL_ROW row; // Row data
        while ((row = mysql_fetch_row(result)) != NULL)
        { // Fetch rows
            id = atoi(row[0]);
        }

        mysql_free_result(result); // Free the result set

        if (sms != NULL)
        {
            query = "INSERT INTO modem_host.sms_messages SET id = ";
            char tmp[12] = "";
            sprintf(tmp, "%d", id);
            query.append(tmp).append(", ");
            query.append("source = '").append(src).append("', ");
            query.append("message = '").append(sms).append("', ");
            query.append("datetime = NOW()");
            mysql_query(mysql_conn, query.c_str());
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
    mysql_close(mysql_conn);
    return 0;
}

// Function to read a value from dvmodem database
std::string readModemConfig(const char* modem_name, const char* key)
{
    char dbhost[20] = "localhost";
    char passwd[30] = "";

    readConfig("database", "host", dbhost);
    readConfig("database", "passwd", passwd);

    std::string query;

    MYSQL *mysql_conn = mysql_init(NULL); // Initialize a MySQL connection handle

    if (mysql_conn == NULL)
    {
        fprintf(stderr, "readModemConfig: mysql_init() failed\n");
        exit(1);
    }

    // Establish a connection to the MariaDB server
    if (mysql_real_connect(mysql_conn, dbhost, "openmt", passwd, "", 0, NULL, 0) == NULL)
    {
        fprintf(stderr, "%s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return "";
    }

    query = "SELECT value FROM dvmodem.config WHERE modem_name = '";
    query.append(modem_name).append("' ").append("AND parameter = '").append(key).append("'");
    if (mysql_query(mysql_conn, query.c_str()))
    { // Execute a query
        fprintf(stderr, "readModemConfig: mysql_query failed: %s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return "";
    }

    MYSQL_RES *result; // Result set
    result = mysql_store_result(mysql_conn); // Store the result set
    if (result == NULL)
    {
        fprintf(stderr, "readModemConfig:  failed: %s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
    }
    else
    {
        MYSQL_ROW row;
        if ((row = mysql_fetch_row(result)) != NULL)
        { // Fetch row
            mysql_free_result(result); // Free the result set
            mysql_close(mysql_conn);
            return row[0];
        }
    }
    mysql_close(mysql_conn);
    return ""; // Key not found
}

// Function to set or add a record to the modem_host config database
int8_t setHostConfig(const char* module_name, const char* key, const char* value)
{
    char dbhost[20] = "localhost";
    char passwd[30] = "";

    readConfig("database", "host", dbhost);
    readConfig("database", "passwd", passwd);

    std::string query;

    MYSQL *mysql_conn = mysql_init(NULL); // Initialize a MySQL connection handle

    if (mysql_conn == NULL)
    {
        fprintf(stderr, "setHostConfig: mysql_init() failed\n");
        return -1;
    }

    // Establish a connection to the MariaDB server
    if (mysql_real_connect(mysql_conn, dbhost, "openmt", passwd, "", 0, NULL, 0) == NULL)
    {
        mysql_close(mysql_conn);
        fprintf(stderr, "%s\n", mysql_error(mysql_conn));
        return -2;
    }

    query = "SELECT * FROM modem_host.config WHERE module = '";
    query.append(module_name).append("' AND parameter = '").append(key).append("'");
    if (mysql_query(mysql_conn, query.c_str()))
    { // Execute a query
        fprintf(stderr, "setHostConfig: mysql_query failed: %s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return -1;
    }

    MYSQL_RES *result; // Result set
    result = mysql_store_result(mysql_conn); // Store the result set
    if (result == NULL)
    {
        fprintf(stderr, "setHostConfig: failed: %s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return -2;
    }

    if (mysql_num_rows(result) == 0)
    {
        query = "INSERT INTO modem_host.config SET module = '";
        query.append(module_name).append("', parameter = '").append(key).append("', ");
        query.append("value = '").append(value).append("'");
        mysql_query(mysql_conn,query.c_str());
    }
    else
    {
        query = "UPDATE modem_host.config SET value = '";
        query.append(value).append("' WHERE module = '").append(module_name).append("' ");
        query.append("AND parameter = '").append(key).append("'");
        mysql_query(mysql_conn, query.c_str());
    }

    mysql_close(mysql_conn);
    return 0;
}

// Function to read a value from modem_host config database
std::string readHostConfig(const char* module_name, const char* key)
{
    char dbhost[20] = "localhost";
    char passwd[30] = "";

    readConfig("database", "host", dbhost);
    readConfig("database", "passwd", passwd);

    std::string query;

    MYSQL *mysql_conn = mysql_init(NULL); // Initialize a MySQL connection handle

    if (mysql_conn == NULL)
    {
        fprintf(stderr, "readHostConfig: mysql_init() failed\n");
        exit(1);
    }

    // Establish a connection to the MariaDB server
    if (mysql_real_connect(mysql_conn, dbhost, "openmt", passwd, "", 0, NULL, 0) == NULL)
    {
        fprintf(stderr, "%s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return "";
    }

    query = "SELECT value FROM modem_host.config WHERE module = '";
    query.append(module_name).append("' ").append("AND parameter = '").append(key).append("'");
    if (mysql_query(mysql_conn, query.c_str()))
    { // Execute a query
        fprintf(stderr, "readHostConfig: mysql_query failed: %s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return "";
    }

    MYSQL_RES *result; // Result set
    result = mysql_store_result(mysql_conn); // Store the result set
    if (result == NULL)
    {
        fprintf(stderr, "readHostConfig: failed: %s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
    }
    else
    {
        MYSQL_ROW row;
        if ((row = mysql_fetch_row(result)) != NULL)
        { // Fetch row
            mysql_free_result(result); // Free the result set
            mysql_close(mysql_conn);
            return row[0];
        }
    }
    mysql_close(mysql_conn);
    return ""; // Key not found
}

bool addMode(const char* module_name, const char* mode)
{
    std::string modes = readHostConfig(module_name, "activeModes");
    if (modes == "none" || modes.empty()) // no modes active
        setHostConfig(module_name, "activeModes", mode);
    else
    {
        std::vector<std::string> modeList = splitString(modes, ',');
        for (uint8_t x=0;x<modeList.size();x++)
        {
            if (modeList[x] == mode)
                return true;
        }
        modes.append(",").append(mode);
        setHostConfig(module_name, "activeModes", modes.c_str());
    }
    return true;
}

bool delMode(const char* module_name, const char* mode)
{
    std::string modes = readHostConfig(module_name, "activeModes");
    if (modes == "none" || modes.empty()) return true;

    std::vector<std::string> modeList = splitString(modes, ',');
    modes.clear();
    for (uint8_t x=0;x<modeList.size();x++)
    {
        if (modeList[x] != mode)
        {
            if (!modes.empty())
                modes.append(",");
            modes.append(modeList[x]);
        }
    }
    if (modes.empty())
        modes = "none";
    setHostConfig(module_name, "activeModes", modes.c_str());
    return true;
}

bool addGateway(const char* module_name, const char* mode)
{
    std::string modes = readHostConfig(module_name, "gateways");
    if (modes == "none" || modes.empty()) // no modes active
        setHostConfig(module_name, "gateways", mode);
    else
    {
        std::vector<std::string> modeList = splitString(modes, ',');
        for (uint8_t x=0;x<modeList.size();x++)
        {
            if (modeList[x] == mode)
                return true;
        }
        modes.append(",").append(mode);
        setHostConfig(module_name, "gateways", modes.c_str());
    }
    return true;
}

bool delGateway(const char* module_name, const char* mode)
{
    std::string modes = readHostConfig(module_name, "gateways");
    if (modes == "none" || modes.empty()) return true;

    std::vector<std::string> modeList = splitString(modes, ',');
    modes.clear();
    for (uint8_t x=0;x<modeList.size();x++)
    {
        if (modeList[x] != mode)
        {
            if (!modes.empty())
                modes.append(",");
            modes.append(modeList[x]);
        }
    }
    if (modes.empty())
        modes = "none";
    setHostConfig(module_name, "gateways", modes.c_str());
    return true;
}

// Function to read a value from modem_host database
std::string readDashbCommand(const char* command)
{
    char dbhost[20] = "localhost";
    char passwd[30] = "";

    readConfig("database", "host", dbhost);
    readConfig("database", "passwd", passwd);

    std::string query;

    MYSQL *mysql_conn = mysql_init(NULL); // Initialize a MySQL connection handle

    if (mysql_conn == NULL)
    {
        fprintf(stderr, "DashCom: mysql_init() failed\n");
        exit(1);
    }

    // Establish a connection to the MariaDB server
    if (mysql_real_connect(mysql_conn, dbhost, "openmt", passwd, "", 0, NULL, 0) == NULL)
    {
        fprintf(stderr, "%s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return "";
    }

    query = "SELECT parameter FROM modem_host.dashb_commands WHERE command = '";
    query.append(command).append("'");
    if (mysql_query(mysql_conn, query.c_str()))
    { // Execute a query
        fprintf(stderr, "DashCom: mysql_query failed: %s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return "";
    }

    MYSQL_RES *result; // Result set
    result = mysql_store_result(mysql_conn); // Store the result set
    if (result == NULL)
    {
        fprintf(stderr, "DashCom: failed: %s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
    }
    else
    {
        MYSQL_ROW row;
        if ((row = mysql_fetch_row(result)) != NULL)
        { // Fetch row
            mysql_free_result(result); // Free the result set
            query = "UPDATE modem_host.dashb_commands SET parameter = 'processing' WHERE command = '";
            query.append(command).append("'");
            mysql_query(mysql_conn, query.c_str());
            mysql_close(mysql_conn);
            return row[0];
        }
    }
    mysql_close(mysql_conn);
    return ""; // Key not found
}

bool ackDashbCommand(const char* command, const char* result)
{
    char dbhost[20] = "localhost";
    char passwd[30] = "";

    readConfig("database", "host", dbhost);
    readConfig("database", "passwd", passwd);

    MYSQL *mysql_conn = mysql_init(NULL); // Initialize a MySQL connection handle
    if (mysql_conn == NULL)
    {
        fprintf(stderr, "AckDash: mysql_init() failed\n");
        exit(1);
    }

    // Establish a connection to the MariaDB server
    if (mysql_real_connect(mysql_conn, dbhost, "openmt", passwd, "", 0, NULL, 0) == NULL)
    {
        fprintf(stderr, "AckDash: %s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return false;
    }

    std::string query;

    query = "UPDATE modem_host.dashb_commands SET result = '";
    query.append(result).append("' WHERE command = '").append(command).append("'");

    if (mysql_query(mysql_conn, query.c_str()))
    { // Execute a query
        fprintf(stderr, "ackDashbCommand: mysql_query failed: %s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return false;
    }

    mysql_close(mysql_conn);
    return true;
}

bool clearDashbCommands()
{
    char dbhost[20] = "localhost";
    char passwd[30] = "";

    readConfig("database", "host", dbhost);
    readConfig("database", "passwd", passwd);

    MYSQL *mysql_conn = mysql_init(NULL); // Initialize a MySQL connection handle
    if (mysql_conn == NULL)
    {
        fprintf(stderr, "ClearDash: mysql_init() failed\n");
        exit(1);
    }

    // Establish a connection to the MariaDB server
    if (mysql_real_connect(mysql_conn, dbhost, "openmt", passwd, "", 0, NULL, 0) == NULL)
    {
        fprintf(stderr, "%s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return false;
    }

    if (mysql_query(mysql_conn, "TRUNCATE modem_host.dashb_commands"))
    { // Execute a query
        fprintf(stderr, "clearDashbCommands: mysql_query failed: %s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return false;
    }

    mysql_close(mysql_conn);
    return true;
}

bool clearReflectorList(const char* type)
{
   char dbhost[20] = "localhost";
    char passwd[30] = "";

    readConfig("database", "host", dbhost);
    readConfig("database", "passwd", passwd);

    MYSQL *mysql_conn = mysql_init(NULL); // Initialize a MySQL connection handle
    if (mysql_conn == NULL)
    {
        fprintf(stderr, "clearReflectorList: mysql_init() failed\n");
        exit(1);
    }

    // Establish a connection to the MariaDB server
    if (mysql_real_connect(mysql_conn, dbhost, "openmt", passwd, "", 0, NULL, 0) == NULL)
    {
        fprintf(stderr, "%s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return false;
    }

    std::string query;
    query = "DELETE FROM modem_host.reflectors WHERE refl_type = '";
    query.append(type).append("'");

    if (mysql_query(mysql_conn, query.c_str()))
    { // Execute a query
        fprintf(stderr, "clearReflectorList: mysql_query failed: %s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return false;
    }

    mysql_close(mysql_conn);
    return true;
}

bool delReflector(const char* type, const char* name)
{
   char dbhost[20] = "localhost";
    char passwd[30] = "";

    readConfig("database", "host", dbhost);
    readConfig("database", "passwd", passwd);

    MYSQL *mysql_conn = mysql_init(NULL); // Initialize a MySQL connection handle
    if (mysql_conn == NULL)
    {
        fprintf(stderr, "delReflector: mysql_init() failed\n");
        exit(1);
    }

    // Establish a connection to the MariaDB server
    if (mysql_real_connect(mysql_conn, dbhost, "openmt", passwd, "", 0, NULL, 0) == NULL)
    {
        fprintf(stderr, "%s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return false;
    }

    std::string query;
    query = "DELETE FROM modem_host.reflectors WHERE refl_type = '";
    query.append(type).append("' AND Nick = '").append(name).append("'");

    if (mysql_query(mysql_conn, query.c_str()))
    { // Execute a query
        fprintf(stderr, "delReflector: mysql_query failed: %s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return false;
    }

    mysql_close(mysql_conn);
    return true;
}

bool addReflector(const char* type, const char* name, const char* ip4Addr, const char* ip6Addr, uint16_t port,
                  const char* dashboardURL, const char* refl_title, const char* country)
{
   char dbhost[20] = "localhost";
    char passwd[30] = "";

    readConfig("database", "host", dbhost);
    readConfig("database", "passwd", passwd);

    MYSQL *mysql_conn = mysql_init(NULL); // Initialize a MySQL connection handle
    if (mysql_conn == NULL)
    {
        fprintf(stderr, "addReflector: mysql_init() failed\n");
        exit(1);
    }

    // Establish a connection to the MariaDB server
    if (mysql_real_connect(mysql_conn, dbhost, "openmt", passwd, "", 0, NULL, 0) == NULL)
    {
        fprintf(stderr, "%s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return false;
    }

    std::string query;
    query = "INSERT INTO modem_host.reflectors SET refl_type = '";
    query.append(type).append("', ");
    query.append("Nick = '").append(name).append("', ");
    query.append("ip4 = '").append(ip4Addr).append("', ");
    query.append("ip6 = '").append(ip6Addr).append("', ");
    char tmp[6];
    sprintf(tmp, "%d", port);
    query.append("port = ").append(tmp).append(", ");
    query.append("dash_address = '").append(dashboardURL).append("', ");
    query.append("Name = '").append(refl_title).append("', ");
    query.append("Country = '").append(country).append("'");

    if (mysql_query(mysql_conn, query.c_str()))
    { // Execute a query
        fprintf(stderr, "addReflector: mysql_query failed: %s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return false;
    }

    mysql_close(mysql_conn);
    return true;
}

bool clearReflLinkStatus(const char* type)
{
    char dbhost[20] = "localhost";
    char passwd[30] = "";

    readConfig("database", "host", dbhost);
    readConfig("database", "passwd", passwd);

    MYSQL *mysql_conn = mysql_init(NULL); // Initialize a MySQL connection handle
    if (mysql_conn == NULL)
    {
        fprintf(stderr, "clearReflectorStatus: mysql_init() failed\n");
        exit(1);
    }

    // Establish a connection to the MariaDB server
    if (mysql_real_connect(mysql_conn, dbhost, "openmt", passwd, "", 0, NULL, 0) == NULL)
    {
        fprintf(stderr, "%s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return false;
    }

    std::string query;

    query = "UPDATE modem_host.reflectors SET status = 'Unlinked' WHERE refl_type = '";
    query.append(type).append("'");

    if (mysql_query(mysql_conn, query.c_str()))
    { // Execute a query
        fprintf(stderr, "clearReflectorStatus: mysql_query failed: %s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return false;
    }

    mysql_close(mysql_conn);
    return true;
}

bool findReflector(const char* type, const char* name, char* ip4, uint16_t* port)
{
    char dbhost[20] = "localhost";
    char passwd[30] = "";

    readConfig("database", "host", dbhost);
    readConfig("database", "passwd", passwd);

    MYSQL *mysql_conn = mysql_init(NULL); // Initialize a MySQL connection handle
    if (mysql_conn == NULL)
    {
        fprintf(stderr, "findReflector: mysql_init() failed\n");
        exit(1);
    }

    // Establish a connection to the MariaDB server
    if (mysql_real_connect(mysql_conn, dbhost, "openmt", passwd, "", 0, NULL, 0) == NULL)
    {
        fprintf(stderr, "%s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return false;
    }

    std::string query;
    query = "SELECT ip4, port FROM modem_host.reflectors WHERE refl_type = '";
    query.append(type).append("' AND Nick = '").append(name).append("'");
    if (mysql_query(mysql_conn, query.c_str()))
    { // Execute a query
        fprintf(stderr, "findReflector: mysql_query failed: %s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return false;
    }

    MYSQL_RES *result; // Result set
    result = mysql_store_result(mysql_conn); // Store the result set
    if (result == NULL)
    {
        fprintf(stderr, "DashCom: failed: %s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return false;
    }
    else
    {
        MYSQL_ROW row;
        if ((row = mysql_fetch_row(result)) != NULL)
        { // Fetch row
             mysql_free_result(result); // Free the result set
             strcpy(ip4, row[0]);
             *port = atoi(row[1]);
        }
        else
        {
            mysql_close(mysql_conn);
            return false;
        }
    }

    mysql_close(mysql_conn);
    return true;
}

bool setReflectorStatus(const char* type, const char* name, const char* module)
{
    char dbhost[20] = "localhost";
    char passwd[30] = "";

    readConfig("database", "host", dbhost);
    readConfig("database", "passwd", passwd);

    MYSQL *mysql_conn = mysql_init(NULL); // Initialize a MySQL connection handle
    if (mysql_conn == NULL)
    {
        fprintf(stderr, "setReflectorStatus: mysql_init() failed\n");
        exit(1);
    }

    // Establish a connection to the MariaDB server
    if (mysql_real_connect(mysql_conn, dbhost, "openmt", passwd, "", 0, NULL, 0) == NULL)
    {
        fprintf(stderr, "%s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return false;
    }

    std::string query;

    query = "UPDATE modem_host.reflectors SET status = '";
    query.append(module).append("' WHERE refl_type = '");
    query.append(type).append("' AND Nick = '").append(name).append("'");

    if (mysql_query(mysql_conn, query.c_str()))
    { // Execute a query
        fprintf(stderr, "setReflectorStatus: mysql_query failed: %s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return false;
    }

    mysql_close(mysql_conn);
    return true;
}

bool logError(const char* module, const char* message)
{
    char dbhost[20] = "localhost";
    char passwd[30] = "";

    readConfig("database", "host", dbhost);
    readConfig("database", "passwd", passwd);

    MYSQL *mysql_conn = mysql_init(NULL); // Initialize a MySQL connection handle
    if (mysql_conn == NULL)
    {
        fprintf(stderr, "logError: mysql_init() failed\n");
        exit(1);
    }

    // Establish a connection to the MariaDB server
    if (mysql_real_connect(mysql_conn, dbhost, "openmt", passwd, "", 0, NULL, 0) == NULL)
    {
        fprintf(stderr, "%s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return false;
    }

    std::string query;
    query = "INSERT INTO modem_host.errors SET module = '";
    query.append(module).append("', message = '").append(message).append("', ");
    query.append("datetime = NOW()");

    if (mysql_query(mysql_conn, query.c_str()))
    { // Execute a query
        fprintf(stderr, "logError: mysql_query failed: %s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return false;
    }

    mysql_close(mysql_conn);
    return true;
}

bool addStatus(const char* property, const char* value)
{
    char dbhost[20] = "localhost";
    char passwd[30] = "";

    readConfig("database", "host", dbhost);
    readConfig("database", "passwd", passwd);

    MYSQL *mysql_conn = mysql_init(NULL); // Initialize a MySQL connection handle
    if (mysql_conn == NULL)
    {
        fprintf(stderr, "addStatus: mysql_init() failed\n");
        exit(1);
    }

    // Establish a connection to the MariaDB server
    if (mysql_real_connect(mysql_conn, dbhost, "openmt", passwd, "", 0, NULL, 0) == NULL)
    {
        fprintf(stderr, "%s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return false;
    }

    std::string query;
    query = "INSERT INTO modem_host.host_status SET property = '";
    query.append(property).append("', value = '").append(value).append("'");

    if (mysql_query(mysql_conn, query.c_str()))
    { // Execute a query
        fprintf(stderr, "addStatus: mysql_query failed: %s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return false;
    }

    mysql_close(mysql_conn);
    return true;
}

bool updateStatus(const char* property, const char* value)
{
    char dbhost[20] = "localhost";
    char passwd[30] = "";

    readConfig("database", "host", dbhost);
    readConfig("database", "passwd", passwd);

    MYSQL *mysql_conn = mysql_init(NULL); // Initialize a MySQL connection handle
    if (mysql_conn == NULL)
    {
        fprintf(stderr, "updateStatus: mysql_init() failed\n");
        exit(1);
    }

    // Establish a connection to the MariaDB server
    if (mysql_real_connect(mysql_conn, dbhost, "openmt", passwd, "", 0, NULL, 0) == NULL)
    {
        fprintf(stderr, "%s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return false;
    }

    std::string query;
    query = "UPDATE modem_host.host_status SET value = '";
    query.append(value).append("' WHERE property = '").append(property).append("'");

    if (mysql_query(mysql_conn, query.c_str()))
    { // Execute a query
        fprintf(stderr, "updateStatus: mysql_query failed: %s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return false;
    }

    mysql_close(mysql_conn);
    return true;
}

bool delStatus(const char* property)
{
    char dbhost[20] = "localhost";
    char passwd[30] = "";

    readConfig("database", "host", dbhost);
    readConfig("database", "passwd", passwd);

    MYSQL *mysql_conn = mysql_init(NULL); // Initialize a MySQL connection handle
    if (mysql_conn == NULL)
    {
        fprintf(stderr, "delStatus: mysql_init() failed\n");
        exit(1);
    }

    // Establish a connection to the MariaDB server
    if (mysql_real_connect(mysql_conn, dbhost, "openmt", passwd, "", 0, NULL, 0) == NULL)
    {
        fprintf(stderr, "%s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return false;
    }

    std::string query;
    query = "DELETE FROM modem_host.host_status WHERE property = '";
    query.append(property).append("'");

    if (mysql_query(mysql_conn, query.c_str()))
    { // Execute a query
        fprintf(stderr, "delStatus: mysql_query failed: %s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return false;
    }

    mysql_close(mysql_conn);
    return true;
}

bool addGPS(const int id, const float latitude, const float longitude,
            const uint16_t altitude, const uint16_t bearing, const uint16_t speed)
{
    char dbhost[20] = "localhost";
    char passwd[30] = "";

    readConfig("database", "host", dbhost);
    readConfig("database", "passwd", passwd);

    MYSQL *mysql_conn = mysql_init(NULL); // Initialize a MySQL connection handle
    if (mysql_conn == NULL)
    {
        fprintf(stderr, "addGPS: mysql_init() failed\n");
        exit(1);
    }

    // Establish a connection to the MariaDB server
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
    { // Execute a query
        fprintf(stderr, "addGPS: mysql_query failed: %s\n", mysql_error(mysql_conn));
        mysql_close(mysql_conn);
        return false;
    }

    mysql_close(mysql_conn);
    return true;
}
