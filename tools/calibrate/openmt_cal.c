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
 ***************************************************************************/

/*
 * openmt_cal - RX frequency calibration per mode (modem type openmths).
 *
 * Every mode has its own RX frequency offset, in Hz, the "rxOffset" setting in the section of the mode (DMR, DSTAR,
 * P25, ...) of the config table.  Modem_Host adds it to the receive frequency of the mode before the mode is sent to
 * the modem.  This program measures the error and saves the offset.
 *
 * It does not talk to the modem itself.  It asks Modem_Host through the dashboard command table, the way the
 * dashboard does, so Modem_Host and the protocol service of the mode must be running.  Put a transmitter with an
 * accurate frequency (a signal generator or a good radio, key down, a modulated or an unmodulated carrier) on the
 * receive frequency of the mode, at a signal strength between -50 and -100 dBm, then run
 *
 *     openmt_cal auto DMR
 *
 * The modem receives the signal with the ADF7021 AFC on, moves its receive frequency by a known step and receives
 * again.  The two AFC readings give the error in Hz without knowing the scale of the readback.  The program adds the
 * error to the offset, saves it, lets Modem_Host load the mode again and measures once more, until the error is below
 * the tolerance.
 */

#include <ctype.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tools.h"

#define OFFSET_LIMIT 20000L

static char modemName[16]  = "modem1";
static long toleranceHz    = 30;     /* the error under which a mode is calibrated */
static int  readings       = 3;      /* measurements per pass, the median is used */
static int  maxPasses      = 4;
static int  settleSec      = 3;      /* wait after a mode was loaded again */

typedef struct
{
    long err;       /* Hz to add to the offset */
    long off;       /* offset in use */
    int  rssi;      /* -dBm */
    int  syncs;
    int  a0;
    int  a1;
    int  step;
} Measurement;

/*
 * usage()
 *
 * Purpose: Prints the command line help.
 *
 * Parameters: program - name the program was started with.
 *
 * Returns: nothing.
 */
static void usage(const char* program)
{
    fprintf(stderr,
            "Usage: %s [options] <command> [arguments]\n"
            "\n"
            "Commands:\n"
            "  list                 show the RX offset of every mode\n"
            "  measure <mode>       measure the frequency error once\n"
            "  auto <mode>          measure, correct and verify until the error is small\n"
            "  manual <mode>        step the offset by hand and watch the error\n"
            "  raw <mode>           print the raw receiver readback words (to check the AFC readback)\n"
            "  sweep <mode>         read the AFC word over time and at several frequency offsets, AFC off and on\n"
            "  set <mode> <hz>      set the offset (+-%ld Hz)\n"
            "  reset <mode>|all     set the offset to 0\n"
            "\n"
            "Options:\n"
            "  -m <n>     modem number (default 1)\n"
            "  -t <hz>    tolerance of auto (default %ld)\n"
            "  -r <n>     measurements per pass (default %d, the median is used)\n"
            "  -n <n>     most passes of auto (default %d)\n"
            "  -s <host> -u <user> -p <password> -c <file>   database server, user, password, ini file\n"
            "\n"
            "<mode> is the name of the protocol service: DMR, DSTAR, P25, YSF, NXDN, M17.  Modem_Host and the service\n"
            "must be running, and a reference transmitter must be on the receive frequency.\n",
            program, OFFSET_LIMIT, toleranceHz, readings, maxPasses);
}

/*
 * modeOk()
 *
 * Purpose: Checks that a mode name is a plain word, since it is used as a section name and in a command.
 *
 * Parameters: mode - the name.
 *
 * Returns: true if it is 1 to 10 letters or digits.
 */
static bool modeOk(const char* mode)
{
    size_t n = strlen(mode);

    if (n < 1 || n > 10)
        return false;
    for (size_t i = 0; i < n; i++)
    {
        if (!isalnum((unsigned char)mode[i]))
            return false;
    }
    return true;
}

/*
 * modemCommand()
 *
 * Purpose: Sends a command to Modem_Host and waits for its answer.
 *
 * Parameters: parameter - the command text; result - output; size - its size.
 *
 * Returns: true if Modem_Host answered.
 */
