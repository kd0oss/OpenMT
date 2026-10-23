/***************************************************************************
 *   Copyright (C) 2025 by Rick KD0OSS                                     *
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
 *   along with this program; if not, see <http:/* www.gnu.org/licenses/>  *
 *                                                                         *
 *   Much of this code is from, based on or inspired by MMDVM created by   *
 *   Jonathan Naylor G4KLX                                                 *
 ***************************************************************************/

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <assert.h>

#include "p25_func.h"
#include <fec_tools.h>
#include "p25_fec.h"

uint8_t p25duid;
uint8_t p25lsd1;
uint8_t p25lsd2;
uint8_t p25hdr[P25_NID_LENGTH_BYTES]    = {0};
uint8_t p25ldu1[P25_NID_LENGTH_BYTES]   = {0};
uint8_t p25ldu2[P25_NID_LENGTH_BYTES]   = {0};
uint8_t p25termlc[P25_NID_LENGTH_BYTES] = {0};
uint8_t p25tsdu[P25_NID_LENGTH_BYTES]   = {0};
uint8_t p25pdu[P25_NID_LENGTH_BYTES]    = {0};
uint8_t p25term[P25_NID_LENGTH_BYTES]   = {0};
uint8_t p25mi[P25_MI_LENGTH_BYTES]      = {0};
uint8_t p25mfId;
uint8_t p25algId;
uint8_t p25lcf;
uint8_t p25lastDUID;
bool    p25emergency;
unsigned int p25kId;
unsigned int p25srcId;
unsigned int p25dstId;
unsigned int p25nac;

/*
 * p25Reset()
 *
 * Resets the stored P25 link control to its defaults: no message indicator,
 * unencrypted, key id 0, group call, manufacturer id 0, source and
 * destination ids 0, no emergency.
 *
 * Parameters: none.
 *
 * Returns: nothing.
 */
void p25Reset()
{
    memset(p25mi, 0x00U, P25_MI_LENGTH_BYTES);

    p25algId     = P25_ALGO_UNENCRYPT;
    p25kId       = 0x0000U;
    p25lcf       = P25_LCF_GROUP;
    p25mfId      = 0x00U;
    p25srcId     = 0U;
    p25dstId     = 0U;
    p25emergency = false;
}

/*
 * addSync()
 *
 * Writes the P25 frame sync pattern at the start of a frame.
 *
 * Parameters:
 *   data - The frame buffer; the sync is written at its start.
 *
 * Returns: nothing.
 */
void addSync(uint8_t* data)
{
    assert(data != NULL);

    memcpy(data, P25_SYNC_BYTES, P25_SYNC_LENGTH_BYTES);
}

/*
 * setBusyBits()
 *
 * Sets the two status symbol (busy) bits at one position of a frame.
 *
 * Parameters:
 *   data     - The frame buffer.
 *   ssOffset - Bit position of the first status symbol bit.
 *   b1       - Value of the first bit.
 *   b2       - Value of the second bit.
 *
 * Returns: nothing.
 */
void setBusyBits(uint8_t* data, unsigned int ssOffset, bool b1, bool b2)
{
    assert(data != NULL);

    WRITE_BIT(data, ssOffset, b1);
    WRITE_BIT(data, ssOffset + 1U, b2);
}

/*
 * addBusyBits()
 *
 * Sets the status symbol (busy) bits at every status symbol position of a
 * frame.
 *
 * Parameters:
 *   data   - The frame buffer.
 *   length - Length of the frame in bits.
 *   b1     - Value for the first bit of each status symbol.
 *   b2     - Value for the second bit of each status symbol.
 *
 * Returns: nothing.
 */
void addBusyBits(uint8_t* data, unsigned int length, bool b1, bool b2)
{
    assert(data != NULL);

    for (unsigned int ss0Pos = P25_SS0_START; ss0Pos < length;
         ss0Pos += P25_SS_INCREMENT)
    {
        unsigned int ss1Pos = ss0Pos + 1U;
        WRITE_BIT(data, ss0Pos, b1);
        WRITE_BIT(data, ss1Pos, b2);
    }
}

/*
 * decode()
 *
 * Copies a range of bits out of a frame, leaving out the status symbol bits
 * that P25 inserts at regular positions.
 *
 * Parameters:
 *   in    - The frame.
 *   out   - Receives the bits without the status symbols.
 *   start - Bit position where the range starts.
 *   stop  - Bit position where the range ends (not included).
 *
 * Returns: nothing.
 */
void decode(const uint8_t* in, uint8_t* out, unsigned int start, unsigned int stop)
{
    assert(in != NULL);
    assert(out != NULL);

    /*  Move the SSx positions to the range needed */
    unsigned int ss0Pos = P25_SS0_START;
    unsigned int ss1Pos = P25_SS1_START;

    while (ss0Pos < start)
    {
        ss0Pos += P25_SS_INCREMENT;
        ss1Pos += P25_SS_INCREMENT;
    }

    unsigned int n = 0U;
    for (unsigned int i = start; i < stop; i++)
    {
        if (i == ss0Pos)
        {
            ss0Pos += P25_SS_INCREMENT;
        }
        else if (i == ss1Pos)
        {
            ss1Pos += P25_SS_INCREMENT;
        }
        else
        {
            bool b = READ_BIT(in, i);
            WRITE_BIT(out, n, b);
            n++;
        }
    }
}

/*
 * encode()
 *
 * The reverse of decode(): copies bits into a range of a frame, skipping the
 * positions of the status symbol bits.
 *
 * Parameters:
 *   in    - The bits to place in the frame.
 *   out   - The frame, changed in place.
 *   start - Bit position where the range starts.
 *   stop  - Bit position where the range ends (not included).
 *
 * Returns: nothing.
 */
void encode(const uint8_t* in, uint8_t* out, unsigned int start, unsigned int stop)
{
    assert(in != NULL);
    assert(out != NULL);

    /*  Move the SSx positions to the range needed */
    unsigned int ss0Pos = P25_SS0_START;
    unsigned int ss1Pos = P25_SS1_START;

    while (ss0Pos < start)
    {
        ss0Pos += P25_SS_INCREMENT;
        ss1Pos += P25_SS_INCREMENT;
    }

    unsigned int n = 0U;
    for (unsigned int i = start; i < stop; i++)
    {
        if (i == ss0Pos)
        {
            ss0Pos += P25_SS_INCREMENT;
        }
        else if (i == ss1Pos)
        {
            ss1Pos += P25_SS_INCREMENT;
        }
        else
        {
            bool b = READ_BIT(in, n);
            WRITE_BIT(out, i, b);
            n++;
        }
    }
}

