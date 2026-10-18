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

#ifndef NXDN_GEN_H
#define NXDN_GEN_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Test frame generator.  Builds the frames of a synthetic voice call:
 * a header (VCALL in both FACCH1 slots), 'voiceFrames' voice frames with the
 * call information in the SACCH superblock and test audio, then a trailer
 * (TX_REL).  Frames are returned as they go on the air (scrambled).
 * Returns the number of frames written (voiceFrames + 2), or 0 if max is too
 * small.
 */
unsigned int nxdnGenVoiceCall(uint8_t ran, uint16_t src, uint16_t dst, bool group, unsigned int voiceFrames,
                              uint8_t frames[][48], unsigned int max, uint32_t seed);

/* A data call: header, 'blocks' data frames, trailer */
unsigned int nxdnGenDataCall(uint8_t ran, uint16_t src, uint16_t dst, bool group, unsigned int blocks,
                             uint8_t frames[][48], unsigned int max, uint32_t seed);

#ifdef __cplusplus
}
#endif

#endif
