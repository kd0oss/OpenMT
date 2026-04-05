/*
 *	Copyright (C) 2009,2011 by Jonathan Naylor, G4KLX
 *
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License as published by
 *	the Free Software Foundation; version 2 of the License.
 *
 *	This program is distributed in the hope that it will be useful,
 *	but WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *	GNU General Public License for more details.
 */

#ifndef CCITT_CHECKSUM_REVERSE_H
#define CCITT_CHECKSUM_REVERSE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* CCITT Checksum (CRC-16-CCITT) structure */
typedef struct
{
    union
    {
        unsigned int  crc16;
        unsigned char crc8[2];
    };
} CCITTChecksumReverse;

/* Utility functions for bit/byte conversion */
unsigned char bitsToByte(const bool* bits);
unsigned char bitsToByteRev(const bool* bits);
void byteToBits(unsigned char byte, bool* data);
void byteToBitsRev(unsigned char byte, bool* data);

/* CCITT Checksum functions */
void ccitt_checksum_init(CCITTChecksumReverse *checksum);
void ccitt_checksum_update_bytes(CCITTChecksumReverse *checksum, const unsigned char* data, unsigned int length);
void ccitt_checksum_update_bits(CCITTChecksumReverse *checksum, const bool* data);
void ccitt_checksum_result_bytes(CCITTChecksumReverse *checksum, unsigned char* data);
void ccitt_checksum_result_bits(CCITTChecksumReverse *checksum, bool* data);
bool ccitt_checksum_check_bytes(CCITTChecksumReverse *checksum, const unsigned char* data);
bool ccitt_checksum_check_bits(CCITTChecksumReverse *checksum, const bool* data);
void ccitt_checksum_reset(CCITTChecksumReverse *checksum);

#ifdef __cplusplus
}
#endif

#endif /* CCITT_CHECKSUM_REVERSE_H */
