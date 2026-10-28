/***************************************************************************
 *   Copyright (C) 2025 by Rick KD0OSS                                     *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 3 of the License, or     *
 *   (at your option) any later version.                                   *
 ***************************************************************************/

/*
 * The SX1255 bridge's settings in the config database.  Kept apart from
 * bridge.c so the database headers stay out of the radio code.
 */

#ifndef BRIDGE_DB_H
#define BRIDGE_DB_H

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    uint32_t rx_freq_hz;
    uint32_t tx_freq_hz;
    uint8_t  lna_gain_db;
    uint8_t  pga_gain_db;
    float    mixer_gain_db;
    int8_t   dac_gain_db;
    uint16_t rx_pll_bw_khz;
    uint16_t tx_pll_bw_khz;
    bool     rx_invert;
    bool     tx_invert;
} SxSettings;

/* Creates every setting that is missing, with the value in d; saved values stay. */
void sxDbDefaults(const char* modemName, const SxSettings* d);

/* Reads the settings; one that is missing or unreadable keeps the value in d. */
void sxDbLoad(const char* modemName, SxSettings* d);

/* Writes streamType (SAMPLE) and bridgePort (the link name below /dev). */
void sxDbPublish(const char* modemName, const char* linkPath);

/* Answers a dashboard command this modem cannot do. */
void sxDbAckUnsupported(const char* modemName);

/* True if the two sets differ. */
bool sxDbDiffer(const SxSettings* a, const SxSettings* b);

#endif