/*
 * compare()
 *
 * Counts the bits that differ between two blocks of data.
 *
 * Parameters:
 *   data1  - First block.
 *   data2  - Second block.
 *   length - Number of bytes to compare.
 *
 * Returns: Number of differing bits.
 */
unsigned int compare(const uint8_t* data1, const uint8_t* data2, unsigned int length)
{
    assert(data1 != NULL);
    assert(data2 != NULL);

    unsigned int errs = 0U;
    for (unsigned int i = 0U; i < length; i++)
    {
        uint8_t v = data1[i] ^ data2[i];
        while (v != 0U)
        {
            v &= v - 1U;
            errs++;
        }
    }

    return errs;
}

/*
 * lowSpeedData_encode()
 *
 * Gets the check byte for a low speed data byte, from the CCS parity table.
 *
 * Parameters:
 *   in - The low speed data byte.
 *
 * Returns: The check byte.
 */
uint8_t lowSpeedData_encode(uint8_t in)
{
    return CCS_PARITY[in];
}

/*
 * lowSpeedData_process()
 *
 * Reads the low speed data of an LDU, corrects it and writes it back.  The
 * two data bytes are decoded from the frame, each with its check byte is
 * matched against all possible values to correct small errors, the data is
 * stored as LSD1 and LSD2, and the corrected bytes are put back in the
 * frame.
 *
 * Parameters:
 *   data - The LDU frame, changed in place.
 *
 * Returns: nothing.
 */
void lowSpeedData_process(uint8_t* data)
{
    assert(data != NULL);

    uint8_t lsd[4U];
    decode(data, lsd, 1546U, 1578U);

    for (unsigned int a = 0x00U; a < 0x100U; a++)
    {
        uint8_t ccs[2U];
        ccs[0U] = a;
        ccs[1U] = lowSpeedData_encode(ccs[0U]);

        unsigned int errs = compare(ccs, lsd + 0U, 2U);
        if (errs < MAX_CCS_ERRS)
        {
            lsd[0U] = ccs[0U];
            lsd[1U] = ccs[1U];
            break;
        }
    }

    for (unsigned int a = 0x00U; a < 0x100U; a++)
    {
        uint8_t ccs[2U];
        ccs[0U] = a;
        ccs[1U] = lowSpeedData_encode(ccs[0U]);

        unsigned int errs = compare(ccs, lsd + 2U, 2U);
        if (errs < MAX_CCS_ERRS)
        {
            lsd[2U] = ccs[0U];
            lsd[3U] = ccs[1U];
            break;
        }
    }

    p25lsd1 = lsd[0U];
    p25lsd2 = lsd[2U];

    encode(lsd, data, 1546U, 1578U);
}

/*
 * lowSpeedData_encode1()
 *
 * Writes the stored LSD1 and LSD2 bytes, with their check bytes, into the
 * low speed data position of an LDU.
 *
 * Parameters:
 *   data - The LDU frame, changed in place.
 *
 * Returns: nothing.
 */
void lowSpeedData_encode1(uint8_t* data)
{
    assert(data != NULL);

    uint8_t lsd[4U];
    lsd[0U] = p25lsd1;
    lsd[1U] = lowSpeedData_encode(p25lsd1);
    lsd[2U] = p25lsd2;
    lsd[3U] = lowSpeedData_encode(p25lsd2);

    encode(lsd, data, 1546U, 1578U);
}

/*
 * lowSpeedData_getLSD1()
 *
 * Gets the first low speed data byte.
 *
 * Parameters: none.
 *
 * Returns: The LSD1 byte.
 */
uint8_t lowSpeedData_getLSD1()
{
    return p25lsd1;
}

/*
 * lowSpeedData_setLSD1()
 *
 * Sets the first low speed data byte.
 *
 * Parameters:
 *   lsd1 - New LSD1 byte.
 *
 * Returns: nothing.
 */
void lowSpeedData_setLSD1(uint8_t lsd1)
{
    p25lsd1 = lsd1;
}

/*
 * lowSpeedData_getLSD2()
 *
 * Gets the second low speed data byte.
 *
 * Parameters: none.
 *
 * Returns: The LSD2 byte.
 */
uint8_t lowSpeedData_getLSD2()
{
    return p25lsd2;
}

/*
 * lowSpeedData_setLSD2()
 *
 * Sets the second low speed data byte.
 *
 * Parameters:
 *   lsd2 - New LSD2 byte.
 *
 * Returns: nothing.
 */
void lowSpeedData_setLSD2(uint8_t lsd2)
{
    p25lsd2 = lsd2;
}

/*
 * resetNID()
 *
 * Builds the network ids (NIDs) for a network access code: one each for the
 * header, LDU1, LDU2, terminator with link control, terminator, TSDU and
 * PDU, each with
 * its BCH check and parity bit.  They are used to recognise and to write the
 * NID of a frame.
 *
 * Parameters:
 *   nac - The network access code.
 *
 * Returns: nothing.
 */
