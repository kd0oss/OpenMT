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

#include <stdlib.h>
#include <string.h>

#include <dsp_tools.h>

#include "nxdn_func.h"

static const uint8_t BIT_MASK_TABLE[] = { 0x80U, 0x40U, 0x20U, 0x10U, 0x08U, 0x04U, 0x02U, 0x01U };

#define WRITE_BIT1(p, i, b) (p)[(i) >> 3] = (b) ? ((p)[(i) >> 3] | BIT_MASK_TABLE[(i) & 7]) : ((p)[(i) >> 3] & ~BIT_MASK_TABLE[(i) & 7])
#define READ_BIT1(p, i)     ((p)[(i) >> 3] & BIT_MASK_TABLE[(i) & 7])

static const uint8_t SCRAMBLER[NXDN_FRAME_LENGTH_BYTES] =
{
    0x00U, 0x00U, 0x00U, 0x82U, 0xA0U, 0x88U, 0x8AU, 0x00U, 0xA2U, 0xA8U, 0x82U, 0x8AU, 0x82U, 0x02U,
    0x20U, 0x08U, 0x8AU, 0x20U, 0xAAU, 0xA2U, 0x82U, 0x08U, 0x22U, 0x8AU, 0xAAU, 0x08U, 0x28U, 0x88U,
    0x28U, 0x28U, 0x00U, 0x0AU, 0x02U, 0x82U, 0x20U, 0x28U, 0x82U, 0x2AU, 0xAAU, 0x20U, 0x22U, 0x80U,
    0xA8U, 0x8AU, 0x08U, 0xA0U, 0xAAU, 0x02U
};

static const uint8_t FSW_BYTES[]      = { 0xCDU, 0xF5U, 0x90U };
static const uint8_t FSW_BYTES_MASK[] = { 0xFFU, 0xFFU, 0xF0U };

static const uint8_t SACCH_IDLE[] = { NXDN_MESSAGE_TYPE_IDLE, 0x00U, 0x00U };

/*
 * nxdnScramble()
 *   XORs the whole frame with the NXDN scrambling sequence.  The first three
 *   bytes of the sequence are zero so the frame sync is unaffected.  The same
 *   call scrambles and descrambles.
 */
void nxdnScramble(uint8_t* frame)
{
    for (unsigned int i = 0U; i < NXDN_FRAME_LENGTH_BYTES; i++)
        frame[i] ^= SCRAMBLER[i];
}

/*
 * nxdnAddSync()
 *   Writes the 20 bit frame sync word at the start of a frame.
 */
void nxdnAddSync(uint8_t* frame)
{
    for (unsigned int i = 0U; i < 3U; i++)
        frame[i] = (frame[i] & ~FSW_BYTES_MASK[i]) | FSW_BYTES[i];
}

/*
 * nxdnHasSync()
 *   True if the start of the frame holds the frame sync word.
 */
bool nxdnHasSync(const uint8_t* frame)
{
    for (unsigned int i = 0U; i < 3U; i++)
        if ((frame[i] & FSW_BYTES_MASK[i]) != FSW_BYTES[i])
            return false;

    return true;
}

/*
 * CRC routines.  The bit stream is MSB first, the CRC register starts at all
 * ones and the CRC is sent MSB first right after the data.
 */
uint8_t nxdnCrc6(const uint8_t* in, unsigned int lengthBits)
{
    uint8_t crc = 0x3FU;

    for (unsigned int i = 0U; i < lengthBits; i++)
    {
        bool bit1 = READ_BIT1(in, i) != 0U;
        bool bit2 = (crc & 0x20U) == 0x20U;

        crc <<= 1;

        if (bit1 ^ bit2)
            crc ^= 0x27U;
    }

    return crc & 0x3FU;
}

uint16_t nxdnCrc12(const uint8_t* in, unsigned int lengthBits)
{
    uint16_t crc = 0x0FFFU;

    for (unsigned int i = 0U; i < lengthBits; i++)
    {
        bool bit1 = READ_BIT1(in, i) != 0U;
        bool bit2 = (crc & 0x0800U) == 0x0800U;

        crc <<= 1;

        if (bit1 ^ bit2)
            crc ^= 0x080FU;
    }

    return crc & 0x0FFFU;
}

uint16_t nxdnCrc15(const uint8_t* in, unsigned int lengthBits)
{
    uint16_t crc = 0x7FFFU;

    for (unsigned int i = 0U; i < lengthBits; i++)
    {
        bool bit1 = READ_BIT1(in, i) != 0U;
        bool bit2 = (crc & 0x4000U) == 0x4000U;

        crc <<= 1;

        if (bit1 ^ bit2)
            crc ^= 0x4CC5U;
    }

    return crc & 0x7FFFU;
}

/* Writes the low 'width' bits of value MSB first at bit position 'pos' */
static void putBits(uint8_t* buffer, unsigned int pos, uint16_t value, unsigned int width)
{
    for (unsigned int i = 0U; i < width; i++, pos++)
    {
        bool b = ((value >> (width - 1U - i)) & 1U) != 0U;
        WRITE_BIT1(buffer, pos, b);
    }
}

static uint16_t getBits(const uint8_t* buffer, unsigned int pos, unsigned int width)
{
    uint16_t v = 0U;

    for (unsigned int i = 0U; i < width; i++, pos++)
        v = (v << 1) | (READ_BIT1(buffer, pos) ? 1U : 0U);

    return v;
}

static bool checkCrc6(const uint8_t* in, unsigned int length)
{
    return nxdnCrc6(in, length) == getBits(in, length, 6U);
}

static bool checkCrc12(const uint8_t* in, unsigned int length)
{
    return nxdnCrc12(in, length) == getBits(in, length, 12U);
}

static bool checkCrc15(const uint8_t* in, unsigned int length)
{
    return nxdnCrc15(in, length) == getBits(in, length, 15U);
}

/*
 * Rate 1/2, constraint length 5 convolutional code (g1 = 1+D^2+D^3, g2 =
 * 1+D+D^3 as used by the NXDN channels) with a 16 state Viterbi decoder.
 * The decoder takes soft symbols 0 (sure zero), 1 (punctured) and 2 (sure
 * one).
 */
#define CONV_STATES     16U
#define CONV_STATES_D2  8U
#define CONV_M          4U
#define CONV_K          5U
#define CONV_MAX_STEPS  300U

static const uint8_t BRANCH_TABLE1[] = { 0U, 0U, 0U, 0U, 2U, 2U, 2U, 2U };
static const uint8_t BRANCH_TABLE2[] = { 0U, 2U, 2U, 0U, 0U, 2U, 2U, 0U };

typedef struct
{
    uint16_t metrics1[CONV_STATES];
    uint16_t metrics2[CONV_STATES];
    uint16_t* oldMetrics;
    uint16_t* newMetrics;
    uint64_t decisions[CONV_MAX_STEPS];
    unsigned int dp;
} Conv;

static void convStart(Conv* c)
{
    memset(c->metrics1, 0x00, sizeof(c->metrics1));
    memset(c->metrics2, 0x00, sizeof(c->metrics2));
    c->oldMetrics = c->metrics1;
    c->newMetrics = c->metrics2;
    c->dp = 0U;
}

