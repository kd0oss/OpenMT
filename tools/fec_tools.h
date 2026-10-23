/***************************************************************************
 *   Copyright (C) 2025 by Rick KD0OSS             *
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
 *                                                                         *
 ***************************************************************************/

/*
 * fec_tools.h
 *
 * Error-correction and checksum coders shared by every protocol: Reed-Solomon,
 * BCH, Golay, Hamming, QR and CRC, plus the bit and symbol packing helpers
 * they use.  This code has no database or network dependencies, so the
 * simulators and tests can link it on its own.
 */

#ifndef FEC_TOOLS_H
#define FEC_TOOLS_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <ctype.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool RS129_check(const unsigned char* in);
void RS129_encode(const unsigned char* msg, unsigned int nbytes, unsigned char* parity);

bool CRC_checkFiveBit(bool* in, unsigned int tcrc);
void CRC_encodeFiveBit(const bool* in, unsigned int* tcrc);

void CRC_addCCITT161(unsigned char* in, unsigned int length);
void CRC_addCCITT162(unsigned char* in, unsigned int length);

bool CRC_checkCCITT161(const unsigned char* in, unsigned int length);
bool CRC_checkCCITT162(const unsigned char* in, unsigned int length);

unsigned char CRC_crc8(const unsigned char* in, unsigned int length);

bool rs241213_decode(uint8_t* data);
void rs241213_encode(uint8_t* data);

void hamming_encode15113_1(bool* d);
bool hamming_decode15113_1(bool* d);

void hamming_encode15113_2(bool* d);
bool hamming_decode15113_2(bool* d);

void hamming_encode1393(bool* d);
bool hamming_decode1393(bool* d);

void hamming_encode1063(bool* d);
bool hamming_decode1063(bool* d);

void hamming_encode16114(bool* d);
bool hamming_decode16114(bool* d);

void hamming_encode17123(bool* d);
bool hamming_decode17123(bool* d);

unsigned int golay24128_encode23127(unsigned int data);
unsigned int golay24128_encode24128(unsigned int data);

unsigned int golay24128_decode23127(unsigned int code);
unsigned int golay24128_correct23127(unsigned int code);
unsigned int get_syndrome_23127(unsigned int pattern);
unsigned int golay24128_decode24128(uint8_t* bytes);

unsigned int golay24128_errors23127(unsigned int word, unsigned int* data);
unsigned int golay24128_errors24128(unsigned int word, unsigned int* data);

extern const unsigned int AMBE_PRNG_TABLE[4096U];

void bch_encode1(uint8_t* data);
void bch_encode2(const int* data, int* bb);

bool decode362017(uint8_t* data);
bool decode24169(uint8_t* data);
void encode362017(uint8_t* data);
void encode24169(uint8_t* data);

void byteToBitsBE(unsigned char byte, bool* bits);
void byteToBitsLE(unsigned char byte, bool* bits);

void bitsToByteBE(const bool* bits, unsigned char* byte);
void bitsToByteLE(const bool* bits, unsigned char* byte);

unsigned int countBits(unsigned int v);

void removeChar(unsigned char * haystack, char needdle);

void QR1676_encode(unsigned char* data);
unsigned char QR1676_decode(const unsigned char* data);

/* Trellis coders (DMR rate 3/4; P25 rate 3/4 and 1/2) */
bool DMRTrellis_decode(const uint8_t* data, uint8_t* payload);
void DMRTrellis_encode(const uint8_t* payload, uint8_t* data);
bool P25Trellis_decode34(const uint8_t* data, uint8_t* payload);
void P25Trellis_encode34(const uint8_t* payload, uint8_t* data);
bool P25Trellis_decode12(const uint8_t* data, uint8_t* payload);
void P25Trellis_encode12(const uint8_t* payload, uint8_t* data);

#ifdef __cplusplus
}
#endif

#endif
