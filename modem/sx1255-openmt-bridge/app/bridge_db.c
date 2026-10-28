/***************************************************************************
 *   Copyright (C) 2025 by Rick KD0OSS                                     *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 3 of the License, or     *
 *   (at your option) any later version.                                   *
 ***************************************************************************/

#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "bridge_common.h"
#include "bridge_db.h"
#include "tools.h"

static void rd(const char* modemName, const char* key, char* out, size_t size)
{
    out[0] = '\0';
    readHostConfigN(modemName, "config", key, out, size);
}

/*
 * sxDbDefaults()
 *
 * Creates the settings of the SX1255 modem that do not exist yet.  The
 * names are the ones the dashboard and Modem_Host already use.
 *
 * Parameters:
 *   modemName - Name of the modem.
 *   d         - Default values.
 *
 * Returns: nothing.
 */
void sxDbDefaults(const char* modemName, const SxSettings* d)
{
    char text[32];

    snprintf(text, sizeof(text), "%u", (unsigned)d->rx_freq_hz);
    bridgeDefault(modemName, "rxFrequency", text);
    snprintf(text, sizeof(text), "%u", (unsigned)d->tx_freq_hz);
    bridgeDefault(modemName, "txFrequency", text);
    snprintf(text, sizeof(text), "%u", (unsigned)d->lna_gain_db);
    bridgeDefault(modemName, "rxLnaGain", text);
    snprintf(text, sizeof(text), "%u", (unsigned)d->pga_gain_db);
    bridgeDefault(modemName, "rxPgaGain", text);
    snprintf(text, sizeof(text), "%g", (double)d->mixer_gain_db);
    bridgeDefault(modemName, "txMixerGain", text);
    snprintf(text, sizeof(text), "%d", (int)d->dac_gain_db);
    bridgeDefault(modemName, "txDacGain", text);
    snprintf(text, sizeof(text), "%u", (unsigned)d->rx_pll_bw_khz);
    bridgeDefault(modemName, "rxPllBw", text);
    snprintf(text, sizeof(text), "%u", (unsigned)d->tx_pll_bw_khz);
    bridgeDefault(modemName, "txPllBw", text);
    bridgeDefault(modemName, "rxInvert", d->rx_invert ? "true" : "false");
    bridgeDefault(modemName, "txInvert", d->tx_invert ? "true" : "false");
    bridgeDefault(modemName, "mode", "simplex");
}

/*
 * sxDbLoad()
 *
 * Reads the settings of the SX1255 modem.
 *
 * Parameters:
 *   modemName - Name of the modem.
 *   d         - Receives the values; a setting that is empty keeps what is there.
 *
 * Returns: nothing.
 */
void sxDbLoad(const char* modemName, SxSettings* d)
{
    char t[32];

    rd(modemName, "rxFrequency", t, sizeof(t));
    if (t[0])
        d->rx_freq_hz = (uint32_t)strtoul(t, NULL, 10);
    rd(modemName, "txFrequency", t, sizeof(t));
    if (t[0])
        d->tx_freq_hz = (uint32_t)strtoul(t, NULL, 10);
    rd(modemName, "rxLnaGain", t, sizeof(t));
    if (t[0])
        d->lna_gain_db = (uint8_t)atoi(t);
    rd(modemName, "rxPgaGain", t, sizeof(t));
    if (t[0])
        d->pga_gain_db = (uint8_t)atoi(t);
    rd(modemName, "txMixerGain", t, sizeof(t));
    if (t[0])
        d->mixer_gain_db = (float)((int)(atof(t) * 2.0)) / 2.0f;    /* 2 dB steps, as the host sent them */
    rd(modemName, "txDacGain", t, sizeof(t));
    if (t[0])
        d->dac_gain_db = (int8_t)atoi(t);
    rd(modemName, "rxPllBw", t, sizeof(t));
    if (t[0])
        d->rx_pll_bw_khz = (uint16_t)atoi(t);
    rd(modemName, "txPllBw", t, sizeof(t));
    if (t[0])
        d->tx_pll_bw_khz = (uint16_t)atoi(t);
    rd(modemName, "rxInvert", t, sizeof(t));
    if (t[0])
        d->rx_invert = (strcasecmp(t, "true") == 0);
    rd(modemName, "txInvert", t, sizeof(t));
    if (t[0])
        d->tx_invert = (strcasecmp(t, "true") == 0);
}

/*
 * sxDbPublish()
 *
 * Tells Modem_Host the stream type of this bridge and where to connect.
 *
 * Parameters:
 *   modemName - Name of the modem.
 *   linkPath  - The symbolic link of the virtual port, or NULL.
 *
 * Returns: nothing.
 */
void sxDbPublish(const char* modemName, const char* linkPath)
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
void sxDbAckUnsupported(const char* modemName)
{
    bridgeAckUnsupported(modemName);
}

bool sxDbDiffer(const SxSettings* a, const SxSettings* b)
{
    return a->rx_freq_hz != b->rx_freq_hz || a->tx_freq_hz != b->tx_freq_hz ||
           a->lna_gain_db != b->lna_gain_db || a->pga_gain_db != b->pga_gain_db ||
           a->mixer_gain_db != b->mixer_gain_db || a->dac_gain_db != b->dac_gain_db ||
           a->rx_pll_bw_khz != b->rx_pll_bw_khz || a->tx_pll_bw_khz != b->tx_pll_bw_khz ||
           a->rx_invert != b->rx_invert || a->tx_invert != b->tx_invert;
}