static void convDecode(Conv* c, uint8_t s0, uint8_t s1)
{
    uint64_t dec = 0U;

    for (uint8_t i = 0U; i < CONV_STATES_D2; i++)
    {
        uint8_t j = i * 2U;
        uint16_t metric = (uint16_t)(abs((int)BRANCH_TABLE1[i] - (int)s0) + abs((int)BRANCH_TABLE2[i] - (int)s1));

        uint16_t m0 = c->oldMetrics[i] + metric;
        uint16_t m1 = c->oldMetrics[i + CONV_STATES_D2] + (CONV_M - metric);
        uint8_t decision0 = (m0 >= m1) ? 1U : 0U;
        c->newMetrics[j + 0U] = decision0 != 0U ? m1 : m0;

        m0 = c->oldMetrics[i] + (CONV_M - metric);
        m1 = c->oldMetrics[i + CONV_STATES_D2] + metric;
        uint8_t decision1 = (m0 >= m1) ? 1U : 0U;
        c->newMetrics[j + 1U] = decision1 != 0U ? m1 : m0;

        dec |= ((uint64_t)decision1 << (j + 1U)) | ((uint64_t)decision0 << (j + 0U));
    }

    if (c->dp < CONV_MAX_STEPS)
        c->decisions[c->dp++] = dec;

    uint16_t* tmp = c->oldMetrics;
    c->oldMetrics = c->newMetrics;
    c->newMetrics = tmp;
}

static void convChainback(Conv* c, uint8_t* out, unsigned int nBits)
{
    uint32_t state = 0U;

    while (nBits-- > 0U)
    {
        --c->dp;

        uint32_t i = state >> (9U - CONV_K);
        uint8_t bit = (uint8_t)(c->decisions[c->dp] >> i) & 1U;
        state = ((uint32_t)bit << 7) | (state >> 1);

        WRITE_BIT1(out, nBits, bit != 0U);
    }
}

static void convEncode(const uint8_t* in, uint8_t* out, unsigned int nBits)
{
    uint8_t d1 = 0U, d2 = 0U, d3 = 0U, d4 = 0U;
    uint32_t k = 0U;

    for (unsigned int i = 0U; i < nBits; i++)
    {
        uint8_t d = READ_BIT1(in, i) ? 1U : 0U;
        uint8_t g1 = (d + d3 + d4) & 1U;
        uint8_t g2 = (d + d1 + d2 + d4) & 1U;

        d4 = d3;
        d3 = d2;
        d2 = d1;
        d1 = d;

        WRITE_BIT1(out, k, g1 != 0U);
        k++;
        WRITE_BIT1(out, k, g2 != 0U);
        k++;
    }
}

/*
 * Channel descriptions.  Every channel is: info + CRC -> convolutional code
 * -> puncture -> interleave.  The interleaver writes a block of columns and
 * reads rows, the puncture positions are periodic.
 */
typedef struct
{
    unsigned int rows;          /* interleaver columns of the written matrix */
    unsigned int stride;        /* interleave stride                          */
    unsigned int codedBits;     /* after the convolutional code               */
    unsigned int sentBits;      /* after puncturing                           */
    unsigned int infoBits;      /* input to the encoder including CRC and tail*/
    unsigned int dataBits;      /* bits covered by the CRC                    */
    unsigned int steps;         /* Viterbi steps                              */
} ChanDef;

static unsigned int interleave(unsigned int chan, unsigned int i)
{
    switch (chan)
    {
        case 0U: return (i % 12U) * 5U + i / 12U;       /* SACCH  */
        case 1U: return (i % 16U) * 9U + i / 16U;       /* FACCH1 */
        default: return (i % 12U) * 29U + i / 12U;      /* UDCH   */
    }
}

static bool punctured(unsigned int chan, unsigned int i)
{
    switch (chan)
    {
        case 0U: return (i % 6U) == 5U;                 /* 5, 11, ..., 71     */
        case 1U: return (i % 4U) == 1U;                 /* 1, 5, ..., 189     */
        default: return (i % 14U) == 3U || (i % 14U) == 11U;  /* 3, 11, 17, ... */
    }
}

/* Reads sentBits from the frame, depunctures and runs the Viterbi decoder */
static void chanDecode(unsigned int chan, const uint8_t* frame, unsigned int offset, unsigned int sentBits,
                       unsigned int steps, uint8_t* out, unsigned int outBits)
{
    uint8_t temp1[48];
    uint8_t temp2[430];
    Conv conv;

    memset(temp1, 0x00, sizeof(temp1));

    for (unsigned int i = 0U; i < sentBits; i++)
    {
        unsigned int n = interleave(chan, i) + offset;
        bool b = READ_BIT1(frame, n) != 0U;
        WRITE_BIT1(temp1, i, b);
    }

    unsigned int n = 0U;

    for (unsigned int i = 0U; i < sentBits; i++)
    {
        while (punctured(chan, n))
            temp2[n++] = 1U;

        bool b = READ_BIT1(temp1, i) != 0U;
        temp2[n++] = b ? 2U : 0U;
    }

    for (unsigned int i = 0U; i < 8U; i++)
        temp2[n++] = 0U;

    convStart(&conv);

    n = 0U;
    for (unsigned int i = 0U; i < steps; i++)
    {
        uint8_t s0 = temp2[n++];
        uint8_t s1 = temp2[n++];

        convDecode(&conv, s0, s1);
    }

    convChainback(&conv, out, outBits);
}

static void chanEncode(unsigned int chan, const uint8_t* info, unsigned int infoBits, unsigned int codedBits,
                       uint8_t* frame, unsigned int offset)
{
    uint8_t temp2[56];
    uint8_t temp3[48];

    memset(temp2, 0x00, sizeof(temp2));
    memset(temp3, 0x00, sizeof(temp3));

    convEncode(info, temp2, infoBits);

    unsigned int n = 0U;

    for (unsigned int i = 0U; i < codedBits; i++)
    {
        if (!punctured(chan, i))
        {
            bool b = READ_BIT1(temp2, i) != 0U;
            WRITE_BIT1(temp3, n, b);
            n++;
        }
    }

    for (unsigned int i = 0U; i < n; i++)
    {
        unsigned int p = interleave(chan, i) + offset;
        bool b = READ_BIT1(temp3, i) != 0U;
        WRITE_BIT1(frame, p, b);
    }
}

/*
 * nxdnLichDecode()
 *   Reads the 8 LICH bits (every other bit of the 16 bit field).  Returns
 *   true if the parity bit is correct.
 */
uint8_t nxdnLichParity(uint8_t lich)
{
    switch (lich & 0xF0U)
    {
        case 0x80U:
        case 0xB0U:
            return 1U;
        default:
            return 0U;
    }
}

bool nxdnLichDecode(const uint8_t* frame, uint8_t* lich)
{
    uint8_t l = 0U;
    unsigned int offset = NXDN_FSW_LENGTH_BITS;

    for (unsigned int i = 0U; i < (NXDN_LICH_LENGTH_BITS / 2U); i++, offset += 2U)
        if (READ_BIT1(frame, offset))
            l |= BIT_MASK_TABLE[i];

    *lich = l;

    return (unsigned int)(l & 0x01U) == nxdnLichParity(l);
}

/*
 * nxdnLichEncode()
 *   Sets the parity bit and writes the LICH, each bit followed by a one.
 */
void nxdnLichEncode(uint8_t lich, uint8_t* frame)
{
    if (nxdnLichParity(lich))
        lich |= 0x01U;
    else
        lich &= 0xFEU;

    unsigned int offset = NXDN_FSW_LENGTH_BITS;

    for (unsigned int i = 0U; i < (NXDN_LICH_LENGTH_BITS / 2U); i++)
    {
        bool b = READ_BIT1(&lich, i) != 0U;
        WRITE_BIT1(frame, offset, b);
        offset++;
        WRITE_BIT1(frame, offset, true);
        offset++;
    }
}

