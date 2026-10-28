/*
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#ifndef AMBE3000_H
#define AMBE3000_H

#include <stdbool.h>
#include <stdint.h>

/*
 * DVSI AMBE-3000R vocoder on a serial port (USB dongles such as the ThumbDV
 * and DVstick 30).  The chip is run at its AMBE+2 3600 bit/s rate with FEC (the
 * DMR rate, the one the NXDN voice coding uses): one 20 ms frame is 160 samples
 * of 8 kHz speech and a 72 bit block.  The block is in coding order; nxdn_func
 * has nxdnAudioChipToAir() / nxdnAudioAirToChip() to spread it for the air.
 */

#define AMBE_SAMPLES        160U
#define AMBE_BITS           72U
#define AMBE_BLOCK_BYTES    9U      /* 72 bits */

extern int ambeChunk;      /* bytes per write, 0 = whole packet */
extern int ambeGapUs;      /* pause between chunks */
extern int ambeFlow;       /* 1: RTS/CTS */
extern int ambeHwReset;    /* 1 DTR, 2 RTS, 3 both: pulse low when opening */
extern int ambeInit;       /* PKT_INIT flags after the rate, 0 to skip */
extern int ambeDebug;      /* non-zero: print every packet to stderr */

typedef struct
{
    int      fd;
    uint16_t rate[6];       /* PKT_RATEP words, used when rateIndex < 0 */
    int      rateIndex;     /* PKT_RATET index, or -1 to use the words */
    char     product[32];
} Ambe;

/* The rate words for AMBE+2 2450 + 1150 bit/s (3600, FEC on, 72 bit channel frame): 0431 0754 2400 0000 0000 6F48 */
void ambeDefaultRate(Ambe* a);

/* Opens the port, resets the chip and selects the rate. 'baud' is 230400 or 460800 for most dongles. */
bool ambeOpen(Ambe* a, const char* device, unsigned int baud);
void ambeClose(Ambe* a);

/* 160 samples in, 9 bytes out */
bool ambeEncode(Ambe* a, const int16_t* pcm, uint8_t* block);
/* 9 bytes in, 160 samples out */
bool ambeDecode(Ambe* a, const uint8_t* block, int16_t* pcm);

#endif