static bool modemCommand(const char* parameter, char* result, size_t size)
{
    if (!sendDashbCommand(modemName, "modemHost", parameter, result, size, 15))
    {
        fprintf(stderr, "No answer from Modem_Host for %s. Is it running, and is the database reachable?\n", modemName);
        return false;
    }
    return true;
}

/*
 * parseField()
 *
 * Purpose: Reads "name=<number>" out of a result text.
 *
 * Parameters: text - the result; name - field name; value - output.
 *
 * Returns: true if the field is there.
 */
static bool parseField(const char* text, const char* name, long* value)
{
    char key[24];

    snprintf(key, sizeof(key), " %s=", name);
    const char* p = strstr(text, key);
    if (p == NULL)
        return false;
    *value = strtol(p + strlen(key), NULL, 10);
    return true;
}

/*
 * measureOnce()
 *
 * Purpose: One measurement of a mode.
 *
 * Parameters: mode - the mode; m - output.
 *
 * Returns: true on success; on failure the reason has been printed.
 */
static bool measureOnce(const char* mode, Measurement* m)
{
    char cmd[40];
    char res[128];
    long v;

    snprintf(cmd, sizeof(cmd), "calibrate %s", mode);
    if (!modemCommand(cmd, res, sizeof(res)))
        return false;

    if (strncmp(res, "ok ", 3) != 0)
    {
        fprintf(stderr, "  %s\n", res);
        return false;
    }

    memset(m, 0, sizeof(*m));
    /* a leading space makes every field findable the same way */
    char text[140];
    snprintf(text, sizeof(text), " %s", res);
    if (!parseField(text, "err", &m->err))
        return false;
    if (parseField(text, "off", &v)) m->off = v;
    if (parseField(text, "rssi", &v)) m->rssi = (int)v;
    if (parseField(text, "syncs", &v)) m->syncs = (int)v;
    if (parseField(text, "a0", &v)) m->a0 = (int)v;
    if (parseField(text, "a1", &v)) m->a1 = (int)v;
    if (parseField(text, "step", &v)) m->step = (int)v;
    return true;
}

/*
 * compareLong()
 *
 * Purpose: qsort() order for long values.
 */
static int compareLong(const void* a, const void* b)
{
    long x = *(const long*)a;
    long y = *(const long*)b;

    return (x > y) - (x < y);
}

/*
 * measureMedian()
 *
 * Purpose: Measures a mode several times and takes the median error, which keeps one bad reading from moving the
 *          offset.
 *
 * Parameters: mode - the mode; m - output (the last reading, with the median error).
 *
 * Returns: true if more than half of the measurements worked.
 */
static bool measureMedian(const char* mode, Measurement* m)
{
    long errs[16];
    int good = 0;
    int n = readings > 16 ? 16 : readings;

    for (int i = 0; i < n; i++)
    {
        Measurement one;

        if (measureOnce(mode, &one))
        {
            errs[good++] = one.err;
            *m = one;
            printf("    reading %d: error %+ld Hz (signal -%d dBm, AFC %d / %d)\n", i + 1, one.err, one.rssi, one.a0, one.a1);
            fflush(stdout);
        }
    }

    if (good * 2 <= n)
    {
        fprintf(stderr, "  Only %d of %d measurements worked.\n", good, n);
        return false;
    }

    qsort(errs, good, sizeof(long), compareLong);
    m->err = errs[good / 2];

    if (errs[good - 1] - errs[0] > 1000)
    {
        fprintf(stderr, "  The readings disagree by %ld Hz, so none of them can be trusted.\n"
                        "  Use a steady unmodulated carrier at -50 to -100 dBm, or run 'raw' to check the AFC readback.\n",
                errs[good - 1] - errs[0]);
        return false;
    }
    return true;
}

/*
 * cmdRaw()
 *
 * Purpose: Prints the sixteen words the receiver readback register can select, with the AFC on, off and on with the
 *          frequency moved, so one can see which word follows the signal (it should change between the first and the
 *          third column by about the same amount every time, and not at all in the second).
 *
 * Parameters: mode - the mode.
 *
 * Returns: 0 on success.
 */