uint8_t nxdnLichMake(uint8_t rfct, uint8_t fct, uint8_t option, uint8_t direction)
{
    uint8_t l = 0U;

    l |= (rfct << 6) & 0xC0U;
    l |= (fct << 4) & 0x30U;
    l |= (option << 2) & 0x0CU;
    l |= (direction << 1) & 0x02U;

    if (nxdnLichParity(l))
        l |= 0x01U;

    return l;
}

/*
 * SACCH.  36 bits (2 structure, 6 RAN, 18 data, 6 CRC... the CRC covers the
 * first 26 bits) plus 4 tail bits, coded to 72 and punctured to 60.
 */
bool nxdnSacchDecode(const uint8_t* frame, uint8_t* sacch)
{
    uint8_t out[8];

    memset(out, 0x00, sizeof(out));

    chanDecode(0U, frame, NXDN_FSW_LENGTH_BITS + NXDN_LICH_LENGTH_BITS, NXDN_SACCH_LENGTH_BITS, 40U, out, 36U);

    memcpy(sacch, out, 5U);

    return checkCrc6(sacch, 26U);
}

void nxdnSacchEncode(const uint8_t* sacch, uint8_t* frame)
{
    uint8_t temp1[5];

    memset(temp1, 0x00, sizeof(temp1));

    for (unsigned int i = 0U; i < 26U; i++)
    {
        bool b = READ_BIT1(sacch, i) != 0U;
        WRITE_BIT1(temp1, i, b);
    }

    putBits(temp1, 26U, nxdnCrc6(temp1, 26U), 6U);

    chanEncode(0U, temp1, 36U, 72U, frame, NXDN_FSW_LENGTH_BITS + NXDN_LICH_LENGTH_BITS);
}

void nxdnSacchGetData(const uint8_t* sacch, uint8_t* data)
{
    unsigned int offset = 8U;

    for (unsigned int i = 0U; i < 18U; i++, offset++)
    {
        bool b = READ_BIT1(sacch, offset) != 0U;
        WRITE_BIT1(data, i, b);
    }
}

void nxdnSacchSetData(uint8_t* sacch, const uint8_t* data)
{
    unsigned int offset = 8U;

    for (unsigned int i = 0U; i < 18U; i++, offset++)
    {
        bool b = READ_BIT1(data, i) != 0U;
        WRITE_BIT1(sacch, offset, b);
    }
}

void nxdnSacchGetRaw(const uint8_t* sacch, uint8_t* raw4)
{
    memcpy(raw4, sacch, 4U);
    putBits(raw4, 26U, nxdnCrc6(raw4, 26U), 6U);
}

void nxdnSacchSetRAN(uint8_t* sacch, uint8_t ran)
{
    sacch[0] &= 0xC0U;
    sacch[0] |= ran;
}

void nxdnSacchSetStructure(uint8_t* sacch, uint8_t structure)
{
    sacch[0] &= 0x3FU;
    sacch[0] |= (structure << 6) & 0xC0U;
}

/*
 * FACCH1.  80 bits of layer 3, CRC12 and 4 tail bits (96), coded to 192 and
 * punctured to 144.
 */
bool nxdnFacch1Decode(const uint8_t* frame, unsigned int offset, uint8_t* facch)
{
    uint8_t out[12];

    memset(out, 0x00, sizeof(out));

    chanDecode(1U, frame, offset, NXDN_FACCH1_LENGTH_BITS, 100U, out, 96U);

    memcpy(facch, out, 12U);

    return checkCrc12(facch, 80U);
}

void nxdnFacch1Encode(const uint8_t* facch10, uint8_t* frame, unsigned int offset)
{
    uint8_t temp1[12];

    memset(temp1, 0x00, sizeof(temp1));
    memcpy(temp1, facch10, 10U);
    putBits(temp1, 80U, nxdnCrc12(temp1, 80U), 12U);

    chanEncode(1U, temp1, 96U, 192U, frame, offset);
}

void nxdnFacch1GetRaw(const uint8_t* facch10, uint8_t* raw12)
{
    memset(raw12, 0x00, 12U);
    memcpy(raw12, facch10, 10U);
    putBits(raw12, 80U, nxdnCrc12(raw12, 80U), 12U);
}

/*
 * UDCH / FACCH2.  184 bits (RAN + layer 3), CRC15 and 4 tail bits (203),
 * coded to 406 and punctured to 348.
 */
bool nxdnUdchDecode(const uint8_t* frame, uint8_t* udch)
{
    uint8_t out[26];

    memset(out, 0x00, sizeof(out));

    chanDecode(2U, frame, NXDN_FSW_LENGTH_BITS + NXDN_LICH_LENGTH_BITS, NXDN_FACCH2_LENGTH_BITS, 207U, out, 203U);

    memcpy(udch, out, 25U);

    return checkCrc15(udch, 184U);
}

void nxdnUdchEncode(const uint8_t* udch23, uint8_t* frame)
{
    uint8_t temp1[26];

    memset(temp1, 0x00, sizeof(temp1));
    memcpy(temp1, udch23, 23U);
    putBits(temp1, 184U, nxdnCrc15(temp1, 184U), 15U);

    chanEncode(2U, temp1, 203U, 406U, frame, NXDN_FSW_LENGTH_BITS + NXDN_LICH_LENGTH_BITS);
}

/*
 * Voice.  The 72 bit AMBE+2 block holds a 24 bit extended Golay word (the
 * 12 voice parameter bits a), a 23 bit Golay word of 12 bits b scrambled by a
 * sequence selected with a, and 25 uncoded bits c.  The bits are spread over
 * the block with the tables below.
 */
static const unsigned int A_TABLE[] =
{
     0U,  4U,  8U, 12U, 16U, 20U, 24U, 28U, 32U, 36U, 40U, 44U,
    48U, 52U, 56U, 60U, 64U, 68U,  1U,  5U,  9U, 13U, 17U, 21U
};

static const unsigned int B_TABLE[] =
{
    25U, 29U, 33U, 37U, 41U, 45U, 49U, 53U, 57U, 61U, 65U, 69U,
     2U,  6U, 10U, 14U, 18U, 22U, 26U, 30U, 34U, 38U, 42U
};

static const unsigned int C_TABLE[] =
{
    46U, 50U, 54U, 58U, 62U, 66U, 70U,  3U,  7U, 11U, 15U, 19U,
    23U, 27U, 31U, 35U, 39U, 43U, 47U, 51U, 55U, 59U, 63U, 67U, 71U
};

/*
 * nxdnAudioDecodeBlock()
 *   Takes one 72 bit block and writes the 49 bit parameter set (a, b, c) at
 *   bit offset 'offset' of out.  Errors in the Golay words are corrected.
 *   Returns the number of corrected bits.
 */