void resetNID(unsigned int nac)
{
    p25nac  = nac;
    p25duid = 0U;

    p25hdr[0U] = (nac >> 4) & 0xFFU;
    p25hdr[1U] = (nac << 4) & 0xF0U;
    p25hdr[1U] |= P25_DUID_HEADER;
    bch_encode1(p25hdr);
    p25hdr[7U] &= 0xFEU;  /*  Clear the parity bit */

    p25ldu1[0U] = (nac >> 4) & 0xFFU;
    p25ldu1[1U] = (nac << 4) & 0xF0U;
    p25ldu1[1U] |= P25_DUID_LDU1;
    bch_encode1(p25ldu1);
    p25ldu1[7U] |= 0x01U;  /*  Set the parity bit */

    p25ldu2[0U] = (nac >> 4) & 0xFFU;
    p25ldu2[1U] = (nac << 4) & 0xF0U;
    p25ldu2[1U] |= P25_DUID_LDU2;
    bch_encode1(p25ldu2);
    p25ldu2[7U] |= 0x01U;  /*  Set the parity bit */

    p25termlc[0U] = (nac >> 4) & 0xFFU;
    p25termlc[1U] = (nac << 4) & 0xF0U;
    p25termlc[1U] |= P25_DUID_TERM_LC;
    bch_encode1(p25termlc);
    p25termlc[7U] &= 0xFEU;  /*  Clear the parity bit */

    p25term[0U] = (nac >> 4) & 0xFFU;
    p25term[1U] = (nac << 4) & 0xF0U;
    p25term[1U] |= P25_DUID_TERM;
    bch_encode1(p25term);
    p25term[7U] &= 0xFEU;  /*  Clear the parity bit */

    p25tsdu[0U] = (nac >> 4) & 0xFFU;
    p25tsdu[1U] = (nac << 4) & 0xF0U;
    p25tsdu[1U] |= P25_DUID_TSDU;
    bch_encode1(p25tsdu);
    p25tsdu[7U] &= 0xFEU;  /*  Clear the parity bit */

    p25pdu[0U] = (nac >> 4) & 0xFFU;
    p25pdu[1U] = (nac << 4) & 0xF0U;
    p25pdu[1U] |= P25_DUID_PDU;
    bch_encode1(p25pdu);
    p25pdu[7U] &= 0xFEU;  /*  Clear the parity bit */
}

/*
 * decodeNID()
 *
 * Decodes the NID of a frame.  The NID is taken from the frame and compared
 * with each of the NIDs made by resetNID(); a match with fewer than
 * MAX_NID_ERRS bit errors sets the data unit id.
 *
 * Parameters:
 *   data - The frame.
 *
 * Returns: true if the NID matched one of the known data units, false if
 *   not.
 */
bool decodeNID(const uint8_t* data)
{
    assert(data != NULL);

    uint8_t nid[P25_NID_LENGTH_BYTES];
    decode(data, nid, 48U, 114U);

    unsigned int errs = compare(nid, p25ldu1, P25_NID_LENGTH_BYTES);
    if (errs < MAX_NID_ERRS)
    {
        p25duid = P25_DUID_LDU1;
        return true;
    }

    errs = compare(nid, p25ldu2, P25_NID_LENGTH_BYTES);
    if (errs < MAX_NID_ERRS)
    {
        p25duid = P25_DUID_LDU2;
        return true;
    }

    errs = compare(nid, p25term, P25_NID_LENGTH_BYTES);
    if (errs < MAX_NID_ERRS)
    {
        p25duid = P25_DUID_TERM;
        return true;
    }

    errs = compare(nid, p25termlc, P25_NID_LENGTH_BYTES);
    if (errs < MAX_NID_ERRS)
    {
        p25duid = P25_DUID_TERM_LC;
        return true;
    }

    errs = compare(nid, p25hdr, P25_NID_LENGTH_BYTES);
    if (errs < MAX_NID_ERRS)
    {
        p25duid = P25_DUID_HEADER;
        return true;
    }

    errs = compare(nid, p25tsdu, P25_NID_LENGTH_BYTES);
    if (errs < MAX_NID_ERRS)
    {
        p25duid = P25_DUID_TSDU;
        return true;
    }

    errs = compare(nid, p25pdu, P25_NID_LENGTH_BYTES);
    if (errs < MAX_NID_ERRS)
    {
        p25duid = P25_DUID_PDU;
        return true;
    }

    return false;
}

/*
 * encodeNID()
 *
 * Writes the NID of a data unit into a frame.  Header, LDU1, LDU2,
 * terminator, terminator with link control, TSDU and PDU are supported; for
 * any other value nothing is written.
 *
 * Parameters:
 *   data - The frame, changed in place.
 *   duid - Data unit id to write.
 *
 * Returns: nothing.
 */
void encodeNID(uint8_t* data, uint8_t duid)
{
    assert(data != NULL);

    switch (duid)
    {
        case P25_DUID_HEADER:
            encode(p25hdr, data, 48U, 114U);
            break;
        case P25_DUID_LDU1:
            encode(p25ldu1, data, 48U, 114U);
            break;
        case P25_DUID_LDU2:
            encode(p25ldu2, data, 48U, 114U);
            break;
        case P25_DUID_TERM:
            encode(p25term, data, 48U, 114U);
            break;
        case P25_DUID_TERM_LC:
            encode(p25termlc, data, 48U, 114U);
            break;
        case P25_DUID_TSDU:
            encode(p25tsdu, data, 48U, 114U);
            break;
        case P25_DUID_PDU:
            encode(p25pdu, data, 48U, 114U);
            break;
        default:
            break;
    }
}

/*
 * getDUID()
 *
 * Gets the data unit id found by the last decodeNID().
 *
 * Parameters: none.
 *
 * Returns: The data unit id.
 */
uint8_t getDUID()
{
    return p25duid;
}

/*
 * decodeLDUHamming()
 *
 * Decodes four Hamming (10,6,3) blocks from the link control data of an LDU
 * into 24 bits.
 *
 * Parameters:
 *   data - The coded bits (40 bits).
 *   raw  - Receives the decoded bits.
 *
 * Returns: nothing.
 */
void decodeLDUHamming(const uint8_t* data, uint8_t* raw)
{
    unsigned int n = 0U;
    unsigned int m = 0U;
    for (unsigned int i = 0U; i < 4U; i++)
    {
        bool hamming[10U];

        for (unsigned int j = 0U; j < 10U; j++)
        {
            hamming[j] = READ_BIT(data, n);
            n++;
        }

        hamming_decode1063(hamming);

        for (unsigned int j = 0U; j < 6U; j++)
        {
            WRITE_BIT(raw, m, hamming[j]);
            m++;
        }
    }
}

/*
 * getNAC()
 *
 * Gets the network access code.
 *
 * Parameters: none.
 *
 * Returns: The NAC.
 */
unsigned int getNAC()
{
    return p25nac;
}

/*
 * getMI()
 *
 * Gets the message indicator.
 *
 * Parameters:
 *   mi - Receives the message indicator bytes.
 *
 * Returns: nothing.
 */
void getMI(uint8_t* mi)
{
    assert(mi != NULL);

    memcpy(mi, p25mi, P25_MI_LENGTH_BYTES);
}

/*
 * getMFId()
 *
 * Gets the manufacturer id.
 *
 * Parameters: none.
 *
 * Returns: The manufacturer id.
 */
uint8_t getMFId()
{
    return p25mfId;
}