static int cmdRaw(const char* mode)
{
    unsigned words[3][16];

    memset(words, 0, sizeof(words));
    for (int pass = 0; pass < 3; pass++)
    {
        for (int half = 0; half < 2; half++)
        {
            char cmd[40];
            char res[128];

            snprintf(cmd, sizeof(cmd), "probe %s %d %d", mode, pass, half);
            if (!modemCommand(cmd, res, sizeof(res)))
                return 1;
            if (strncmp(res, "raw ", 4) != 0)
            {
                fprintf(stderr, "  %s\n", res);
                return 1;
            }

            const char* p = res + 4;
            char* end;

            strtol(p, &end, 10);    /* pass */
            p = end;
            strtol(p, &end, 10);    /* half */
            p = end;
            for (int i = 0; i < 8; i++)
            {
                words[pass][half * 8 + i] = (unsigned)strtoul(p, &end, 16);
                p = end;
            }
        }
    }

    printf("Readback words of %s (reg7 = 0x0107 | n << 4):\n", mode);
    printf("   n   reg7    AFC on    AFC off   AFC on, moved\n");
    for (int n = 0; n < 16; n++)
        printf("  %2d  0x%04X   %04X %6d   %04X %6d   %04X %6d\n", n, 0x0107 | (n << 4), words[0][n], (int16_t)words[0][n],
               words[1][n], (int16_t)words[1][n], words[2][n], (int16_t)words[2][n]);
    return 0;
}

/*
 * cmdSweep()
 *
 * Purpose: Moves the receive frequency over +-64 synthesizer counts (about +-14 kHz in the UHF1 band) with the AFC off
 *          and on and prints the AFC readback four times each (every 60 ms), the RSSI, the filter calibration and the
 *          revision word.  The column that follows the shift tells how to read the AFC.
 *
 * Parameters: mode - the mode.
 *
 * Returns: 0 on success.
 */
static int cmdSweep(const char* mode)
{
    printf("Readback of %s with the receive frequency moved (1 count = PFD/65536 Hz, 225 Hz at a 14.7456 MHz PFD):\n", mode);
    printf("  AFC  shift     afc@60  @120  @180  @240    rssi  fcal   rev\n");

    for (int afc = 0; afc < 2; afc++)
    {
        for (int pass = 16; pass <= 20; pass++)
        {
            char cmd[40];
            char res[128];

            snprintf(cmd, sizeof(cmd), "probe %s %d %d", mode, pass, afc);
            if (!modemCommand(cmd, res, sizeof(res)))
                return 1;
            if (strncmp(res, "raw ", 4) != 0)
            {
                fprintf(stderr, "  %s\n", res);
                return 1;
            }

            const char* p = res + 4;
            char* end;
            long w[8];

            strtol(p, &end, 10);
            p = end;
            strtol(p, &end, 10);
            p = end;
            for (int i = 0; i < 8; i++)
            {
                w[i] = strtol(p, &end, 16);
                p = end;
            }
            printf("  %-4s %+5d  %8d %5d %5d %5d  %6ld %5ld  %04lX\n", afc ? "on" : "off", (pass - 18) * 32, (int16_t)w[0],
                   (int16_t)w[1], (int16_t)w[2], (int16_t)w[3], w[4], w[5], w[6]);
            fflush(stdout);
        }
    }
    return 0;
}

/*
 * readOffset()
 *
 * Purpose: Reads the RX offset of a mode from the config table.
 *
 * Parameters: mode - the mode.
 *
 * Returns: the offset in Hz (0 if there is none).
 */
static long readOffset(const char* mode)
{
    char tmp[32] = "";

    readHostConfig(modemName, mode, "rxOffset", tmp);
    return strtol(tmp, NULL, 10);
}

/*
 * writeOffset()
 *
 * Purpose: Saves the RX offset of a mode and has Modem_Host load the mode again with it.
 *
 * Parameters: mode - the mode; hz - the offset (limited to +-20 kHz).
 *
 * Returns: true if it was saved and Modem_Host answered.
 */
static bool writeOffset(const char* mode, long hz)
{
    char tmp[32];
    char res[64];

    if (hz > OFFSET_LIMIT)
        hz = OFFSET_LIMIT;
    if (hz < -OFFSET_LIMIT)
        hz = -OFFSET_LIMIT;

    snprintf(tmp, sizeof(tmp), "%ld", hz);
    if (!setHostConfig(modemName, mode, "rxOffset", "rxoffset", tmp))
    {
        fprintf(stderr, "Could not save the offset in the database.\n");
        return false;
    }

    if (!modemCommand("reloadOffsets", res, sizeof(res)))
        return false;

    sleep((unsigned)settleSec);
    return true;
}

