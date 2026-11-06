/***************************************************************************
 *   Copyright (C) 2026 by Rick KD0OSS                                     *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 3 of the License, or     *
 *   (at your option) any later version.                                   *
 ***************************************************************************/

#include "hrf_db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifdef HRF_NO_DB

void hrfDbDefaults(const char* modemName, const HrfSettings* d) { (void)modemName; (void)d; }
void hrfDbLoad(const char* modemName, HrfSettings* d) { (void)modemName; (void)d; }
void hrfDbPublish(const char* modemName, const char* linkPath) { (void)modemName; (void)linkPath; }
void hrfDbAckUnsupported(const char* modemName) { (void)modemName; }

#else

#include <libgen.h>

#include "bridge_common.h"
#include "tools.h"

static void rd(const char* modemName, const char* key, char* out, size_t size)
{
    out[0] = '\0';
    readHostConfigN(modemName, "config", key, out, size);
}

/*
 * hrfDbDefaults()
 *
 * Creates the settings that do not exist yet.
 *
 * Parameters:
 *   modemName - Name of the modem.
 *   d         - Default values (the command line values).
 *
 * Returns: nothing.
 */
void hrfDbDefaults(const char* modemName, const HrfSettings* d)
{
    char text[32];

    snprintf(text, sizeof(text), "%u", (unsigned)d->rx_freq_hz);
    bridgeDefault(modemName, "rxFrequency", text);
    snprintf(text, sizeof(text), "%u", (unsigned)d->tx_freq_hz);
    bridgeDefault(modemName, "txFrequency", text);
    snprintf(text, sizeof(text), "%u", (unsigned)d->lna_gain_db);
    bridgeDefault(modemName, "rxLnaGain", text);
    snprintf(text, sizeof(text), "%u", (unsigned)d->vga_gain_db);
    bridgeDefault(modemName, "rxPgaGain", text);
    snprintf(text, sizeof(text), "%u", (unsigned)d->tx_vga_db);
    bridgeDefault(modemName, "txDacGain", text);
    bridgeDefault(modemName, "rxInvert", d->rx_invert ? "true" : "false");
    bridgeDefault(modemName, "txInvert", d->tx_invert ? "true" : "false");
    snprintf(text, sizeof(text), "%u", (unsigned)d->rx_level);
    bridgeDefault(modemName, "rxLevel", text);
    snprintf(text, sizeof(text), "%g", d->ppm);
    bridgeDefault(modemName, "freqOffsetPpm", text);
    bridgeDefault(modemName, "mode", "simplex");
}

/*
 * hrfDbLoad()
 *
 * Reads the settings.
 *
 * Parameters:
 *   modemName - Name of the modem.
 *   d         - Receives the values; a setting that is empty (or, for the
 *               frequencies and the TX gain, 0 or less) keeps what is there.
 *
 * Returns: nothing.
 */
void hrfDbLoad(const char* modemName, HrfSettings* d)
{
    char t[32];
    long v;

    rd(modemName, "rxFrequency", t, sizeof(t));
    if (t[0] && (v = strtol(t, NULL, 10)) >= 1000000L)
        d->rx_freq_hz = (uint32_t)v;
    rd(modemName, "txFrequency", t, sizeof(t));
    if (t[0] && (v = strtol(t, NULL, 10)) >= 1000000L)
        d->tx_freq_hz = (uint32_t)v;
    rd(modemName, "rxLnaGain", t, sizeof(t));
    if (t[0] && (v = atol(t)) >= 0)     /* 0 is a real setting: the lowest gain */
        d->lna_gain_db = (uint32_t)((v > 40 ? 40 : v) / 8 * 8);
    rd(modemName, "rxPgaGain", t, sizeof(t));
    if (t[0] && (v = atol(t)) >= 0)
        d->vga_gain_db = (uint32_t)((v > 62 ? 62 : v) / 2 * 2);
    rd(modemName, "txDacGain", t, sizeof(t));
    if (t[0] && (v = atol(t)) > 0)
        d->tx_vga_db = (uint32_t)(v > 47 ? 47 : v);
    rd(modemName, "rxInvert", t, sizeof(t));
    if (t[0])
        d->rx_invert = (strcasecmp(t, "true") == 0);
    rd(modemName, "txInvert", t, sizeof(t));
    if (t[0])
        d->tx_invert = (strcasecmp(t, "true") == 0);
    rd(modemName, "rxLevel", t, sizeof(t));
    if (t[0] && (v = atol(t)) > 0)
        d->rx_level = (uint32_t)(v > 255 ? 255 : v);
    rd(modemName, "freqOffsetPpm", t, sizeof(t));
    if (t[0])
    {
        double p = atof(t);

        d->ppm = p < -100.0 ? -100.0 : (p > 100.0 ? 100.0 : p);
    }
}

/*
 * hrfDbPublish()
 *
 * Tells Modem_Host the stream type of this bridge and where to connect.
 *
 * Parameters:
 *   modemName - Name of the modem.
 *   linkPath  - The symbolic link of the virtual port, or NULL.
 *
 * Returns: nothing.
 */
void hrfDbPublish(const char* modemName, const char* linkPath)
{
    if (linkPath != NULL && linkPath[0] != '\0')
    {
        char copy[128];

        snprintf(copy, sizeof(copy), "%s", linkPath);
        bridgePublishPort(modemName, basename(copy));
    }
    bridgePublishStreamType(modemName, STREAM_SAMPLE);
    bridgePublishCaps(modemName, false, 12, false);
}

/* Answers a dashboard command this modem cannot do. */
void hrfDbAckUnsupported(const char* modemName)
{
    bridgeAckUnsupported(modemName);
}

#endif

bool hrfDbDiffer(const HrfSettings* a, const HrfSettings* b)
{
    return a->rx_freq_hz != b->rx_freq_hz || a->tx_freq_hz != b->tx_freq_hz ||
           a->lna_gain_db != b->lna_gain_db || a->vga_gain_db != b->vga_gain_db ||
           a->tx_vga_db != b->tx_vga_db || a->ppm != b->ppm || a->rx_level != b->rx_level || a->rx_invert != b->rx_invert || a->tx_invert != b->tx_invert;
}