unsigned int nxdnAudioDecodeBlock(const uint8_t* in, uint8_t* out, unsigned int offset)
{
    unsigned int a = 0U;
    unsigned int MASK = 0x800000U;

    for (unsigned int i = 0U; i < 24U; i++, MASK >>= 1)
        if (READ_BIT1(in, A_TABLE[i]))
            a |= MASK;

    unsigned int b = 0U;
    MASK = 0x400000U;

    for (unsigned int i = 0U; i < 23U; i++, MASK >>= 1)
        if (READ_BIT1(in, B_TABLE[i]))
            b |= MASK;

    unsigned int c = 0U;
    MASK = 0x1000000U;

    for (unsigned int i = 0U; i < 25U; i++, MASK >>= 1)
        if (READ_BIT1(in, C_TABLE[i]))
            c |= MASK;

    unsigned int a12 = 0U;
    unsigned int b12 = 0U;
    unsigned int errors = golay24128_errors24128(a, &a12);

    b ^= (AMBE_PRNG_TABLE[a12] >> 1);
    errors += golay24128_errors23127(b, &b12);

    MASK = 0x000800U;
    for (unsigned int i = 0U; i < 12U; i++, MASK >>= 1)
    {
        unsigned int aPos = i + offset + 0U;
        unsigned int bPos = i + offset + 12U;
        bool ab = (a12 & MASK) != 0U;
        bool bb = (b12 & MASK) != 0U;
        WRITE_BIT1(out, aPos, ab);
        WRITE_BIT1(out, bPos, bb);
    }

    MASK = 0x1000000U;
    for (unsigned int i = 0U; i < 25U; i++, MASK >>= 1)
    {
        unsigned int cPos = i + offset + 24U;
        bool cb = (c & MASK) != 0U;
        WRITE_BIT1(out, cPos, cb);
    }

    return errors;
}

void nxdnAudioEncodeBlock(const uint8_t* in, uint8_t* out, unsigned int offset)
{
    unsigned int aOrig = 0U;
    unsigned int bOrig = 0U;
    unsigned int cOrig = 0U;

    unsigned int MASK = 0x000800U;
    for (unsigned int i = 0U; i < 12U; i++, MASK >>= 1)
    {
        unsigned int n1 = i + offset + 0U;
        unsigned int n2 = i + offset + 12U;

        if (READ_BIT1(in, n1))
            aOrig |= MASK;
        if (READ_BIT1(in, n2))
            bOrig |= MASK;
    }

    MASK = 0x1000000U;
    for (unsigned int i = 0U; i < 25U; i++, MASK >>= 1)
    {
        unsigned int n = i + offset + 24U;

        if (READ_BIT1(in, n))
            cOrig |= MASK;
    }

    unsigned int a = golay24128_encode24128(aOrig);
    unsigned int p = AMBE_PRNG_TABLE[aOrig] >> 1;
    unsigned int b = golay24128_encode23127(bOrig) >> 1;
    b ^= p;

    MASK = 0x800000U;
    for (unsigned int i = 0U; i < 24U; i++, MASK >>= 1)
    {
        bool v = (a & MASK) != 0U;
        WRITE_BIT1(out, A_TABLE[i], v);
    }

    MASK = 0x400000U;
    for (unsigned int i = 0U; i < 23U; i++, MASK >>= 1)
    {
        bool v = (b & MASK) != 0U;
        WRITE_BIT1(out, B_TABLE[i], v);
    }

    MASK = 0x1000000U;
    for (unsigned int i = 0U; i < 25U; i++, MASK >>= 1)
    {
        bool v = (cOrig & MASK) != 0U;
        WRITE_BIT1(out, C_TABLE[i], v);
    }
}

/*
 * nxdnAudioChipToNet49() / nxdnAudioNet49ToChip()
 *   The AMBE3000R, set to its 72 bit (DMR) rate, delivers the 49 voice bits without
 *   Golay parity: the 49 bits (a 12, b 12, c 25 in the order of the network form),
 *   then 23 zero bits, read row by row from a matrix of 4 rows and 18 columns whose
 *   bit k is at position 4 * (k % 18) + k / 18 of the 72 bit block.
 *   Worked out from 470 frames of a real dongle: those 23 positions are always zero
 *   and the 49 others are independent.
 */
static unsigned int chipPosition(unsigned int k)
{
    return 4U * (k % 18U) + k / 18U;
}

void nxdnAudioChipToNet49(const uint8_t* chip, uint8_t* net7)
{
    memset(net7, 0x00, 7U);

    for (unsigned int k = 0U; k < 49U; k++)
        WRITE_BIT1(net7, k, READ_BIT1(chip, chipPosition(k)) != 0U);
}

void nxdnAudioNet49ToChip(const uint8_t* net7, uint8_t* chip)
{
    memset(chip, 0x00, 9U);

    for (unsigned int k = 0U; k < 49U; k++)
        WRITE_BIT1(chip, chipPosition(k), READ_BIT1(net7, k) != 0U);
}

/*
 * Coding order and air order.  With its FEC on, a vocoder chip may deliver the 72 bit block
 * as the air block itself (NXDN_CHIP_AIR), or in coding order, 24 bits a, 23 bits b, 25 bits c
 * (NXDN_CHIP_CODING), which the tables of the air block then spread.
 */
static void chipCodingToAir(const uint8_t* chip, uint8_t* air)
{
    memset(air, 0x00, 9U);

    for (unsigned int i = 0U; i < 24U; i++)
        WRITE_BIT1(air, A_TABLE[i], READ_BIT1(chip, i) != 0U);
    for (unsigned int i = 0U; i < 23U; i++)
        WRITE_BIT1(air, B_TABLE[i], READ_BIT1(chip, 24U + i) != 0U);
    for (unsigned int i = 0U; i < 25U; i++)
        WRITE_BIT1(air, C_TABLE[i], READ_BIT1(chip, 47U + i) != 0U);
}

static void airToChipCoding(const uint8_t* air, uint8_t* chip)
{
    memset(chip, 0x00, 9U);

    for (unsigned int i = 0U; i < 24U; i++)
        WRITE_BIT1(chip, i, READ_BIT1(air, A_TABLE[i]) != 0U);
    for (unsigned int i = 0U; i < 23U; i++)
        WRITE_BIT1(chip, 24U + i, READ_BIT1(air, B_TABLE[i]) != 0U);
    for (unsigned int i = 0U; i < 25U; i++)
        WRITE_BIT1(chip, 47U + i, READ_BIT1(air, C_TABLE[i]) != 0U);
}

unsigned int nxdnAudioChipDecode(unsigned int layout, const uint8_t* chip, uint8_t* net7)
{
    uint8_t air[9];

    memset(net7, 0x00, 7U);

    switch (layout)
    {
        case NXDN_CHIP_AIR:
            return nxdnAudioDecodeBlock(chip, net7, 0U);
        case NXDN_CHIP_CODING:
            chipCodingToAir(chip, air);
            return nxdnAudioDecodeBlock(air, net7, 0U);
        default:
            nxdnAudioChipToNet49(chip, net7);
            return nxdnAudioChipPadding(chip);
    }
}

void nxdnAudioChipEncode(unsigned int layout, const uint8_t* net7, uint8_t* chip)
{
    uint8_t air[9];

    switch (layout)
    {
        case NXDN_CHIP_AIR:
            memset(chip, 0x00, 9U);
            nxdnAudioEncodeBlock(net7, chip, 0U);
            break;
        case NXDN_CHIP_CODING:
            memset(air, 0x00, sizeof(air));
            nxdnAudioEncodeBlock(net7, air, 0U);
            airToChipCoding(air, chip);
            break;
        default:
            nxdnAudioNet49ToChip(net7, chip);
            break;
    }
}

/* Number of the 23 padding bits of a chip block that are set (0 for a block in the layout above) */
unsigned int nxdnAudioChipPadding(const uint8_t* chip)
{
    unsigned int n = 0U;

    for (unsigned int k = 49U; k < 72U; k++)
        if (READ_BIT1(chip, chipPosition(k)) != 0U)
            n++;
    return n;
}

/*
 * nxdnAudioDecode() / nxdnAudioEncode()
 *   Two consecutive 72 bit blocks (18 bytes on air) to or from 98 bits (13
 *   bytes plus padding) of network data.
 */