/*
 * cmdList()
 *
 * Purpose: Lists the offsets of all modes that have one.
 *
 * Returns: 0 on success, 1 on a database error.
 */
static int cmdList(void)
{
    static const char* known[] = {"DMR", "DSTAR", "P25", "YSF", "NXDN", "M17", "POCSAG", "FM"};

    printf("RX frequency offsets of %s:\n", modemName);
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++)
    {
        char tmp[32] = "";

        readHostConfig(modemName, known[i], "rxOffset", tmp);
        if (tmp[0] != '\0')
            printf("  %-8s %+6ld Hz\n", known[i], strtol(tmp, NULL, 10));
    }
    return 0;
}

/*
 * cmdMeasure()
 *
 * Purpose: One measurement, printed.
 *
 * Parameters: mode - the mode.
 *
 * Returns: 0 on success, 1 on failure.
 */
static int cmdMeasure(const char* mode)
{
    Measurement m;

    if (!measureMedian(mode, &m))
        return 1;

    printf("%s: error %+ld Hz with offset %+ld Hz in use; a new offset of %+ld Hz would centre it.\n", mode, m.err, m.off,
           m.off + m.err);
    return 0;
}

/*
 * cmdAuto()
 *
 * Purpose: Measures, corrects and verifies a mode until the error is below the tolerance.
 *
 * Parameters: mode - the mode.
 *
 * Returns: 0 when calibrated, 1 otherwise.
 */
static int cmdAuto(const char* mode)
{
    long start = readOffset(mode);

    printf("Calibrating %s (offset now %+ld Hz, tolerance %ld Hz).\n", mode, start, toleranceHz);

    for (int pass = 1; pass <= maxPasses; pass++)
    {
        Measurement m;

        printf("  Pass %d:\n", pass);
        if (!measureMedian(mode, &m))
        {
            printf("Stopped. The offset is %+ld Hz.\n", readOffset(mode));
            return 1;
        }

        printf("  Pass %d: error %+ld Hz with offset %+ld Hz.\n", pass, m.err, m.off);

        if (labs(m.err) <= toleranceHz)
        {
            printf("%s is calibrated: RX offset %+ld Hz (residual error %+ld Hz).\n", mode, m.off, m.err);
            return 0;
        }

        long next = m.off + m.err;
        if (labs(next) > OFFSET_LIMIT)
        {
            printf("The offset would be %+ld Hz, more than the +-%ld Hz allowed. Check the reference and the TCXO.\n", next,
                   OFFSET_LIMIT);
            return 1;
        }

        printf("  Setting the offset to %+ld Hz.\n", next);
        if (!writeOffset(mode, next))
            return 1;
    }

    printf("Not within %ld Hz after %d passes. The offset is %+ld Hz.\n", toleranceHz, maxPasses, readOffset(mode));
    return 1;
}

/*
 * cmdManual()
 *
 * Purpose: Lets the user step the offset and watch the measured error, for a fine adjustment or to check a mode.
 *          Every change is saved and loaded at once.
 *
 * Parameters: mode - the mode.
 *
 * Returns: 0.
 */
