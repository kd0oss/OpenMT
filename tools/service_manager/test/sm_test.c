/*
 * Created by Rick KD0OSS
 *
 * Test of the service manager against a small fake of the database calls
 * and a fake systemctl script (fake_systemctl.sh).
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mariadb/mysql.h>

#include "../service_manager.c"

char* readConfig(const char* section, const char* key, char* value)
{
    (void)section;
    (void)key;
    (void)value;
    return NULL;
}

/* ---- fake database: two tables held in memory ---- */
struct cmd { long id; char unit[64]; char action[16]; char result[80]; int hasResult; };
static struct cmd cmds[16];
static int        ncmds = 0;
static const char* listed[] = { "fm-host.service", "fm-gateway.service", "dstar-host.service", "fail.service" };
static char        actState[4][32], enState[4][32];
static int         rowIdx, rowCount;
static char        rowBuf[10][3][80];
static char*       rowPtr[3];
static char        lastErr[8];
static int         poweroffListed = 1;

struct st_mysql { int x; };
struct st_res   { int x; };
static struct st_mysql dbObj;
static struct st_res   resObj;

MYSQL* mysql_init(MYSQL* m) { (void)m; return &dbObj; }
MYSQL* mysql_real_connect(MYSQL* m, const char* a, const char* b, const char* c, const char* d, unsigned e, const char* f, unsigned long g)
{ (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g; return m; }
void mysql_close(MYSQL* m) { (void)m; }
const char* mysql_error(MYSQL* m) { (void)m; return lastErr; }
unsigned long mysql_real_escape_string(MYSQL* m, char* to, const char* from, unsigned long n)
{ (void)m; memcpy(to, from, n); to[n] = '\0'; return n; }
unsigned long long mysql_affected_rows(MYSQL* m) { (void)m; return 0; }
void mysql_free_result(MYSQL_RES* r) { (void)r; }

int mysql_query(MYSQL* m, const char* q)
{
    (void)m;
    rowCount = rowIdx = 0;
    if (strncmp(q, "SELECT unit FROM modem_host.services", 36) == 0)
    {
        for (int i = 0; i < 4; i++)
            strcpy(rowBuf[rowCount++][0], listed[i]);
    }
    else if (strncmp(q, "SELECT COUNT(*) FROM modem_host.services WHERE unit = '", 55) == 0)
    {
        char u[80];
        snprintf(u, sizeof(u), "%s", q + 55);
        *strchr(u, '\'') = '\0';
        int found = 0;
        for (int i = 0; i < 4; i++)
            if (strcmp(u, listed[i]) == 0)
                found = 1;
        sprintf(rowBuf[rowCount++][0], "%d", found);
    }
    else if (strncmp(q, "SELECT COUNT(*) FROM modem_host.system_actions WHERE action = '", 63) == 0)
    {
        char a[40];
        snprintf(a, sizeof(a), "%s", q + 63);
        *strchr(a, '\'') = '\0';
        sprintf(rowBuf[rowCount++][0], "%d", strcmp(a, "reboot") == 0 || strcmp(a, "poweroff") == 0 ? (strcmp(a, "poweroff") == 0 ? poweroffListed : 1) : 0);
    }
    else if (strncmp(q, "SELECT id, unit, action", 23) == 0)
    {
        for (int i = 0; i < ncmds && rowCount < 10; i++)
        {
            if (cmds[i].hasResult)
                continue;
            sprintf(rowBuf[rowCount][0], "%ld", cmds[i].id);
            strcpy(rowBuf[rowCount][1], cmds[i].unit);
            strcpy(rowBuf[rowCount][2], cmds[i].action);
            rowCount++;
        }
    }
    else if (strncmp(q, "UPDATE modem_host.service_commands SET result = '", 49) == 0 && strstr(q, "WHERE id = "))
    {
        char r[80];
        snprintf(r, sizeof(r), "%s", q + 49);
        *strchr(r, '\'') = '\0';
        long id = atol(strstr(q, "WHERE id = ") + 11);
        for (int i = 0; i < ncmds; i++)
            if (cmds[i].id == id)
            {
                strcpy(cmds[i].result, r);
                cmds[i].hasResult = 1;
            }
    }
    else if (strncmp(q, "UPDATE modem_host.services SET active = '", 41) == 0)
    {
        char a[40], e[40], u[80];
        const char* p = q + 41;
        snprintf(a, sizeof(a), "%s", p);
        *strchr(a, '\'') = '\0';
        p = strstr(q, "enabled = '") + 11;
        snprintf(e, sizeof(e), "%s", p);
        *strchr(e, '\'') = '\0';
        p = strstr(q, "WHERE unit = '") + 14;
        snprintf(u, sizeof(u), "%s", p);
        *strchr(u, '\'') = '\0';
        for (int i = 0; i < 4; i++)
            if (strcmp(u, listed[i]) == 0)
            {
                strcpy(actState[i], a);
                strcpy(enState[i], e);
            }
    }
    return 0;
}

MYSQL_RES* mysql_store_result(MYSQL* m) { (void)m; return &resObj; }
MYSQL_ROW mysql_fetch_row(MYSQL_RES* r)
{
    (void)r;
    if (rowIdx >= rowCount)
        return NULL;
    for (int i = 0; i < 3; i++)
        rowPtr[i] = rowBuf[rowIdx][i];
    rowIdx++;
    return rowPtr;
}

static void add(long id, const char* unit, const char* action)
{
    cmds[ncmds].id = id;
    strcpy(cmds[ncmds].unit, unit);
    strcpy(cmds[ncmds].action, action);
    cmds[ncmds].hasResult = 0;
    ncmds++;
}

static void check(const char* what, int ok)
{
    printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        exit(1);
}

int main(void)
{
    char units[2][SM_UNIT_MAX] = { "a.service", "b.service" };
    char act[2][SM_UNIT_MAX], en[2][SM_UNIT_MAX];

    printf("unit and action checks\n");
    check("fm-host.service valid", smValidUnit("fm-host.service"));
    check("name without .service refused", !smValidUnit("fm-host"));
    check("leading dash refused", !smValidUnit("-fm.service"));
    check("path refused", !smValidUnit("../x.service"));
    check("space refused", !smValidUnit("a b.service"));
    check("shell characters refused", !smValidUnit("a;rm.service"));
    check("start valid", smValidAction("start"));
    check("poweroff refused", !smValidAction("poweroff"));
    check("empty refused", !smValidAction(""));

    printf("parsing systemctl show\n");
    strcpy(act[0], "unknown"); strcpy(act[1], "unknown");
    strcpy(en[0], "unknown"); strcpy(en[1], "unknown");
    smParseShow("Id=b.service\nActiveState=active\nUnitFileState=enabled\n\nId=a.service\nActiveState=failed\nUnitFileState=\n", units, 2, act, en);
    check("b is active and enabled", strcmp(act[1], "active") == 0 && strcmp(en[1], "enabled") == 0);
    check("a is failed, no boot state", strcmp(act[0], "failed") == 0 && strcmp(en[0], "none") == 0);
    smParseShow("Id=a.service\nLoadState=not-found\nActiveState=inactive\nUnitFileState=\n", units, 2, act, en);
    check("a missing service file gives not-installed", strcmp(act[0], "not-installed") == 0);

    printf("requests\n");
    setenv("SM_LOG", "/tmp/sm_test_calls.log", 1);
    unlink("/tmp/sm_test_calls.log");
    smSetSystemctl("./fake_systemctl.sh");
    add(1, "fm-host.service", "start");
    add(2, "fm-gateway.service", "enable");
    add(3, "other.service", "start");           /* not listed */
    add(4, "fm-host.service", "halt");          /* bad action */
    add(5, "fail.service", "stop");             /* systemctl fails */
    add(6, "a b.service", "start");             /* bad name */
    smSetPowerDelay(0);
    add(7, "system", "reboot");                 /* listed power action */
    add(8, "system", "poweroff");               /* not listed (see below) */
    add(9, "other.service", "reboot");          /* power action with a unit: refused */
    poweroffListed = 0;
    check("cycle works", smCycle(&dbObj, true));
    check("1 ok", strcmp(cmds[0].result, "ok") == 0);
    check("2 ok", strcmp(cmds[1].result, "ok") == 0);
    check("3 refused (not listed)", strcmp(cmds[2].result, "refused") == 0);
    check("4 refused (action)", strcmp(cmds[3].result, "refused") == 0);
    check("5 failed with the code", strcmp(cmds[4].result, "failed (code 5)") == 0);
    check("6 refused (name)", strcmp(cmds[5].result, "refused") == 0);

    check("7 reboot ok", strcmp(cmds[6].result, "ok") == 0);
    check("8 poweroff refused when not listed", strcmp(cmds[7].result, "refused") == 0);
    check("9 power action with another unit refused", strcmp(cmds[8].result, "refused") == 0);
    check("power actions are checked", smValidPower("reboot") && smValidPower("poweroff") && !smValidPower("halt"));

    FILE* f = fopen("/tmp/sm_test_calls.log", "r");
    char  log[2048] = "";
    size_t n = f ? fread(log, 1, sizeof(log) - 1, f) : 0;
    log[n] = '\0';
    if (f)
        fclose(f);
    check("systemctl got start fm-host", strstr(log, "--no-ask-password start -- fm-host.service") != NULL);
    check("systemctl got enable fm-gateway", strstr(log, "enable -- fm-gateway.service") != NULL);
    check("not listed unit never reached systemctl", strstr(log, "other.service") == NULL);
    check("systemctl got reboot once", strstr(log, "--no-ask-password reboot\n") != NULL && strstr(strstr(log, "reboot\n") + 1, "reboot\n") == NULL);
    check("poweroff never reached systemctl", strstr(log, "poweroff") == NULL);
    check("bad action never reached systemctl", strstr(log, "halt") == NULL);

    printf("state copied to the table\n");
    check("fm-host active, enabled", strcmp(actState[0], "active") == 0 && strcmp(enState[0], "enabled") == 0);
    check("fm-gateway inactive, disabled", strcmp(actState[1], "inactive") == 0 && strcmp(enState[1], "disabled") == 0);
    check("dstar-host inactive, none", strcmp(actState[2], "inactive") == 0 && strcmp(enState[2], "none") == 0);

    printf("PASS\n");
    return 0;
}