void nxdnAudioDecode(const uint8_t* in, uint8_t* out, unsigned int* errors)
{
    unsigned int e = 0U;

    e += nxdnAudioDecodeBlock(in + 0U, out, 0U);
    e += nxdnAudioDecodeBlock(in + 9U, out, 49U);

    if (errors != NULL)
        *errors += e;
}

void nxdnAudioEncode(const uint8_t* in, uint8_t* out)
{
    nxdnAudioEncodeBlock(in, out + 0U, 0U);
    nxdnAudioEncodeBlock(in, out + 9U, 49U);
}

/*
 * nxdnL3Decode()
 *   Copies lengthBits of bytes into the layer 3 buffer at bit offset 'offset'.
 */
void nxdnL3Decode(uint8_t* l3, const uint8_t* bytes, unsigned int lengthBits, unsigned int offset)
{
    for (unsigned int i = 0U; i < lengthBits; i++, offset++)
    {
        bool b = READ_BIT1(bytes, i) != 0U;
        WRITE_BIT1(l3, offset, b);
    }
}

/* ------------------------------------------------------------------------ */
/* Control                                                                   */
/* ------------------------------------------------------------------------ */

void nxdnCtrlInit(NxdnCtrl* c)
{
    c->ran = 1U;
    c->id = 0U;
    c->selfOnly = false;
    c->remoteGateway = false;
    c->localRepeat = false;
    c->duplex = false;
    c->rfTimeoutMs = 180000U;
    c->netWatchdogMs = 1500U;
    c->sendNet = NULL;
    c->sendRf = NULL;
    c->event = NULL;
    c->ctx = NULL;
    c->rfState = NXDN_RF_LISTENING;
    c->netState = NXDN_NET_IDLE;
    c->rfMask = 0U;
    c->netMask = 0U;
    memset(c->rfLayer3, 0x00, sizeof(c->rfLayer3));
    memset(c->netLayer3, 0x00, sizeof(c->netLayer3));
    c->lastLich = 0U;
    c->rfFrames = 0U;
    c->rfErrs = 0U;
    c->rfBits = 1U;
    c->netFrames = 0U;
    c->rfTimer = 0U;
    c->rfTimerRunning = false;
    c->netTimer = 0U;
    c->netWatchdog = 0U;
    c->netWatchdogRunning = false;
}

static void emit(NxdnCtrl* c, NxdnEvent ev, const uint8_t* l3, bool data, float seconds, float ber)
{
    NxdnCall call;

    if (c->event == NULL)
        return;

    call.event = ev;
    call.src = NXDN_L3_SRC(l3);
    call.dst = NXDN_L3_DST(l3);
    call.group = NXDN_L3_GROUP(l3);
    call.data = data;
    call.seconds = seconds;
    call.ber = ber;

    c->event(c->ctx, &call);
}

static float rfBer(const NxdnCtrl* c)
{
    return (float)(c->rfErrs * 100U) / (float)c->rfBits;
}

static bool rfTimedOut(const NxdnCtrl* c)
{
    return c->rfTimerRunning && c->rfTimer >= c->rfTimeoutMs;
}

static void toNet(NxdnCtrl* c, const uint8_t* netData, uint8_t type)
{
    if (c->sendNet == NULL || rfTimedOut(c))
        return;

    c->sendNet(c->ctx, netData, type);
}

/* frame is unscrambled; it is scrambled on a copy before it is sent */
static void toRf(NxdnCtrl* c, const uint8_t* frame, bool eot)
{
    uint8_t tmp[NXDN_FRAME_LENGTH_BYTES];

    if (c->sendRf == NULL)
        return;

    memcpy(tmp, frame, NXDN_FRAME_LENGTH_BYTES);
    nxdnScramble(tmp);

    c->sendRf(c->ctx, tmp, eot);
}

static void endRf(NxdnCtrl* c)
{
    c->rfState = NXDN_RF_LISTENING;
    c->rfMask = 0U;
    memset(c->rfLayer3, 0x00, sizeof(c->rfLayer3));
    c->rfTimerRunning = false;
    c->rfTimer = 0U;
}

static void endNet(NxdnCtrl* c)
{
    c->netState = NXDN_NET_IDLE;
    c->netMask = 0U;
    memset(c->netLayer3, 0x00, sizeof(c->netLayer3));
    c->netTimer = 0U;
    c->netWatchdogRunning = false;
    c->netWatchdog = 0U;
}

/*
 * direction()
 *   A simplex hotspot or a remote gateway sends everything "inbound" to the
 *   radios, a duplex repeater sends "outbound".  The copy that goes to the
 *   gateway is always inbound.
 */
static uint8_t direction(const NxdnCtrl* c)
{
    return (c->remoteGateway || !c->duplex) ? NXDN_LICH_DIRECTION_INBOUND : NXDN_LICH_DIRECTION_OUTBOUND;
}

static uint8_t inboundRaw(uint8_t lich)
{
    return lich & ~0x02U;
}

static void startRf(NxdnCtrl* c, NxdnEvent ev)
{
    c->rfFrames = 0U;
    c->rfErrs = 0U;
    c->rfBits = 1U;
    c->rfTimer = 0U;
    c->rfTimerRunning = true;
    c->rfState = NXDN_RF_AUDIO;

    emit(c, ev, c->rfLayer3, false, 0.0F, 0.0F);
}

/* Builds the frame that carries a layer 3 message in both FACCH1 slots */
static void buildFacchFrame(NxdnCtrl* c, const uint8_t* l3, uint8_t* frame, uint8_t* netData)
{
    uint8_t sacch[5];
    uint8_t lich;
    uint8_t raw[12];

    memset(frame, 0x00, NXDN_FRAME_LENGTH_BYTES);
    nxdnAddSync(frame);

    lich = nxdnLichMake(NXDN_LICH_RFCT_RDCH, NXDN_LICH_USC_SACCH_NS, NXDN_LICH_STEAL_FACCH, direction(c));
    nxdnLichEncode(lich, frame);

    memset(sacch, 0x00, sizeof(sacch));
    nxdnSacchSetRAN(sacch, c->ran);
    nxdnSacchSetStructure(sacch, NXDN_SR_SINGLE);
    nxdnSacchSetData(sacch, SACCH_IDLE);
    nxdnSacchEncode(sacch, frame);

    nxdnFacch1Encode(l3, frame, NXDN_FACCH1_OFFSET_1);
    nxdnFacch1Encode(l3, frame, NXDN_FACCH1_OFFSET_2);

    if (netData != NULL)
    {
        memset(netData, 0x00, NXDN_NET_DATA_LENGTH);
        netData[0] = inboundRaw(lich);
        nxdnSacchGetRaw(sacch, netData + 1U);
        nxdnFacch1GetRaw(l3, raw);
        memcpy(netData + 5U, raw, 12U);
        memcpy(netData + 19U, raw, 12U);
    }
}