/*
 * getAlgId()
 *
 * Gets the encryption algorithm id.
 *
 * Parameters: none.
 *
 * Returns: The algorithm id.
 */
uint8_t getAlgId()
{
    return p25algId;
}

/*
 * getKId()
 *
 * Gets the encryption key id.
 *
 * Parameters: none.
 *
 * Returns: The key id.
 */
unsigned int getKId()
{
    return p25kId;
}

/*
 * getSrcId()
 *
 * Gets the source id.
 *
 * Parameters: none.
 *
 * Returns: The source id.
 */
unsigned int getSrcId()
{
    return p25srcId;
}

/*
 * setMI()
 *
 * Sets the message indicator.
 *
 * Parameters:
 *   mi - The message indicator bytes.
 *
 * Returns: nothing.
 */
void setMI(const uint8_t* mi)
{
    assert(mi != NULL);

    memcpy(p25mi, mi, P25_MI_LENGTH_BYTES);
}

/*
 * setMFId()
 *
 * Sets the manufacturer id.
 *
 * Parameters:
 *   id - New manufacturer id.
 *
 * Returns: nothing.
 */
void setMFId(uint8_t id)
{
    p25mfId = id;
}

/*
 * setAlgId()
 *
 * Sets the encryption algorithm id.
 *
 * Parameters:
 *   id - New algorithm id.
 *
 * Returns: nothing.
 */
void setAlgId(uint8_t id)
{
    p25algId = id;
}

/*
 * setKId()
 *
 * Sets the encryption key id.
 *
 * Parameters:
 *   id - New key id.
 *
 * Returns: nothing.
 */
void setKId(unsigned int id)
{
    p25kId = id;
}

/*
 * setSrcId()
 *
 * Sets the source id.
 *
 * Parameters:
 *   id - New source id.
 *
 * Returns: nothing.
 */
void setSrcId(unsigned int id)
{
    p25srcId = id;
}

/*
 * setEmergency()
 *
 * Sets the emergency flag.
 *
 * Parameters:
 *   on - New value of the flag.
 *
 * Returns: nothing.
 */
void setEmergency(bool on)
{
    p25emergency = on;
}

/*
 * setLCF()
 *
 * Sets the link control format.
 *
 * Parameters:
 *   lcf - New link control format.
 *
 * Returns: nothing.
 */
void setLCF(uint8_t lcf)
{
    p25lcf = lcf;
}

/*
 * setDstId()
 *
 * Sets the destination id.
 *
 * Parameters:
 *   id - New destination id.
 *
 * Returns: nothing.
 */
void setDstId(unsigned int id)
{
    p25dstId = id;
}

/*
 * getEmergency()
 *
 * Gets the emergency flag.
 *
 * Parameters: none.
 *
 * Returns: The emergency flag.
 */
bool getEmergency()
{
    return p25emergency;
}

/*
 * getLCF()
 *
 * Gets the link control format.
 *
 * Parameters: none.
 *
 * Returns: The link control format.
 */
uint8_t getLCF()
{
    return p25lcf;
}

/*
 * getDstId()
 *
 * Gets the destination id.
 *
 * Parameters: none.
 *
 * Returns: The destination id.
 */
unsigned int getDstId()
{
    return p25dstId;
}

/*
 * decodeLDU1()
 *
 * Decodes the link control of an LDU1.  The six Hamming protected blocks are
 * read, the Reed-Solomon (24,12,13) check is made, and the source id is
 * checked not to be zero (the Reed-Solomon check already guards against
 * damage, so any other P25 radio id is accepted).  For a group or private
 * call the
 * destination id, source id, emergency flag, link control format and
 * manufacturer id are stored.
 *
 * Parameters:
 *   data - The LDU1 frame.
 *
 * Returns: true if the link control was decoded and valid, false if the
 *   check failed, the source id was zero or the format is not group or
 *   private.
 */
bool decodeLDU1(const uint8_t* data)
{
    assert(data != NULL);

    uint8_t rs[18U];

    uint8_t raw[5U];
    decode(data, raw, 410U, 452U);
    decodeLDUHamming(raw, rs + 0U);

    decode(data, raw, 600U, 640U);
    decodeLDUHamming(raw, rs + 3U);

    decode(data, raw, 788U, 830U);
    decodeLDUHamming(raw, rs + 6U);

    decode(data, raw, 978U, 1020U);
    decodeLDUHamming(raw, rs + 9U);

    decode(data, raw, 1168U, 1208U);
    decodeLDUHamming(raw, rs + 12U);

    decode(data, raw, 1356U, 1398U);
    decodeLDUHamming(raw, rs + 15U);

    bool ret = rs241213_decode(rs);
    if (!ret) return false;

    /*  Simple validation of the source id: zero is not a radio id */
    unsigned int srcId = (rs[6U] << 16) + (rs[7U] << 8) + rs[8U];
    if (srcId == 0U) return false;

    switch (rs[0U])
    {
        case P25_LCF_GROUP:
            p25emergency = (rs[2U] & 0x80U) == 0x80U;
            p25dstId     = (rs[4U] << 8) + rs[5U];
            p25srcId     = srcId;
            break;

        case P25_LCF_PRIVATE:
            p25emergency = false;
            p25dstId     = (rs[3U] << 16) + (rs[4U] << 8) + rs[5U];
            p25srcId     = srcId;
            break;

        default:
            return false;
    }

    p25lcf  = rs[0U];
    p25mfId = rs[1U];

    return true;
}

/*
 * decodeLDU2()
 *
 * Reads the link control of an LDU2: the six Hamming protected blocks are
 * decoded and the Reed-Solomon (24,16,9) check is made.  If it passes, the
 * message indicator, algorithm id and key id are stored.  Nothing is stored
 * if it fails.
 *
 * Parameters:
 *   data - The LDU2 frame.
 *
 * Returns: true if the link control was decoded and valid, false if the
 *   Reed-Solomon check failed.
 */
