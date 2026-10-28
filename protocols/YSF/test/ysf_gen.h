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

#ifndef YSF_GEN_H
#define YSF_GEN_H

#include <stdbool.h>
#include <stdint.h>

#include "ysf_func.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Test frame generator.  Frames are the 120 bytes that go on the air.
 *
 * A voice call is a header, 'frames' V/D mode 2 frames (the callsigns travel in
 * frames 0 and 1; the voice channels hold random bits, each repeated three times
 * as the mode requires) and a terminator.  A data call is a header, 'frames'
 * data FR mode frames with valid channels, and a terminator.  'dgid' is the
 * DG-ID in the FICH.  Returns the number of frames written, or 0 if max is too
 * small.
 */
unsigned int ysfGenVoiceCall(const char* dest, const char* src, uint8_t dgid, unsigned int frames,
                             uint8_t out[][YSF_FRAME_LENGTH_BYTES], unsigned int max);

/*
 * As ysfGenVoiceCall(), but the voice comes from 'ambe': frames * 5 blocks of 49 bits, each in 7 bytes
 * (MSB first).  The first 27 bits of a block are sent three times, the other 22 once.
 */
unsigned int ysfGenVoiceCallAudio(const char* dest, const char* src, uint8_t dgid, unsigned int frames, const uint8_t* ambe,
                                  uint8_t out[][YSF_FRAME_LENGTH_BYTES], unsigned int max);

unsigned int ysfGenDataCall(const char* dest, const char* src, uint8_t dgid, unsigned int frames,
                            uint8_t out[][YSF_FRAME_LENGTH_BYTES], unsigned int max);

#ifdef __cplusplus
}
#endif

#endif