static bool processVoice(NxdnCtrl* c, uint8_t usc, uint8_t option, uint8_t* data)
{
    uint8_t sacch[5];
    uint8_t netData[NXDN_NET_DATA_LENGTH];

    memset(sacch, 0x00, sizeof(sacch));

    bool valid = nxdnSacchDecode(data, sacch);

    if (valid)
    {
        uint8_t ran = NXDN_SACCH_RAN(sacch);

        if (ran != c->ran && ran != 0U)
            return false;
    }
    else if (c->rfState == NXDN_RF_LISTENING)
    {
        return false;
    }

    memset(netData, 0x00, sizeof(netData));

    if (usc == NXDN_LICH_USC_SACCH_NS)
    {
        /* Non superblock frame: header or trailer carried in FACCH1 */
        uint8_t facch[12];

        bool v = nxdnFacch1Decode(data, NXDN_FACCH1_OFFSET_1, facch);
        if (!v)
            v = nxdnFacch1Decode(data, NXDN_FACCH1_OFFSET_2, facch);
        if (!v)
            return false;

        uint8_t layer3[22];
        memset(layer3, 0x00, sizeof(layer3));
        nxdnL3Decode(layer3, facch, 80U, 0U);

        uint8_t type = NXDN_L3_TYPE(layer3);

        if (type == NXDN_MESSAGE_TYPE_TX_REL)
        {
            if (c->rfState != NXDN_RF_AUDIO)
            {
                c->rfState = NXDN_RF_LISTENING;
                c->rfMask = 0U;
                memset(c->rfLayer3, 0x00, sizeof(c->rfLayer3));
                return false;
            }
        }
        else if (type == NXDN_MESSAGE_TYPE_VCALL)
        {
            if (c->rfState == NXDN_RF_LISTENING && c->selfOnly)
            {
                if (NXDN_L3_SRC(layer3) != c->id)
                {
                    c->rfState = NXDN_RF_REJECTED;
                    emit(c, NXDN_EV_RF_REJECTED, layer3, false, 0.0F, 0.0F);
                    return true;
                }
            }
        }
        else
        {
            return false;
        }

        memcpy(c->rfLayer3, layer3, sizeof(layer3));

        bool eot = type == NXDN_MESSAGE_TYPE_TX_REL;
        uint8_t frame[NXDN_FRAME_LENGTH_BYTES];

        buildFacchFrame(c, layer3, frame, netData);

        toNet(c, netData, eot ? NXDN_NET_VOICE_TRAILER : NXDN_NET_VOICE_HEADER);

        if (c->localRepeat)
            toRf(c, frame, eot);

        if (eot)
        {
            c->rfFrames++;
            emit(c, NXDN_EV_RF_END, c->rfLayer3, false, (float)c->rfFrames / 12.5F, rfBer(c));
            endRf(c);
        }
        else
        {
            startRf(c, NXDN_EV_RF_START);
        }

        return true;
    }

    if (c->rfState == NXDN_RF_LISTENING)
    {
        /* Late entry: find the call information in FACCH1 or the SACCH superblock */
        uint8_t facch[12];
        bool v = false;
        bool hasInfo = false;

        switch (option)
        {
            case NXDN_LICH_STEAL_FACCH:
                v = nxdnFacch1Decode(data, NXDN_FACCH1_OFFSET_1, facch);
                if (!v)
                    v = nxdnFacch1Decode(data, NXDN_FACCH1_OFFSET_2, facch);
                break;
            case NXDN_LICH_STEAL_FACCH1_1:
                v = nxdnFacch1Decode(data, NXDN_FACCH1_OFFSET_1, facch);
                break;
            case NXDN_LICH_STEAL_FACCH1_2:
                v = nxdnFacch1Decode(data, NXDN_FACCH1_OFFSET_2, facch);
                break;
            default:
                break;
        }

        if (v)
        {
            uint8_t layer3[22];
            memset(layer3, 0x00, sizeof(layer3));
            nxdnL3Decode(layer3, facch, 80U, 0U);

            hasInfo = NXDN_L3_TYPE(layer3) == NXDN_MESSAGE_TYPE_VCALL;
            if (!hasInfo)
                return false;

            memcpy(c->rfLayer3, layer3, sizeof(layer3));
        }

        if (!hasInfo)
        {
            if (!valid)
                return false;

            uint8_t message[3];
            nxdnSacchGetData(sacch, message);

            switch (NXDN_SACCH_STRUCTURE(sacch))
            {
                case NXDN_SR_1_4:
                    nxdnL3Decode(c->rfLayer3, message, 18U, 0U);
                    c->rfMask = NXDN_L3_TYPE(c->rfLayer3) == NXDN_MESSAGE_TYPE_VCALL ? 0x01U : 0x00U;
                    break;
                case NXDN_SR_2_4:
                    c->rfMask |= 0x02U;
                    nxdnL3Decode(c->rfLayer3, message, 18U, 18U);
                    break;
                case NXDN_SR_3_4:
                    c->rfMask |= 0x04U;
                    nxdnL3Decode(c->rfLayer3, message, 18U, 36U);
                    break;
                case NXDN_SR_4_4:
                    c->rfMask |= 0x08U;
                    nxdnL3Decode(c->rfLayer3, message, 18U, 54U);
                    break;
                default:
                    break;
            }

            if (c->rfMask != 0x0FU)
                return false;

            if (NXDN_L3_TYPE(c->rfLayer3) != NXDN_MESSAGE_TYPE_VCALL)
                return false;
        }

        if (c->selfOnly && NXDN_L3_SRC(c->rfLayer3) != c->id)
        {
            c->rfState = NXDN_RF_REJECTED;
            emit(c, NXDN_EV_RF_REJECTED, c->rfLayer3, false, 0.0F, 0.0F);
            return true;
        }

        startRf(c, NXDN_EV_RF_LATE);

        /* A dummy start message for the network and the transmitter */
        uint8_t start[NXDN_FRAME_LENGTH_BYTES];
        buildFacchFrame(c, c->rfLayer3, start, netData);

        toNet(c, netData, NXDN_NET_VOICE_HEADER);

        if (c->localRepeat)
            toRf(c, start, false);

        memset(netData, 0x00, sizeof(netData));
    }

    if (c->rfState == NXDN_RF_AUDIO)
    {
        uint8_t lich = nxdnLichMake(NXDN_LICH_RFCT_RDCH, NXDN_LICH_USC_SACCH_SS, option, direction(c));

        nxdnAddSync(data);
        nxdnLichEncode(lich, data);
        netData[0] = inboundRaw(lich);

        if (valid)
        {
            nxdnSacchSetRAN(sacch, c->ran);
            nxdnSacchEncode(sacch, data);
        }

        nxdnSacchGetRaw(sacch, netData + 1U);

        unsigned int errors = 0U;
        unsigned int bits = 0U;
        const uint8_t* aud = data + NXDN_FSW_LICH_SACCH_LENGTH_BYTES;
        uint8_t facch[12];

        if (option == NXDN_LICH_STEAL_NONE)
        {
            nxdnAudioDecode(aud + 0U, netData + 5U + 0U, &errors);
            nxdnAudioDecode(aud + 18U, netData + 5U + 14U, &errors);
            bits = 188U;
        }
        else if (option == NXDN_LICH_STEAL_FACCH1_1)
        {
            if (nxdnFacch1Decode(data, NXDN_FACCH1_OFFSET_1, facch))
                nxdnFacch1Encode(facch, data, NXDN_FACCH1_OFFSET_1);
            nxdnFacch1GetRaw(facch, netData + 5U + 0U);

            nxdnAudioDecode(aud + 18U, netData + 5U + 14U, &errors);
            bits = 94U;
        }
        else if (option == NXDN_LICH_STEAL_FACCH1_2)
        {
            nxdnAudioDecode(aud + 0U, netData + 5U + 0U, &errors);
            bits = 94U;

            if (nxdnFacch1Decode(data, NXDN_FACCH1_OFFSET_2, facch))
                nxdnFacch1Encode(facch, data, NXDN_FACCH1_OFFSET_2);
            nxdnFacch1GetRaw(facch, netData + 5U + 14U);
        }
        else
        {
            if (nxdnFacch1Decode(data, NXDN_FACCH1_OFFSET_1, facch))
                nxdnFacch1Encode(facch, data, NXDN_FACCH1_OFFSET_1);
            nxdnFacch1GetRaw(facch, netData + 5U + 0U);

            if (nxdnFacch1Decode(data, NXDN_FACCH1_OFFSET_2, facch))
                nxdnFacch1Encode(facch, data, NXDN_FACCH1_OFFSET_2);
            nxdnFacch1GetRaw(facch, netData + 5U + 14U);
        }

        c->rfErrs += errors;
        c->rfBits += bits;

        toNet(c, netData, NXDN_NET_VOICE_BODY);

        if (c->localRepeat)
            toRf(c, data, false);

        c->rfFrames++;
    }

    return true;
}