bool decodeLDU2(const uint8_t* data)
{
    assert(data != NULL);

    uint8_t rs[18U];

    uint8_t raw[5U];
    decode(data, raw, 410U, 452U);
    decodeLDUHamming(raw, rs + 0U);

    decode(data, raw, 600U, 640U);
    decodeLDUHamming(raw, rs + 3U);

    decode(data, raw, 788U, 830U);
    decodeLDUHamming(raw, rs + 6U);

    decode(data, raw, 978U, 1020U);
    decodeLDUHamming(raw, rs + 9U);

    decode(data, raw, 1168U, 1208U);
    decodeLDUHamming(raw, rs + 12U);

    decode(data, raw, 1356U, 1398U);
    decodeLDUHamming(raw, rs + 15U);

    if (!decode24169(rs))
        return false;

    for (unsigned int i = 0U; i < P25_MI_LENGTH_BYTES; i++)
        p25mi[i] = rs[i];

    p25algId = rs[9U];
    p25kId   = (rs[10U] << 8) | rs[11U];

    return true;
}

/*
 * decodeHeader()
 *
 * Reads the header data unit: the frame is de-interleaved, the 36 shortened
 * Golay (18,6,8) words are decoded and the Reed-Solomon (36,20,17) check is
 * made.  If it passes, the message indicator, manufacturer id, algorithm id,
 * key id and destination id are stored.  Nothing is stored if it fails.
 *
 * Parameters:
 *   data - The header frame.
 *
 * Returns: true if the header was decoded and valid, false if a Golay word
 *   or the Reed-Solomon check failed.
 */
bool decodeHeader(const uint8_t* data)
{
    assert(data != NULL);

    uint8_t raw[81U];
    memset(raw, 0x00U, 81U);
    decode(data, raw, 114U, 780U);

    uint8_t rs[81U];
    memset(rs, 0x00U, 81U);

    /*  shortened Golay (18,6,8) decode: the 18 bits are the low bits of a
     *  (24,12,8) code word whose data has six leading zeros */
    unsigned int n = 0U;
    unsigned int m = 0U;

    for (unsigned int i = 0U; i < 36U; i++)
    {
        unsigned int word = 0U;

        for (unsigned int j = 0U; j < 18U; j++, n++)
            word = (word << 1) | (READ_BIT(raw, n) ? 0x01U : 0x00U);

        unsigned int value = 0U;
        unsigned int errs  = golay24128_errors24128(word, &value);

        if (errs > 3U || (value & 0xFC0U) != 0U)
            return false;

        for (int j = 5; j >= 0; j--, m++)
            WRITE_BIT(rs, m, ((value >> j) & 0x01U) == 0x01U);
    }

    if (!decode362017(rs))
        return false;

    for (unsigned int i = 0U; i < P25_MI_LENGTH_BYTES; i++)
        p25mi[i] = rs[i];

    p25mfId  = rs[9U];
    p25algId = rs[10U];
    p25kId   = (rs[11U] << 8) | rs[12U];
    p25dstId = (rs[13U] << 8) | rs[14U];

    return true;
}

/*
 * encodeLDUHamming()
 *
 * Encodes 24 bits as four Hamming (10,6,3) blocks, for the link control data
 * of an LDU.
 *
 * Parameters:
 *   data - Receives the coded bits (40 bits).
 *   raw  - The 24 bits to encode.
 *
 * Returns: nothing.
 */
void encodeLDUHamming(uint8_t* data, const uint8_t* raw)
{
    unsigned int n = 0U;
    unsigned int m = 0U;
    for (unsigned int i = 0U; i < 4U; i++)
    {
        bool hamming[10U];

        for (unsigned int j = 0U; j < 6U; j++)
        {
            hamming[j] = READ_BIT(raw, m);
            m++;
        }

        hamming_encode1063(hamming);

        for (unsigned int j = 0U; j < 10U; j++)
        {
            WRITE_BIT(data, n, hamming[j]);
            n++;
        }
    }
}

/*
 * encodeLDU1()
 *
 * Writes the link control of an LDU1 from the stored values (format,
 * manufacturer id, emergency flag, destination and source ids) into a frame,
 * with Reed-Solomon (24,12,13) and Hamming protection.  An unknown link
 * control format is reported.
 *
 * Parameters:
 *   data - The LDU1 frame, changed in place.
 *
 * Returns: nothing.
 */
void encodeLDU1(uint8_t* data)
{
    assert(data != NULL);

    uint8_t rs[18U];
    memset(rs, 0x00U, 18U);

    rs[0U] = p25lcf;
    rs[1U] = p25mfId;

    switch (p25lcf)
    {
        case P25_LCF_GROUP:
            rs[2U] = p25emergency ? 0x80U : 0x00U;
            rs[4U] = (p25dstId >> 8) & 0xFFU;
            rs[5U] = (p25dstId >> 0) & 0xFFU;
            rs[6U] = (p25srcId >> 16) & 0xFFU;
            rs[7U] = (p25srcId >> 8) & 0xFFU;
            rs[8U] = (p25srcId >> 0) & 0xFFU;
            break;
        case P25_LCF_PRIVATE:
            rs[3U] = (p25dstId >> 16) & 0xFFU;
            rs[4U] = (p25dstId >> 8) & 0xFFU;
            rs[5U] = (p25dstId >> 0) & 0xFFU;
            rs[6U] = (p25srcId >> 16) & 0xFFU;
            rs[7U] = (p25srcId >> 8) & 0xFFU;
            rs[8U] = (p25srcId >> 0) & 0xFFU;
            break;
        default:
            /*  LogMessage("P25, unknown LCF value in LDU1 - $%02X", m_lcf); */
            fprintf(stderr, "P25, unknown LCF value in LDU1 - $%02X\n", p25lcf);
            break;
    }

    rs241213_encode(rs);

    uint8_t raw[5U];
    encodeLDUHamming(raw, rs + 0U);
    encode(raw, data, 410U, 452U);

    encodeLDUHamming(raw, rs + 3U);
    encode(raw, data, 600U, 640U);

    encodeLDUHamming(raw, rs + 6U);
    encode(raw, data, 788U, 830U);

    encodeLDUHamming(raw, rs + 9U);
    encode(raw, data, 978U, 1020U);

    encodeLDUHamming(raw, rs + 12U);
    encode(raw, data, 1168U, 1208U);

    encodeLDUHamming(raw, rs + 15U);
    encode(raw, data, 1356U, 1398U);
}

/*
 * encodeLDU2()
 *
 * Writes the link control of an LDU2 from the stored message indicator,
 * algorithm id and key id into a frame, with Reed-Solomon (24,16,9) and
 * Hamming protection.
 *
 * Parameters:
 *   data - The LDU2 frame, changed in place.
 *
 * Returns: nothing.
 */
