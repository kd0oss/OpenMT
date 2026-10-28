/* Test stand-in for the database functions: settings come from the file named
 * by STUBCFG (lines module|key|value); writes go to the file named by STUBOUT. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

static bool lookup(const char* module, const char* key, char* value, size_t size)
{
    const char* path = getenv("STUBCFG");
    FILE*       f    = path ? fopen(path, "r") : NULL;
    char        line[256];
    bool        found = false;

    if (size > 0) value[0] = '\0';
    if (!f) return false;
    while (fgets(line, sizeof(line), f))
    {
        char* m = strtok(line, "|");
        char* k = strtok(NULL, "|");
        char* v = strtok(NULL, "\n");
        if (m && k && strcasecmp(m, module) == 0 && strcasecmp(k, key) == 0)
        {
            snprintf(value, size, "%s", v ? v : "");
            found = true;
        }
    }
    fclose(f);
    return found;
}

bool readHostConfigN(const char* modem_name, const char* module, const char* key, char* value, size_t size)
{
    (void)modem_name;
    lookup(module, key, value, size);
    return true;
}

bool readHostConfig(const char* modem_name, const char* module, const char* key, char* value)
{
    return readHostConfigN(modem_name, module, key, value, 0x7fffffff);
}

static void record(const char* what, const char* module, const char* key, const char* value)
{
    const char* path = getenv("STUBOUT");
    FILE*       f    = path ? fopen(path, "a") : NULL;

    if (f) { fprintf(f, "%s|%s|%s|%s\n", what, module, key, value); fclose(f); }
}

bool setHostConfig(const char* modem_name, const char* module, const char* key, const char* display_type, const char* value)
{
    (void)modem_name; (void)display_type;
    record("set", module, key, value);
    return true;
}

bool addHostConfigDefault(const char* modem_name, const char* module, const char* key, const char* display_type, const char* value)
{
    char tmp[64];
    (void)modem_name; (void)display_type;
    if (!lookup(module, key, tmp, sizeof(tmp)))
        record("default", module, key, value);
    return true;
}

bool ackDashbCommand(const char* modem_name, const char* command, const char* result)
{
    (void)modem_name;
    record("ack", command, "result", result);
    return true;
}

uint64_t nowMs(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}