static bool processData(NxdnCtrl* c, uint8_t option, uint8_t* data)
{
    uint8_t udch[25];
    uint8_t netData[NXDN_NET_DATA_LENGTH];

    memset(udch, 0x00, sizeof(udch));

    bool validUdch = nxdnUdchDecode(data, udch);

    if (c->rfState == NXDN_RF_LISTENING && !validUdch)
        return false;

    if (validUdch)
    {
        uint8_t ran = udch[0] & 0x3FU;

        if (ran != c->ran && ran != 0U)
            return false;
    }

    memset(netData, 0x00, sizeof(netData));

    uint8_t layer3[22];
    memcpy(layer3, udch + 1U, 22U);

    if (c->rfState == NXDN_RF_LISTENING)
    {
        if (NXDN_L3_TYPE(layer3) != NXDN_MESSAGE_TYPE_DCALL_HDR)
            return false;

        if (c->selfOnly && NXDN_L3_SRC(layer3) != c->id)
            return false;

        memcpy(c->rfLayer3, layer3, sizeof(layer3));
        c->rfFrames = 0U;
        c->rfState = NXDN_RF_DATA;

        emit(c, NXDN_EV_RF_START, layer3, true, 0.0F, 0.0F);
    }

    if (c->rfState != NXDN_RF_DATA)
        return false;

    nxdnAddSync(data);

    uint8_t lich = nxdnLichMake(NXDN_LICH_RFCT_RDCH, NXDN_LICH_USC_UDCH, option, direction(c));
    nxdnLichEncode(lich, data);

    netData[0] = inboundRaw(lich);
    memcpy(netData + 1U, udch, 23U);

    uint8_t type = NXDN_MESSAGE_TYPE_DCALL_DATA;
    bool eot = false;

    if (validUdch)
    {
        type = NXDN_L3_TYPE(layer3);
        eot = type == NXDN_MESSAGE_TYPE_TX_REL;
        udch[0] = c->ran;
        nxdnUdchEncode(udch, data);
    }

    switch (type)
    {
        case NXDN_MESSAGE_TYPE_DCALL_HDR:
            toNet(c, netData, NXDN_NET_DATA_HEADER);
            break;
        case NXDN_MESSAGE_TYPE_TX_REL:
            toNet(c, netData, NXDN_NET_DATA_TRAILER);
            break;
        default:
            toNet(c, netData, NXDN_NET_DATA_BODY);
            break;
    }

    if (c->localRepeat)
        toRf(c, data, eot);

    c->rfFrames++;

    if (eot)
    {
        emit(c, NXDN_EV_RF_END, c->rfLayer3, true, (float)c->rfFrames / 12.5F, 0.0F);
        endRf(c);
    }

    return true;
}

/*
 * nxdnCtrlRf()
 *   Handles one frame from the receiver.  'frame' holds the 48 bytes as they
 *   came off the air (scrambled).  lost = true tells the control that the
 *   signal has gone without an end of transmission frame.
 */
void nxdnCtrlRf(NxdnCtrl* c, const uint8_t* frame, bool lost)
{
    uint8_t data[NXDN_FRAME_LENGTH_BYTES];

    if (lost)
    {
        if (c->rfState == NXDN_RF_AUDIO)
            emit(c, NXDN_EV_RF_LOST, c->rfLayer3, false, (float)c->rfFrames / 12.5F, rfBer(c));
        else if (c->rfState == NXDN_RF_DATA)
            emit(c, NXDN_EV_RF_LOST, c->rfLayer3, true, (float)c->rfFrames / 12.5F, 0.0F);

        endRf(c);
        return;
    }

    memcpy(data, frame, NXDN_FRAME_LENGTH_BYTES);
    nxdnScramble(data);

    uint8_t lich;
    bool v = nxdnLichDecode(data, &lich);

    if (v)
        c->lastLich = lich;

    /* Stop repeater packets coming through, unless we're acting as a remote gateway */
    uint8_t dir = NXDN_LICH_DIRECTION(c->lastLich);

    if (c->remoteGateway)
    {
        if (dir == NXDN_LICH_DIRECTION_INBOUND)
            return;
    }
    else
    {
        if (dir == NXDN_LICH_DIRECTION_OUTBOUND)
            return;
    }

    uint8_t usc = NXDN_LICH_FCT(c->lastLich);
    uint8_t option = NXDN_LICH_OPTION(c->lastLich);

    if (usc == NXDN_LICH_USC_UDCH)
        processData(c, option, data);
    else
        processVoice(c, usc, option, data);
}

/* Closes the transmission on the air of a network call that ended without a release message */
static void netCloseOnAir(NxdnCtrl* c)
{
    uint8_t l3[22];
    uint8_t frame[NXDN_FRAME_LENGTH_BYTES];

    memcpy(l3, c->netLayer3, sizeof(l3));
    l3[0] = (l3[0] & 0xC0U) | NXDN_MESSAGE_TYPE_TX_REL;
    buildFacchFrame(c, l3, frame, NULL);
    toRf(c, frame, true);
}

/*
 * nxdnCtrlNet()
 *   Handles one NXDN message from the gateway and queues the frame it
 *   describes for transmission.
 */