void encodeLDU2(uint8_t* data)
{
    assert(data != NULL);

    uint8_t rs[18U];
    memset(rs, 0x00U, 18U);

    for (unsigned int i = 0; i < P25_MI_LENGTH_BYTES; i++)
        rs[i] = p25mi[i];  /*  Message Indicator */

    rs[9U]  = p25algId;               /*  Algorithm ID */
    rs[10U] = (p25kId >> 8) & 0xFFU;  /*  Key ID MSB */
    rs[11U] = (p25kId >> 0) & 0xFFU;  /*  Key ID LSB */

    /*  encode RS (24,16,9) FEC */
    encode24169(rs);

    /*  encode Hamming (10,6,3) FEC and interleave for LC data */
    uint8_t raw[5U];
    encodeLDUHamming(raw, rs + 0U);
    encode(raw, data, 410U, 452U);

    encodeLDUHamming(raw, rs + 3U);
    encode(raw, data, 600U, 640U);

    encodeLDUHamming(raw, rs + 6U);
    encode(raw, data, 788U, 830U);

    encodeLDUHamming(raw, rs + 9U);
    encode(raw, data, 978U, 1020U);

    encodeLDUHamming(raw, rs + 12U);
    encode(raw, data, 1168U, 1208U);

    encodeLDUHamming(raw, rs + 15U);
    encode(raw, data, 1356U, 1398U);
}

/*
 * decodeTDULC()
 *
 * Decodes the link control of a terminator with link control (TDULC).  The
 * 288 bits after the NID are twelve extended Golay (24,12) words; together
 * they carry the 24 six bit symbols of a Reed-Solomon (24,12,13) code word.
 * Both are corrected.  The link control bytes are returned as they are, so
 * any link control format can be carried on.
 *
 * Parameters:
 *   data - The TDULC frame.
 *   lc   - Receives the 9 corrected link control bytes (72 bits).
 *
 * Returns: true if the Reed-Solomon check passed, false if the link control
 *   could not be corrected.
 */
bool decodeTDULC(const uint8_t* data, uint8_t* lc)
{
    assert(data != NULL);
    assert(lc != NULL);

    uint8_t raw[36U];
    uint8_t rs[18U];

    decode(data, raw, 114U, 410U);

    for (unsigned int k = 0U; k < 6U; k++)
    {
        unsigned int d0 = golay24128_decode24128(raw + 6U * k);
        unsigned int d1 = golay24128_decode24128(raw + 6U * k + 3U);

        rs[3U * k + 0U] = (uint8_t)(d0 >> 4);
        rs[3U * k + 1U] = (uint8_t)(((d0 & 0x0FU) << 4) | (d1 >> 8));
        rs[3U * k + 2U] = (uint8_t)(d1 & 0xFFU);
    }

    if (!rs241213_decode(rs))
        return false;

    memcpy(lc, rs, 9U);
    return true;
}

/*
 * encodeTDULC()
 *
 * Writes the link control of a terminator with link control into a frame:
 * Reed-Solomon (24,12,13) parity is added to the 9 bytes and the 24 six bit
 * symbols are carried in twelve extended Golay (24,12) words.
 *
 * Parameters:
 *   data - The TDULC frame, changed in place (bits 114 to 409, leaving out
 *          the status bits).
 *   lc   - The 9 link control bytes (72 bits).
 *
 * Returns: nothing.
 */
void encodeTDULC(uint8_t* data, const uint8_t* lc)
{
    assert(data != NULL);
    assert(lc != NULL);

    uint8_t rs[18U];
    uint8_t raw[36U];

    memset(rs, 0x00U, sizeof(rs));
    memcpy(rs, lc, 9U);
    rs241213_encode(rs);

    for (unsigned int k = 0U; k < 6U; k++)
    {
        unsigned int d0 = ((unsigned int)rs[3U * k] << 4) | (rs[3U * k + 1U] >> 4);
        unsigned int d1 = ((unsigned int)(rs[3U * k + 1U] & 0x0FU) << 8) | rs[3U * k + 2U];
        unsigned int g0 = golay24128_encode24128(d0);
        unsigned int g1 = golay24128_encode24128(d1);

        raw[6U * k + 0U] = (uint8_t)(g0 >> 16);
        raw[6U * k + 1U] = (uint8_t)(g0 >> 8);
        raw[6U * k + 2U] = (uint8_t)g0;
        raw[6U * k + 3U] = (uint8_t)(g1 >> 16);
        raw[6U * k + 4U] = (uint8_t)(g1 >> 8);
        raw[6U * k + 5U] = (uint8_t)g1;
    }

    encode(raw, data, 114U, 410U);
}

/*
 * encodeHeaderGolay()
 *
 * Encodes 36 six bit words as shortened Golay (18,6,8) blocks for the header
 * data unit.
 *
 * Parameters:
 *   data - Receives the coded bits (648 bits).
 *   raw  - The 216 bits to encode.
 *
 * Returns: nothing.
 */
void encodeHeaderGolay(uint8_t* data, const uint8_t* raw)
{
    /*  shortened Golay (18,6,8) encode */
    unsigned int n = 0U;
    unsigned int m = 0U;

    for (unsigned int i = 0U; i < 36U; i++)
    {
        bool golay[18U];
        for (unsigned int j = 0U; j < 6U; j++)
        {
            golay[j] = READ_BIT(raw, m);
            m++;
        }

        unsigned int c0data = 0U;
        for (unsigned int j = 0U; j < 6U; j++)
            c0data = (c0data << 1) | (golay[j] ? 0x01U : 0x00U);

        unsigned int g0 = golay24128_encode24128(c0data);
        for (int j = 17; j >= 0; j--)
        {
            golay[j] = (g0 & 0x01U) == 0x01U;
            g0 >>= 1;
        }

        for (unsigned int j = 0U; j < 18U; j++)
        {
            WRITE_BIT(data, n, golay[j]);
            n++;
        }
    }
}

/*
 * encodeHeader()
 *
 * Builds the header data unit from the stored message indicator,
 * manufacturer id, algorithm id, key id and destination id, with Reed-
 * Solomon (36,20,17) and Golay (18,6,8) protection, and writes it into the
 * frame.
 *
 * Parameters:
 *   data - The header frame, changed in place.
 *
 * Returns: nothing.
 */
