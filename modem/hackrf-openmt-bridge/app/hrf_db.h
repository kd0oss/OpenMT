/***************************************************************************
 *   Copyright (C) 2026 by Rick KD0OSS                                     *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 3 of the License, or     *
 *   (at your option) any later version.                                   *
 ***************************************************************************/

/*
 * The HackRF bridge's settings in the config database (module "config" of the
 * modem), the same names the SX1255 bridge and the dashboard use.  Kept apart
 * from bridge.c so the database headers stay out of the radio code.
 *
 * Built with HRF_DB=OFF (simulator tests, no MariaDB) every function does
 * nothing and the command line values stay.
 */

#ifndef HRF_DB_H
#define HRF_DB_H

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    uint32_t rx_freq_hz;      /* rxFrequency */
    uint32_t tx_freq_hz;      /* txFrequency */
    uint32_t lna_gain_db;     /* rxLnaGain: HackRF LNA (IF) gain, 0-40 in 8 dB steps (0 is allowed) */
    uint32_t vga_gain_db;     /* rxPgaGain: HackRF RX VGA gain, 0-62 in 2 dB steps (0 is allowed) */
    uint32_t tx_vga_db;       /* txDacGain: HackRF TX VGA gain, 0-47 (a value of 0 or less keeps the current one) */
    bool     rx_invert;       /* rxInvert */
    bool     tx_invert;       /* txInvert */
    uint32_t rx_level;        /* rxLevel (Modem tab): RX gain, 50 is gain x1 like the SAMPLE firmware, 1-255 */
    double   ppm;             /* freqOffsetPpm: correction of the HackRF reference clock, -100 to 100 ppm */
} HrfSettings;

/* Creates every setting that is missing, with the value in d; saved values stay. */
void hrfDbDefaults(const char* modemName, const HrfSettings* d);

/* Reads the settings; one that is missing or unreadable keeps the value in d. */
void hrfDbLoad(const char* modemName, HrfSettings* d);

/* Writes streamType (SAMPLE), the capabilities and bridgePort (the link name below /dev). */
void hrfDbPublish(const char* modemName, const char* linkPath);

/* Answers a dashboard command this modem cannot do. */
void hrfDbAckUnsupported(const char* modemName);

/* True if the two sets differ. */
bool hrfDbDiffer(const HrfSettings* a, const HrfSettings* b);

#endif