static int cmdManual(const char* mode)
{
    long start = readOffset(mode);
    long offset = start;
    long step = 10;
    char line[64];
    bool measure = true;

    printf("Manual calibration of %s, offset %+ld Hz.\n"
           "  +  -     offset up or down by the step     s <hz>  set the step (now %ld)\n"
           "  = <hz>   set the offset                    0       offset 0\n"
           "  m        measure again                     r       back to %+ld\n"
           "  q        quit, keeping the current offset\n",
           mode, offset, step, start);

    for (;;)
    {
        if (measure)
        {
            Measurement m;

            if (measureOnce(mode, &m))
                printf("offset %+ld Hz   error %+ld Hz   signal -%d dBm   syncs %d\n", offset, m.err, m.rssi, m.syncs);
            measure = false;
        }

        printf("> ");
        fflush(stdout);
        if (fgets(line, sizeof(line), stdin) == NULL)
            break;

        char* p = line;
        while (*p == ' ')
            p++;

        if (*p == 'q' || *p == 'Q')
            break;
        else if (*p == '+' || *p == '-')
        {
            offset += (*p == '+') ? step : -step;
            writeOffset(mode, offset);
            measure = true;
        }
        else if (*p == '=')
        {
            offset = strtol(p + 1, NULL, 10);
            writeOffset(mode, offset);
            measure = true;
        }
        else if (*p == '0')
        {
            offset = 0;
            writeOffset(mode, offset);
            measure = true;
        }
        else if (*p == 'r' || *p == 'R')
        {
            offset = start;
            writeOffset(mode, offset);
            measure = true;
        }
        else if (*p == 's' || *p == 'S')
        {
            long v = strtol(p + 1, NULL, 10);

            if (v >= 1 && v <= 1000)
                step = v;
        }
        else if (*p == 'm' || *p == 'M')
            measure = true;
    }

    printf("%s: RX offset %+ld Hz.\n", mode, offset);
    return 0;
}

/*
 * main()
 *
 * Purpose: Reads the options and runs the command.
 *
 * Parameters: argc, argv - see usage().
 *
 * Returns: 0 on success.
 */
int main(int argc, char** argv)
{
    int c;

    while ((c = getopt(argc, argv, ":m:t:r:n:s:u:p:c:h")) != -1)
    {
        switch (c)
        {
            case 'm':
                snprintf(modemName, sizeof(modemName), "modem%d", atoi(optarg));
                break;
            case 't':
                toleranceHz = atol(optarg);
                break;
            case 'r':
                readings = atoi(optarg) < 1 ? 1 : atoi(optarg);
                break;
            case 'n':
                maxPasses = atoi(optarg) < 1 ? 1 : atoi(optarg);
                break;
            case 's':
            case 'u':
            case 'p':
            case 'c':
                dbOption(c, optarg);
                break;
            case ':':
                fprintf(stderr, "Option '-%c' requires a value.\n", optopt);
                return 1;
            default:
                usage(argv[0]);
                return c == 'h' ? 0 : 1;
        }
    }

    if (optind >= argc)
    {
        usage(argv[0]);
        return 1;
    }

    const char* command = argv[optind];
    const char* mode = (optind + 1 < argc) ? argv[optind + 1] : NULL;

    if (strcmp(command, "list") == 0)
        return cmdList();

    if (mode == NULL || (strcmp(mode, "all") != 0 && !modeOk(mode)))
    {
        fprintf(stderr, "%s needs a mode name such as DMR.\n", command);
        return 1;
    }

    if (strcmp(command, "measure") == 0)
        return cmdMeasure(mode);
    if (strcmp(command, "auto") == 0)
        return cmdAuto(mode);
    if (strcmp(command, "raw") == 0)
        return cmdRaw(mode);
    if (strcmp(command, "sweep") == 0)
        return cmdSweep(mode);
    if (strcmp(command, "manual") == 0)
        return cmdManual(mode);
    if (strcmp(command, "set") == 0)
    {
        if (optind + 2 >= argc)
        {
            fprintf(stderr, "set needs a mode and an offset in Hz.\n");
            return 1;
        }
        long hz = strtol(argv[optind + 2], NULL, 10);

        settleSec = 0;
        if (!writeOffset(mode, hz))
            return 1;
        printf("%s: RX offset %+ld Hz.\n", mode, hz);
        return 0;
    }
    if (strcmp(command, "reset") == 0)
    {
        static const char* all[] = {"DMR", "DSTAR", "P25", "YSF", "NXDN", "M17", "POCSAG"};

        settleSec = 0;
        if (strcmp(mode, "all") == 0)
        {
            for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++)
            {
                char tmp[32] = "";

                readHostConfig(modemName, all[i], "rxOffset", tmp);
                if (tmp[0] != '\0' && !writeOffset(all[i], 0))
                    return 1;
            }
            printf("All RX offsets are 0.\n");
            return 0;
        }
        if (!writeOffset(mode, 0))
            return 1;
        printf("%s: RX offset 0 Hz.\n", mode);
        return 0;
    }

    usage(argv[0]);
    return 1;
}
