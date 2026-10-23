/*
 * Created by Rick KD0OSS
 *
 * error_test.c - tests reportError() and errorFlush() from tools.c.
 *
 * This file supplies its own logError(), which records what would be written
 * to the database, slowly, as a busy database would, and is linked in place of
 * the one in tools.c.  It checks that
 *   1. an error is passed to logError() with its modem, module and text,
 *   2. the same error sent again within 10 seconds is not written again,
 *   3. a different error is written,
 *   4. reportError() does not wait for the database,
 *   5. errorFlush() waits until everything queued has been written, and
 *   6. a forked child (which has no copy of the writer thread) still writes
 *      its errors, and the exit of the child flushes them.
 *
 * Build: see run_error_test.sh
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include "tools.h"

static int  written = 0;
static int  delayUs = 100000;
static char last[300];
static char lastModem[64];
static char lastModule[64];

/*
 * logError()
 *
 * Test replacement: records the error and takes 100 ms, as a slow database.
 *
 * Parameters:
 *   modem_name - Modem name given to reportError().
 *   module     - Module given to reportError().
 *   message    - The text.
 *
 * Returns: Always true.
 */
bool logError(const char* modem_name, const char* module, const char* message)
{
    usleep(delayUs);
    snprintf(lastModem, sizeof(lastModem), "%s", modem_name);
    snprintf(lastModule, sizeof(lastModule), "%s", module);
    snprintf(last, sizeof(last), "%s", message);
    written++;
    return true;
}

static int fails = 0;

/*
 * check()
 *
 * Prints one result.
 *
 * Parameters:
 *   ok   - true if the check passed.
 *   text - What was checked.
 *
 * Returns: nothing.
 */
static void check(bool ok, const char* text)
{
    printf("  %s: %s\n", ok ? "ok" : "FAIL", text);
    if (!ok)
        fails++;
}

/*
 * now_ms()
 *
 * Gives a time in milliseconds.
 *
 * Parameters: none.
 *
 * Returns: Milliseconds from a fixed point.
 */
static long now_ms(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

/*
 * main()
 *
 * Program entry.  Runs the checks described at the top of the file.
 *
 * Parameters: none.
 *
 * Returns: 0 if all checks pass, 1 if one fails.
 */
int main(void)
{
    long t0 = now_ms();

    reportError("modem1", "P25", "socket %d failed", 7);
    long took = now_ms() - t0;

    check(took < 50, "reportError() returns at once, without waiting for the database");

    errorFlush();
    check(written == 1, "the error was written after the flush");
    check(strcmp(last, "socket 7 failed") == 0 && strcmp(lastModem, "modem1") == 0 && strcmp(lastModule, "P25") == 0,
          "the modem, module and text were passed on");

    reportError("modem1", "P25", "socket %d failed", 7);
    errorFlush();
    check(written == 1, "the same error again within 10 seconds is not written again");

    reportError("modem1", "P25", "socket %d failed", 8);
    reportError("modem1", "DMR", "socket %d failed", 7);
    errorFlush();
    check(written == 3, "different errors are written");

    delayUs = 20000;
    for (int i = 0; i < 100; i++)
        reportError("modem1", "P25", "burst %d", i);
    t0 = now_ms();
    errorFlush();
    long flushed = now_ms() - t0;
    check(written >= 3 + 32 && written <= 3 + 36, "a flood is limited to the queue size, and what was queued is written");
    check(flushed < 2900, "the flush returns when the queue is empty, not at its time limit");
    printf("  (%d written, flush took %ld ms)\n", written, flushed);

    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0)
    {
        written = 0;
        reportError("modem2", "child", "child error");
        errorFlush();
        exit(written == 1 && strcmp(lastModule, "child") == 0 ? 0 : 3);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "a forked child writes its own errors (it has no copy of the writer thread)");

    printf("\n%s\n", fails == 0 ? "PASS" : "FAIL");
    return fails == 0 ? 0 : 1;
}