void encodeHeader(uint8_t* data)
{
    assert(data != NULL);
    assert(p25mi != NULL);

    uint8_t rs[81U];
    memset(rs, 0x00U, 81U);

    for (unsigned int i = 0; i < P25_MI_LENGTH_BYTES; i++)
        rs[i] = p25mi[i];  /*  Message Indicator */

    rs[9U]  = p25mfId;                  /*  Mfg Id. */
    rs[10U] = p25algId;                 /*  Algorithm ID */
    rs[11U] = (p25kId >> 8) & 0xFFU;    /*  Key ID MSB */
    rs[12U] = (p25kId >> 0) & 0xFFU;    /*  Key ID LSB */
    rs[13U] = (p25dstId >> 8) & 0xFFU;  /*  Talkgroup Address MSB */
    rs[14U] = (p25dstId >> 0) & 0xFFU;  /*  Talkgroup Address LSB */

    /*  encode RS (36,20,17) FEC */
    encode362017(rs);

    uint8_t raw[81U];
    memset(raw, 0x00U, 81U);

    /*  encode Golay (18,6,8) FEC */
    encodeHeaderGolay(raw, rs);

    /*  interleave */
    encode(raw, data, 114U, 780U);
}

/*
 * audip25encode()
 *
 * Encodes one IMBE voice frame into an LDU: the frame is protected with
 * Golay and Hamming codes, whitened, interleaved and written at the position
 * of voice frame n.
 *
 * Parameters:
 *   data - The LDU frame, changed in place.
 *   imbe - The IMBE voice frame.
 *   n    - Number of the voice frame in the LDU, 0 to 8.
 *
 * Returns: nothing.
 */
void audip25encode(uint8_t* data, const uint8_t* imbe, unsigned int n)
{
    assert(data != NULL);
    assert(imbe != NULL);

    bool bTemp[144U];
    bool* bit = bTemp;

    /*  c0 */
    unsigned int c0 = 0U;
    for (unsigned int i = 0U; i < 12U; i++)
    {
        bool b = READ_BIT(imbe, i);
        c0     = (c0 << 1) | (b ? 0x01U : 0x00U);
    }
    unsigned int g2 = golay24128_encode23127(c0);
    for (int i = 23; i >= 0; i--)
    {
        bit[i] = (g2 & 0x01U) == 0x01U;
        g2 >>= 1;
    }
    bit += 23U;

    /*  c1 */
    unsigned int c1 = 0U;
    for (unsigned int i = 12U; i < 24U; i++)
    {
        bool b = READ_BIT(imbe, i);
        c1     = (c1 << 1) | (b ? 0x01U : 0x00U);
    }
    g2 = golay24128_encode23127(c1);
    for (int i = 23; i >= 0; i--)
    {
        bit[i] = (g2 & 0x01U) == 0x01U;
        g2 >>= 1;
    }
    bit += 23U;

    /*  c2 */
    unsigned int c2 = 0;
    for (unsigned int i = 24U; i < 36U; i++)
    {
        bool b = READ_BIT(imbe, i);
        c2     = (c2 << 1) | (b ? 0x01U : 0x00U);
    }
    g2 = golay24128_encode23127(c2);
    for (int i = 23; i >= 0; i--)
    {
        bit[i] = (g2 & 0x01U) == 0x01U;
        g2 >>= 1;
    }
    bit += 23U;

    /*  c3 */
    unsigned int c3 = 0U;
    for (unsigned int i = 36U; i < 48U; i++)
    {
        bool b = READ_BIT(imbe, i);
        c3     = (c3 << 1) | (b ? 0x01U : 0x00U);
    }
    g2 = golay24128_encode23127(c3);
    for (int i = 23; i >= 0; i--)
    {
        bit[i] = (g2 & 0x01U) == 0x01U;
        g2 >>= 1;
    }
    bit += 23U;

    /*  c4 */
    for (unsigned int i = 0U; i < 11U; i++) bit[i] = READ_BIT(imbe, i + 48U);
    hamming_encode15113_1(bit);
    bit += 15U;

    /*  c5 */
    for (unsigned int i = 0U; i < 11U; i++) bit[i] = READ_BIT(imbe, i + 59U);
    hamming_encode15113_1(bit);
    bit += 15U;

    /*  c6 */
    for (unsigned int i = 0U; i < 11U; i++) bit[i] = READ_BIT(imbe, i + 70U);
    hamming_encode15113_1(bit);
    bit += 15U;

    /*  c7 */
    for (unsigned int i = 0U; i < 7U; i++) bit[i] = READ_BIT(imbe, i + 81U);

    bool prn[114U];

    /*  Create the whitening vector and save it for future use */
    unsigned int p = 16U * c0;
    for (unsigned int i = 0U; i < 114U; i++)
    {
        p      = (173U * p + 13849U) % 65536U;
        prn[i] = p >= 32768U;
    }

    /*  Whiten some bits */
    for (unsigned int i = 0U; i < 114U; i++) bTemp[i + 23U] ^= prn[i];

    uint8_t temp[18U];

    /*  Interleave */
    for (unsigned int i = 0U; i < 144U; i++)
    {
        unsigned int n = IMBE_INTERLEAVE[i];
        WRITE_BIT(temp, n, bTemp[i]);
    }

    switch (n)
    {
        case 0U:
            encode(temp, data, 114U, 262U);
            break;
        case 1U:
            encode(temp, data, 262U, 410U);
            break;
        case 2U:
            encode(temp, data, 452U, 600U);
            break;
        case 3U:
            encode(temp, data, 640U, 788U);
            break;
        case 4U:
            encode(temp, data, 830U, 978U);
            break;
        case 5U:
            encode(temp, data, 1020U, 1168U);
            break;
        case 6U:
            encode(temp, data, 1208U, 1356U);
            break;
        case 7U:
            encode(temp, data, 1398U, 1546U);
            break;
        case 8U:
            encode(temp, data, 1578U, 1726U);
            break;
        default:
            return;
    }
}

/*
 * audip25decode()
 *
 * Decodes one IMBE voice frame from an LDU: the reverse of audip25encode().
 *
 * Parameters:
 *   data - The LDU frame.
 *   imbe - Receives the IMBE voice frame.
 *   n    - Number of the voice frame in the LDU, 0 to 8.
 *
 * Returns: nothing.
 */
