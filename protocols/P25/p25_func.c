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
#include <dsp_tools.h>

uint8_t p25duid;
uint8_t p25lsd1;
uint8_t p25lsd2;
uint8_t p25hdr[P25_NID_LENGTH_BYTES]    = {0};
uint8_t p25ldu1[P25_NID_LENGTH_BYTES]   = {0};
uint8_t p25ldu2[P25_NID_LENGTH_BYTES]   = {0};
uint8_t p25termlc[P25_NID_LENGTH_BYTES] = {0};
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

void addSync(uint8_t* data)
{
    assert(data != NULL);

    memcpy(data, P25_SYNC_BYTES, P25_SYNC_LENGTH_BYTES);
}

void setBusyBits(uint8_t* data, unsigned int ssOffset, bool b1, bool b2)
{
    assert(data != NULL);

    WRITE_BIT(data, ssOffset, b1);
    WRITE_BIT(data, ssOffset + 1U, b2);
}

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

uint8_t lowSpeedData_encode(uint8_t in)
{
    return CCS_PARITY[in];
}

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

uint8_t lowSpeedData_getLSD1()
{
    return p25lsd1;
}

void lowSpeedData_setLSD1(uint8_t lsd1)
{
    p25lsd1 = lsd1;
}

uint8_t lowSpeedData_getLSD2()
{
    return p25lsd2;
}

void lowSpeedData_setLSD2(uint8_t lsd2)
{
    p25lsd2 = lsd2;
}

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
}

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

    return false;
}

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
        default:
            break;
    }
}

uint8_t getDUID()
{
    return p25duid;
}

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

unsigned int getNAC()
{
    return p25nac;
}

void getMI(uint8_t* mi)
{
    assert(mi != NULL);

    memcpy(mi, p25mi, P25_MI_LENGTH_BYTES);
}

uint8_t getMFId()
{
    return p25mfId;
}

uint8_t getAlgId()
{
    return p25algId;
}

unsigned int getKId()
{
    return p25kId;
}

unsigned int getSrcId()
{
    return p25srcId;
}

void setMI(const uint8_t* mi)
{
    assert(mi != NULL);

    memcpy(p25mi, mi, P25_MI_LENGTH_BYTES);
}

void setMFId(uint8_t id)
{
    p25mfId = id;
}

void setAlgId(uint8_t id)
{
    p25algId = id;
}

void setKId(unsigned int id)
{
    p25kId = id;
}

void setSrcId(unsigned int id)
{
    p25srcId = id;
}

void setEmergency(bool on)
{
    p25emergency = on;
}

void setLCF(uint8_t lcf)
{
    p25lcf = lcf;
}

void setDstId(unsigned int id)
{
    p25dstId = id;
}

bool getEmergency()
{
    return p25emergency;
}

uint8_t getLCF()
{
    return p25lcf;
}

unsigned int getDstId()
{
    return p25dstId;
}

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

    /*  Simple validation of the source id */
    unsigned int srcId = (rs[6U] << 16) + (rs[7U] << 8) + rs[8U];
    if (srcId < 1000000U) return false;

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

    return true;
}

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

void encodeLDU2(uint8_t* data)
{
    assert(data != NULL);
    assert(p25mi != NULL);

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