void nxdnCtrlNet(NxdnCtrl* c, const uint8_t* netData)
{
    uint8_t data[NXDN_FRAME_LENGTH_BYTES];

    if (c->rfState != NXDN_RF_LISTENING && c->netState == NXDN_NET_IDLE)
        return;

    c->netWatchdogRunning = true;
    c->netWatchdog = 0U;

    memset(data, 0x00, sizeof(data));
    nxdnAddSync(data);

    uint8_t lich = netData[0];
    uint8_t usc = NXDN_LICH_FCT(lich);
    uint8_t option = NXDN_LICH_OPTION(lich);

    lich = (lich & ~0x02U) | ((direction(c) << 1) & 0x02U);
    nxdnLichEncode(lich, data);

    if (usc == NXDN_LICH_USC_UDCH)
    {
        uint8_t layer3[22];
        memcpy(layer3, netData + 2U, 22U);
        uint8_t type = NXDN_L3_TYPE(layer3);

        if (c->netState == NXDN_NET_IDLE)
        {
            if (type == NXDN_MESSAGE_TYPE_DCALL_HDR)
            {
                memcpy(c->netLayer3, layer3, 22U);
                emit(c, NXDN_EV_NET_START, layer3, true, 0.0F, 0.0F);
                c->netState = NXDN_NET_DATA;
                c->netTimer = 0U;
            }
            else
            {
                return;
            }
        }

        if (c->netState == NXDN_NET_DATA)
        {
            uint8_t udch[23];

            udch[0] = c->ran;
            memcpy(udch + 1U, netData + 2U, 22U);
            nxdnUdchEncode(udch, data);

            bool eot = type == NXDN_MESSAGE_TYPE_TX_REL;

            toRf(c, data, eot);

            if (eot)
            {
                emit(c, NXDN_EV_NET_END, c->netLayer3, true, 0.0F, 0.0F);
                endNet(c);
            }
        }
    }
    else if (usc == NXDN_LICH_USC_SACCH_NS)
    {
        uint8_t sacch[5];
        uint8_t oldL3[22];

        memcpy(oldL3, c->netLayer3, sizeof(oldL3));

        memset(c->netLayer3, 0x00, sizeof(c->netLayer3));
        memcpy(c->netLayer3, netData + 5U, 10U);

        uint8_t type = NXDN_L3_TYPE(c->netLayer3);

        if (type == NXDN_MESSAGE_TYPE_TX_REL && c->netState == NXDN_NET_IDLE)
            return;

        if (type == NXDN_MESSAGE_TYPE_VCALL && c->netState != NXDN_NET_IDLE)
        {
            /*
             * A call message in the middle of a call.  From the same station it is a repeat of the call message and
             * is ignored.  From another one, the release of the call before never came (a gateway or bridge that
             * does not send it): that call is over, and the new one starts here.  Without this the new talker is
             * heard, and logged, under the name of the one before for as long as he talks, because every packet
             * of his keeps the network watchdog from running out.
             */
            if (NXDN_L3_SRC(oldL3) == 0U || NXDN_L3_SRC(c->netLayer3) == NXDN_L3_SRC(oldL3))
                return;

            uint8_t newL3[22];
            bool wasData = c->netState == NXDN_NET_DATA;

            memcpy(newL3, c->netLayer3, sizeof(newL3));
            memcpy(c->netLayer3, oldL3, sizeof(oldL3));

            emit(c, NXDN_EV_NET_SWITCH, c->netLayer3, wasData, (float)c->netFrames / 12.5F, 0.0F);
            netCloseOnAir(c);
            endNet(c);

            memcpy(c->netLayer3, newL3, sizeof(newL3));
        }

        memset(sacch, 0x00, sizeof(sacch));
        nxdnSacchSetRAN(sacch, c->ran);
        nxdnSacchSetStructure(sacch, NXDN_SR_SINGLE);
        nxdnSacchSetData(sacch, SACCH_IDLE);
        nxdnSacchEncode(sacch, data);

        nxdnFacch1Encode(netData + 5U, data, NXDN_FACCH1_OFFSET_1);
        nxdnFacch1Encode(netData + 5U, data, NXDN_FACCH1_OFFSET_2);

        bool eot = type == NXDN_MESSAGE_TYPE_TX_REL;

        toRf(c, data, eot);

        if (eot)
        {
            c->netFrames++;
            emit(c, NXDN_EV_NET_END, c->netLayer3, false, (float)c->netFrames / 12.5F, 0.0F);
            endNet(c);
        }
        else if (type == NXDN_MESSAGE_TYPE_VCALL)
        {
            emit(c, NXDN_EV_NET_START, c->netLayer3, false, 0.0F, 0.0F);
            c->netTimer = 0U;
            c->netState = NXDN_NET_AUDIO;
            c->netFrames = 1U;
        }
    }
    else
    {
        if (c->netState == NXDN_NET_IDLE)
        {
            /* Late entry from the network: wait for the whole superblock */
            uint8_t structure = (netData[1] >> 6) & 0x03U;

            switch (structure)
            {
                case NXDN_SR_1_4:
                    nxdnL3Decode(c->netLayer3, netData + 2U, 18U, 0U);
                    c->netMask = NXDN_L3_TYPE(c->netLayer3) == NXDN_MESSAGE_TYPE_VCALL ? 0x01U : 0x00U;
                    break;
                case NXDN_SR_2_4:
                    c->netMask |= 0x02U;
                    nxdnL3Decode(c->netLayer3, netData + 2U, 18U, 18U);
                    break;
                case NXDN_SR_3_4:
                    c->netMask |= 0x04U;
                    nxdnL3Decode(c->netLayer3, netData + 2U, 18U, 36U);
                    break;
                case NXDN_SR_4_4:
                    c->netMask |= 0x08U;
                    nxdnL3Decode(c->netLayer3, netData + 2U, 18U, 54U);
                    break;
                default:
                    break;
            }

            if (c->netMask != 0x0FU)
                return;

            if (NXDN_L3_TYPE(c->netLayer3) != NXDN_MESSAGE_TYPE_VCALL)
                return;

            emit(c, NXDN_EV_NET_START, c->netLayer3, false, 0.0F, 0.0F);

            c->netTimer = 0U;
            c->netWatchdogRunning = true;
            c->netWatchdog = 0U;
            c->netState = NXDN_NET_AUDIO;
            c->netFrames = 1U;

            uint8_t start[NXDN_FRAME_LENGTH_BYTES];
            buildFacchFrame(c, c->netLayer3, start, NULL);
            toRf(c, start, false);
        }

        c->netFrames++;

        uint8_t sacch[5];

        memset(sacch, 0x00, sizeof(sacch));
        memcpy(sacch, netData + 1U, 4U);
        nxdnSacchSetRAN(sacch, c->ran);
        nxdnSacchEncode(sacch, data);

        if (option == NXDN_LICH_STEAL_NONE)
        {
            nxdnAudioEncode(netData + 5U + 0U, data + NXDN_FSW_LICH_SACCH_LENGTH_BYTES + 0U);
            nxdnAudioEncode(netData + 5U + 14U, data + NXDN_FSW_LICH_SACCH_LENGTH_BYTES + 18U);
        }
        else if (option == NXDN_LICH_STEAL_FACCH1_1)
        {
            nxdnFacch1Encode(netData + 5U + 0U, data, NXDN_FACCH1_OFFSET_1);
            nxdnAudioEncode(netData + 5U + 14U, data + NXDN_FSW_LICH_SACCH_LENGTH_BYTES + 18U);
        }
        else if (option == NXDN_LICH_STEAL_FACCH1_2)
        {
            nxdnAudioEncode(netData + 5U + 0U, data + NXDN_FSW_LICH_SACCH_LENGTH_BYTES + 0U);
            nxdnFacch1Encode(netData + 5U + 14U, data, NXDN_FACCH1_OFFSET_2);
        }
        else
        {
            nxdnFacch1Encode(netData + 5U + 0U, data, NXDN_FACCH1_OFFSET_1);
            nxdnFacch1Encode(netData + 5U + 14U, data, NXDN_FACCH1_OFFSET_2);
        }

        toRf(c, data, false);
    }
}

/*
 * nxdnCtrlClock()
 *   Advances the timers.  The network watchdog ends a network transmission
 *   that stopped without a trailer.
 */
void nxdnCtrlClock(NxdnCtrl* c, uint32_t ms)
{
    if (c->rfTimerRunning)
        c->rfTimer += ms;

    if (c->netState != NXDN_NET_IDLE)
        c->netTimer += ms;

    if (c->netState == NXDN_NET_AUDIO || c->netState == NXDN_NET_DATA)
    {
        if (c->netWatchdogRunning)
        {
            c->netWatchdog += ms;

            if (c->netWatchdog >= c->netWatchdogMs)
            {
                emit(c, NXDN_EV_NET_LOST, c->netLayer3, c->netState == NXDN_NET_DATA, (float)c->netFrames / 12.5F, 0.0F);

                /* Close the transmission on the air */
                netCloseOnAir(c);

                endNet(c);
            }
        }
    }
}

bool nxdnCtrlBusy(const NxdnCtrl* c)
{
    return c->rfState != NXDN_RF_LISTENING || c->netState != NXDN_NET_IDLE;
}

void nxdnCtrlReset(NxdnCtrl* c)
{
    endRf(c);
    endNet(c);
}