void audip25decode(const uint8_t* data, uint8_t* imbe, unsigned int n)
{
    assert(data != NULL);
    assert(imbe != NULL);

    uint8_t temp[18U];

    switch (n)
    {
        case 0U:
            decode(data, temp, 114U, 262U);
            break;
        case 1U:
            decode(data, temp, 262U, 410U);
            break;
        case 2U:
            decode(data, temp, 452U, 600U);
            break;
        case 3U:
            decode(data, temp, 640U, 788U);
            break;
        case 4U:
            decode(data, temp, 830U, 978U);
            break;
        case 5U:
            decode(data, temp, 1020U, 1168U);
            break;
        case 6U:
            decode(data, temp, 1208U, 1356U);
            break;
        case 7U:
            decode(data, temp, 1398U, 1546U);
            break;
        case 8U:
            decode(data, temp, 1578U, 1726U);
            break;
        default:
            return;
    }

    bool bit[144U];

    /*  De-interleave */
    for (unsigned int i = 0U; i < 144U; i++)
    {
        unsigned int n = IMBE_INTERLEAVE[i];
        bit[i]         = READ_BIT(temp, n);
    }

    /*  now .. */

    /*  12 voice bits     0 */
    /*  11 golay bits     12 */
    /*  */
    /*  12 voice bits     23 */
    /*  11 golay bits     35 */
    /*  */
    /*  12 voice bits     46 */
    /*  11 golay bits     58 */
    /*  */
    /*  12 voice bits     69 */
    /*  11 golay bits     81 */
    /*  */
    /*  11 voice bits     92 */
    /*   4 hamming bits   103 */
    /*  */
    /*  11 voice bits     107 */
    /*   4 hamming bits   118 */
    /*  */
    /*  11 voice bits     122 */
    /*   4 hamming bits   133 */
    /*  */
    /*   7 voice bits     137 */

    /*  c0 */
    unsigned int c0data = 0U;
    for (unsigned int i = 0U; i < 12U; i++)
        c0data = (c0data << 1) | (bit[i] ? 0x01U : 0x00U);

    bool prn[114U];

    /*  Create the whitening vector and save it for future use */
    unsigned int p = 16U * c0data;
    for (unsigned int i = 0U; i < 114U; i++)
    {
        p      = (173U * p + 13849U) % 65536U;
        prn[i] = p >= 32768U;
    }

    /*  De-whiten some bits */
    for (unsigned int i = 0U; i < 114U; i++) bit[i + 23U] ^= prn[i];

    unsigned int offset = 0U;
    for (unsigned int i = 0U; i < 12U; i++, offset++)
        WRITE_BIT(imbe, offset, bit[i + 0U]);
    for (unsigned int i = 0U; i < 12U; i++, offset++)
        WRITE_BIT(imbe, offset, bit[i + 23U]);
    for (unsigned int i = 0U; i < 12U; i++, offset++)
        WRITE_BIT(imbe, offset, bit[i + 46U]);
    for (unsigned int i = 0U; i < 12U; i++, offset++)
        WRITE_BIT(imbe, offset, bit[i + 69U]);
    for (unsigned int i = 0U; i < 11U; i++, offset++)
        WRITE_BIT(imbe, offset, bit[i + 92U]);
    for (unsigned int i = 0U; i < 11U; i++, offset++)
        WRITE_BIT(imbe, offset, bit[i + 107U]);
    for (unsigned int i = 0U; i < 11U; i++, offset++)
        WRITE_BIT(imbe, offset, bit[i + 122U]);
    for (unsigned int i = 0U; i < 7U; i++, offset++)
        WRITE_BIT(imbe, offset, bit[i + 137U]);
}

/*
 * p25RegenerateLDU()
 *
 * Checks the nine IMBE voice frames of an LDU.  Each frame is taken out of
 * the LDU, de-interleaved and checked by the FEC.  When rewrite is true,
 * every frame with at most P25_FEC_MAX_ERRORS errors is replaced by its
 * corrected, re-encoded version; the others are left as received.
 *
 * Parameters:
 *   data    - The LDU as received, without the 8 byte packet prefix.
 *   rewrite - true to write the corrected frames back.
 *
 * Returns: The number of bit errors found by the FEC in the 1233 protected
 *   bits.
 */
unsigned int p25RegenerateLDU(uint8_t* data, bool rewrite)
{
    static const unsigned int start[9U] = {114U, 262U, 452U, 640U, 830U, 1020U, 1208U, 1398U, 1578U};

    assert(data != NULL);

    unsigned int errors = 0U;

    for (unsigned int n = 0U; n < 9U; n++)
    {
        uint8_t temp[18U];
        bool    bit[144U];

        decode(data, temp, start[n], start[n] + 148U);

        /*  De-interleave */
        for (unsigned int i = 0U; i < 144U; i++)
        {
            bit[i] = READ_BIT(temp, IMBE_INTERLEAVE[i]) != 0U;
        }

        unsigned int e = p25FecFrame(bit, rewrite);
        errors += e;

        if (rewrite && P25_FEC_REWRITE_OK(e))
        {
            /*  Interleave */
            for (unsigned int i = 0U; i < 144U; i++)
            {
                WRITE_BIT(temp, IMBE_INTERLEAVE[i], bit[i]);
            }

            encode(temp, data, start[n], start[n] + 148U);
        }
    }

    return errors;
}

/*
 * rebuildTrellis12()
 *
 * Decodes the first rate 1/2 trellis block of a frame (the TSBK of a TSDU,
 * or the header of a PDU), checks its CRC-CCITT and, if it is good, encodes
 * it again in place, as MMDVMHost does before it relays the frame.  The
 * 12 bytes are not interpreted, so any trunking signalling or packet data
 * header is carried on whatever its format.  A block that does not decode,
 * or whose CRC fails, is left as it was.
 *
 * Parameters:
 *   data - The frame (sync, NID and the block), changed in place.
 *
 * Returns: true if the block was valid and was encoded again.
 */
bool rebuildTrellis12(uint8_t* data)
{
    assert(data != NULL);

    uint8_t raw[25U];
    uint8_t block[12U];

    memset(raw, 0x00U, sizeof(raw));
    decode(data, raw, 114U, 318U);

    if (!P25Trellis_decode12(raw, block))
        return false;

    if (!CRC_checkCCITT162(block, 12U))
        return false;

    memset(raw, 0x00U, sizeof(raw));
    P25Trellis_encode12(block, raw);
    encode(raw, data, 114U, 318U);

    return true;
}
