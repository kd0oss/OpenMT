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
 *   along with this program; if not, see <http://www.gnu.org/licenses/>   *
 *                                                                         *
 *   Some functions based on or inspired by MMDVM Jonathan Naylor G4KLX    *
 ***************************************************************************/

#include "dmr_func.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "dsp_tools.h"

SLOTDATA slotData[2];

// The main decode function
void BPTC19696_decode(const unsigned char* in, unsigned char* out, const uint8_t slot)
{
    assert(in != NULL);
    assert(out != NULL);

    //  Get the raw binary
    BPTC19696_decodeExtractBinary(in, slot);

    // Deinterleave
    BPTC19696_decodeDeInterleave(slot);

    // Error check
    BPTC19696_decodeErrorCheck(slot);

    // Extract Data
    BPTC19696_decodeExtractData(out, slot);
}

// The main encode function
void BPTC19696_encode(const unsigned char* in, unsigned char* out, const uint8_t slot)
{
    assert(in != NULL);
    assert(out != NULL);

    // Extract Data
    BPTC19696_encodeExtractData(in, slot);

    // Error check
    BPTC19696_encodeErrorCheck(slot);

    // Deinterleave
    BPTC19696_encodeInterleave(slot);

    //  Get the raw binary
    BPTC19696_encodeExtractBinary(out, slot);
}

void BPTC19696_decodeExtractBinary(const unsigned char* in, const uint8_t slot)
{
    // First block
    byteToBitsBE(in[0U], slotData[slot].rawData + 0U);
    byteToBitsBE(in[1U], slotData[slot].rawData + 8U);
    byteToBitsBE(in[2U], slotData[slot].rawData + 16U);
    byteToBitsBE(in[3U], slotData[slot].rawData + 24U);
    byteToBitsBE(in[4U], slotData[slot].rawData + 32U);
    byteToBitsBE(in[5U], slotData[slot].rawData + 40U);
    byteToBitsBE(in[6U], slotData[slot].rawData + 48U);
    byteToBitsBE(in[7U], slotData[slot].rawData + 56U);
    byteToBitsBE(in[8U], slotData[slot].rawData + 64U);
    byteToBitsBE(in[9U], slotData[slot].rawData + 72U);
    byteToBitsBE(in[10U], slotData[slot].rawData + 80U);
    byteToBitsBE(in[11U], slotData[slot].rawData + 88U);
    byteToBitsBE(in[12U], slotData[slot].rawData + 96U);

    // Handle the two bits
    bool bits[8U];
    byteToBitsBE(in[20U], bits);
    slotData[slot].rawData[98U] = bits[6U];
    slotData[slot].rawData[99U] = bits[7U];

    // Second block
    byteToBitsBE(in[21U], slotData[slot].rawData + 100U);
    byteToBitsBE(in[22U], slotData[slot].rawData + 108U);
    byteToBitsBE(in[23U], slotData[slot].rawData + 116U);
    byteToBitsBE(in[24U], slotData[slot].rawData + 124U);
    byteToBitsBE(in[25U], slotData[slot].rawData + 132U);
    byteToBitsBE(in[26U], slotData[slot].rawData + 140U);
    byteToBitsBE(in[27U], slotData[slot].rawData + 148U);
    byteToBitsBE(in[28U], slotData[slot].rawData + 156U);
    byteToBitsBE(in[29U], slotData[slot].rawData + 164U);
    byteToBitsBE(in[30U], slotData[slot].rawData + 172U);
    byteToBitsBE(in[31U], slotData[slot].rawData + 180U);
    byteToBitsBE(in[32U], slotData[slot].rawData + 188U);
}

// Deinterleave the raw data
void BPTC19696_decodeDeInterleave(const uint8_t slot)
{
    for (unsigned int i = 0U; i < 196U; i++)
        slotData[slot].deInterData[i] = false;

    // The first bit is slotData[slot].R(3) which is not used so can be ignored
    for (unsigned int a = 0U; a < 196U; a++)
    {
        // Calculate the interleave sequence
        unsigned int interleaveSequence = (a * 181U) % 196U;
        // Shuffle the data
        slotData[slot].deInterData[a] = slotData[slot].rawData[interleaveSequence];
    }
}

// Check each row with a Hamming (15,11,3) code and each column with a Hamming (13,9,3) code
void BPTC19696_decodeErrorCheck(const uint8_t slot)
{
    bool fixing;
    unsigned int count = 0U;
    do
    {
        fixing = false;

        // slotData[slot].Run through each of the 15 columns
        bool col[13U];
        for (unsigned int c = 0U; c < 15U; c++)
        {
            unsigned int pos = c + 1U;
            for (unsigned int a = 0U; a < 13U; a++)
            {
                col[a] = slotData[slot].deInterData[pos];
                pos    = pos + 15U;
            }

            if (hamming_decode1393(col))
            {
                unsigned int pos = c + 1U;
                for (unsigned int a = 0U; a < 13U; a++)
                {
                    slotData[slot].deInterData[pos] = col[a];
                    pos              = pos + 15U;
                }

                fixing = true;
            }
        }

        // slotData[slot].Run through each of the 9 rows containing data
        for (unsigned int r = 0U; r < 9U; r++)
        {
            unsigned int pos = (r * 15U) + 1U;
            if (hamming_decode15113_2(slotData[slot].deInterData + pos))
                fixing = true;
        }

        count++;
    } while (fixing && count < 5U);
}

// Extract the 96 bits of payload
void BPTC19696_decodeExtractData(unsigned char* data, const uint8_t slot)
{
    bool bData[96U];
    unsigned int pos = 0U;
    for (unsigned int a = 4U; a <= 11U; a++, pos++)
        bData[pos] = slotData[slot].deInterData[a];

    for (unsigned int a = 16U; a <= 26U; a++, pos++)
        bData[pos] = slotData[slot].deInterData[a];

    for (unsigned int a = 31U; a <= 41U; a++, pos++)
        bData[pos] = slotData[slot].deInterData[a];

    for (unsigned int a = 46U; a <= 56U; a++, pos++)
        bData[pos] = slotData[slot].deInterData[a];

    for (unsigned int a = 61U; a <= 71U; a++, pos++)
        bData[pos] = slotData[slot].deInterData[a];

    for (unsigned int a = 76U; a <= 86U; a++, pos++)
        bData[pos] = slotData[slot].deInterData[a];

    for (unsigned int a = 91U; a <= 101U; a++, pos++)
        bData[pos] = slotData[slot].deInterData[a];

    for (unsigned int a = 106U; a <= 116U; a++, pos++)
        bData[pos] = slotData[slot].deInterData[a];

    for (unsigned int a = 121U; a <= 131U; a++, pos++)
        bData[pos] = slotData[slot].deInterData[a];

    bitsToByteBE(bData + 0U, &data[0U]);
    bitsToByteBE(bData + 8U, &data[1U]);
    bitsToByteBE(bData + 16U, &data[2U]);
    bitsToByteBE(bData + 24U, &data[3U]);
    bitsToByteBE(bData + 32U, &data[4U]);
    bitsToByteBE(bData + 40U, &data[5U]);
    bitsToByteBE(bData + 48U, &data[6U]);
    bitsToByteBE(bData + 56U, &data[7U]);
    bitsToByteBE(bData + 64U, &data[8U]);
    bitsToByteBE(bData + 72U, &data[9U]);
    bitsToByteBE(bData + 80U, &data[10U]);
    bitsToByteBE(bData + 88U, &data[11U]);
}

// Extract the 96 bits of payload
void BPTC19696_encodeExtractData(const unsigned char* in, const uint8_t slot)
{
    bool bData[96U];
    byteToBitsBE(in[0U], bData + 0U);
    byteToBitsBE(in[1U], bData + 8U);
    byteToBitsBE(in[2U], bData + 16U);
    byteToBitsBE(in[3U], bData + 24U);
    byteToBitsBE(in[4U], bData + 32U);
    byteToBitsBE(in[5U], bData + 40U);
    byteToBitsBE(in[6U], bData + 48U);
    byteToBitsBE(in[7U], bData + 56U);
    byteToBitsBE(in[8U], bData + 64U);
    byteToBitsBE(in[9U], bData + 72U);
    byteToBitsBE(in[10U], bData + 80U);
    byteToBitsBE(in[11U], bData + 88U);

    for (unsigned int i = 0U; i < 196U; i++)
        slotData[slot].deInterData[i] = false;

    unsigned int pos = 0U;
    for (unsigned int a = 4U; a <= 11U; a++, pos++)
        slotData[slot].deInterData[a] = bData[pos];

    for (unsigned int a = 16U; a <= 26U; a++, pos++)
        slotData[slot].deInterData[a] = bData[pos];

    for (unsigned int a = 31U; a <= 41U; a++, pos++)
        slotData[slot].deInterData[a] = bData[pos];

    for (unsigned int a = 46U; a <= 56U; a++, pos++)
        slotData[slot].deInterData[a] = bData[pos];

    for (unsigned int a = 61U; a <= 71U; a++, pos++)
        slotData[slot].deInterData[a] = bData[pos];

    for (unsigned int a = 76U; a <= 86U; a++, pos++)
        slotData[slot].deInterData[a] = bData[pos];

    for (unsigned int a = 91U; a <= 101U; a++, pos++)
        slotData[slot].deInterData[a] = bData[pos];

    for (unsigned int a = 106U; a <= 116U; a++, pos++)
        slotData[slot].deInterData[a] = bData[pos];

    for (unsigned int a = 121U; a <= 131U; a++, pos++)
        slotData[slot].deInterData[a] = bData[pos];
}

// Check each row with a Hamming (15,11,3) code and each column with a Hamming (13,9,3) code
void BPTC19696_encodeErrorCheck(const uint8_t slot)
{
    // slotData[slot].Run through each of the 9 rows containing data
    for (unsigned int r = 0U; r < 9U; r++)
    {
        unsigned int pos = (r * 15U) + 1U;
        hamming_encode15113_2(slotData[slot].deInterData + pos);
    }

    // slotData[slot].Run through each of the 15 columns
    bool col[13U];
    for (unsigned int c = 0U; c < 15U; c++)
    {
        unsigned int pos = c + 1U;
        for (unsigned int a = 0U; a < 13U; a++)
        {
            col[a] = slotData[slot].deInterData[pos];
            pos    = pos + 15U;
        }

        hamming_encode1393(col);

        pos = c + 1U;
        for (unsigned int a = 0U; a < 13U; a++)
        {
            slotData[slot].deInterData[pos] = col[a];
            pos              = pos + 15U;
        }
    }
}

// Interleave the raw data
void BPTC19696_encodeInterleave(const uint8_t slot)
{
    for (unsigned int i = 0U; i < 196U; i++)
        slotData[slot].rawData[i] = false;

    // The first bit is slotData[slot].R(3) which is not used so can be ignored
    for (unsigned int a = 0U; a < 196U; a++)
    {
        // Calculate the interleave sequence
        unsigned int interleaveSequence = (a * 181U) % 196U;
        // Unshuffle the data
        slotData[slot].rawData[interleaveSequence] = slotData[slot].deInterData[a];
    }
}

void BPTC19696_encodeExtractBinary(unsigned char* data, const uint8_t slot)
{
    // First block
    bitsToByteBE(slotData[slot].rawData + 0U, &data[0U]);
    bitsToByteBE(slotData[slot].rawData + 8U, &data[1U]);
    bitsToByteBE(slotData[slot].rawData + 16U, &data[2U]);
    bitsToByteBE(slotData[slot].rawData + 24U, &data[3U]);
    bitsToByteBE(slotData[slot].rawData + 32U, &data[4U]);
    bitsToByteBE(slotData[slot].rawData + 40U, &data[5U]);
    bitsToByteBE(slotData[slot].rawData + 48U, &data[6U]);
    bitsToByteBE(slotData[slot].rawData + 56U, &data[7U]);
    bitsToByteBE(slotData[slot].rawData + 64U, &data[8U]);
    bitsToByteBE(slotData[slot].rawData + 72U, &data[9U]);
    bitsToByteBE(slotData[slot].rawData + 80U, &data[10U]);
    bitsToByteBE(slotData[slot].rawData + 88U, &data[11U]);

    // Handle the two bits
    unsigned char byte;
    bitsToByteBE(slotData[slot].rawData + 96U, &byte);
    data[12U] = (data[12U] & 0x3FU) | ((byte >> 0) & 0xC0U);
    data[20U] = (data[20U] & 0xFCU) | ((byte >> 4) & 0x03U);

    // Second block
    bitsToByteBE(slotData[slot].rawData + 100U, &data[21U]);
    bitsToByteBE(slotData[slot].rawData + 108U, &data[22U]);
    bitsToByteBE(slotData[slot].rawData + 116U, &data[23U]);
    bitsToByteBE(slotData[slot].rawData + 124U, &data[24U]);
    bitsToByteBE(slotData[slot].rawData + 132U, &data[25U]);
    bitsToByteBE(slotData[slot].rawData + 140U, &data[26U]);
    bitsToByteBE(slotData[slot].rawData + 148U, &data[27U]);
    bitsToByteBE(slotData[slot].rawData + 156U, &data[28U]);
    bitsToByteBE(slotData[slot].rawData + 164U, &data[29U]);
    bitsToByteBE(slotData[slot].rawData + 172U, &data[30U]);
    bitsToByteBE(slotData[slot].rawData + 180U, &data[31U]);
    bitsToByteBE(slotData[slot].rawData + 188U, &data[32U]);
}

bool CSBK_decode(const unsigned char* bytes, const uint8_t slot)
{
    assert(bytes != NULL);

    BPTC19696_decode(bytes, slotData[slot].tdata, slot);

    slotData[slot].tdata[10U] ^= CSBK_CRC_MASK[0U];
    slotData[slot].tdata[11U] ^= CSBK_CRC_MASK[1U];

    bool valid = CRC_checkCCITT162(slotData[slot].tdata, 12U);
    if (!valid)
        return false;

    // slotData[slot].Restore the checksum
    slotData[slot].tdata[10U] ^= CSBK_CRC_MASK[0U];
    slotData[slot].tdata[11U] ^= CSBK_CRC_MASK[1U];

    slotData[slot].CSBK = slotData[slot].tdata[0U] & 0x3FU;
    slotData[slot].FID  = slotData[slot].tdata[1U];

    switch (slotData[slot].CSBK)
    {
        case CSBKO_BSDWNACT:
            slotData[slot].GI         = false;
            slotData[slot].bsId       = slotData[slot].tdata[4U] << 16 | slotData[slot].tdata[5U] << 8 | slotData[slot].tdata[6U];
            slotData[slot].srcId      = slotData[slot].tdata[7U] << 16 | slotData[slot].tdata[8U] << 8 | slotData[slot].tdata[9U];
            slotData[slot].dataContent = false;
            slotData[slot].CBF        = 0U;
            // dump(1U, "Downlink Activate slotData[slot].CSBK", slotData[slot].tdata, 12U);
            break;

        case CSBKO_UUVREQ:
            slotData[slot].GI         = false;
            slotData[slot].dstId      = slotData[slot].tdata[4U] << 16 | slotData[slot].tdata[5U] << 8 | slotData[slot].tdata[6U];
            slotData[slot].srcId      = slotData[slot].tdata[7U] << 16 | slotData[slot].tdata[8U] << 8 | slotData[slot].tdata[9U];
            slotData[slot].dataContent = false;
            slotData[slot].CBF        = 0U;
            slotData[slot].OVCM       = (slotData[slot].tdata[2U] & 0x04U) == 0x04U;
            // dump(1U, "Unit to Unit Service slotData[slot].Request slotData[slot].CSBK", slotData[slot].tdata, 12U);
            break;

        case CSBKO_UUANSRSP:
            slotData[slot].GI         = false;
            slotData[slot].dstId      = slotData[slot].tdata[4U] << 16 | slotData[slot].tdata[5U] << 8 | slotData[slot].tdata[6U];
            slotData[slot].srcId      = slotData[slot].tdata[7U] << 16 | slotData[slot].tdata[8U] << 8 | slotData[slot].tdata[9U];
            slotData[slot].dataContent = false;
            slotData[slot].CBF        = 0U;
            slotData[slot].OVCM       = (slotData[slot].tdata[2U] & 0x04U) == 0x04U;
            // dump(1U, "Unit to Unit Service Answer slotData[slot].Response slotData[slot].CSBK", slotData[slot].tdata, 12U);
            break;

        case CSBKO_PRECCSBK:
            slotData[slot].GI         = (slotData[slot].tdata[2U] & 0x40U) == 0x40U;
            slotData[slot].dstId      = slotData[slot].tdata[4U] << 16 | slotData[slot].tdata[5U] << 8 | slotData[slot].tdata[6U];
            slotData[slot].srcId      = slotData[slot].tdata[7U] << 16 | slotData[slot].tdata[8U] << 8 | slotData[slot].tdata[9U];
            slotData[slot].dataContent = (slotData[slot].tdata[2U] & 0x80U) == 0x80U;
            slotData[slot].CBF        = slotData[slot].tdata[3U];
            // dump(1U, "Preamble slotData[slot].CSBK", slotData[slot].tdata, 12U);
            break;

        case CSBKO_NACKRSP:
            slotData[slot].GI         = false;
            slotData[slot].srcId      = slotData[slot].tdata[4U] << 16 | slotData[slot].tdata[5U] << 8 | slotData[slot].tdata[6U];
            slotData[slot].dstId      = slotData[slot].tdata[7U] << 16 | slotData[slot].tdata[8U] << 8 | slotData[slot].tdata[9U];
            slotData[slot].dataContent = false;
            slotData[slot].CBF        = 0U;
            // dump(1U, "Negative Acknowledge slotData[slot].Response slotData[slot].CSBK", slotData[slot].tdata, 12U);
            break;

        case CSBKO_CALL_ALERT:
            slotData[slot].GI         = false;
            slotData[slot].dstId      = slotData[slot].tdata[4U] << 16 | slotData[slot].tdata[5U] << 8 | slotData[slot].tdata[6U];
            slotData[slot].srcId      = slotData[slot].tdata[7U] << 16 | slotData[slot].tdata[8U] << 8 | slotData[slot].tdata[9U];
            slotData[slot].dataContent = false;
            slotData[slot].CBF        = 0U;
            // dump(1U, "Call Alert slotData[slot].CSBK", slotData[slot].tdata, 12U);
            break;

        case CSBKO_CALL_ALERT_ACK:
            slotData[slot].GI         = false;
            slotData[slot].dstId      = slotData[slot].tdata[4U] << 16 | slotData[slot].tdata[5U] << 8 | slotData[slot].tdata[6U];
            slotData[slot].srcId      = slotData[slot].tdata[7U] << 16 | slotData[slot].tdata[8U] << 8 | slotData[slot].tdata[9U];
            slotData[slot].dataContent = false;
            slotData[slot].CBF        = 0U;
            // dump(1U, "Call Alert Ack slotData[slot].CSBK", slotData[slot].tdata, 12U);
            break;

        case CSBKO_RADIO_CHECK:
            slotData[slot].GI = false;
            if (slotData[slot].tdata[3U] == 0x80)
            {
                slotData[slot].dstId = slotData[slot].tdata[4U] << 16 | slotData[slot].tdata[5U] << 8 | slotData[slot].tdata[6U];
                slotData[slot].srcId = slotData[slot].tdata[7U] << 16 | slotData[slot].tdata[8U] << 8 | slotData[slot].tdata[9U];
                // dump(1U, "slotData[slot].Radio Check slotData[slot].Req slotData[slot].CSBK", slotData[slot].tdata, 12U);
            }
            else
            {
                slotData[slot].srcId = slotData[slot].tdata[4U] << 16 | slotData[slot].tdata[5U] << 8 | slotData[slot].tdata[6U];
                slotData[slot].dstId = slotData[slot].tdata[7U] << 16 | slotData[slot].tdata[8U] << 8 | slotData[slot].tdata[9U];
                // dump(1U, "slotData[slot].Radio Check Ack slotData[slot].CSBK", slotData[slot].tdata, 12U);
            }
            slotData[slot].dataContent = false;
            slotData[slot].CBF        = 0U;
            break;

        case CSBKO_CALL_EMERGENCY:
            slotData[slot].GI         = true;
            slotData[slot].dstId      = slotData[slot].tdata[4U] << 16 | slotData[slot].tdata[5U] << 8 | slotData[slot].tdata[6U];
            slotData[slot].srcId      = slotData[slot].tdata[7U] << 16 | slotData[slot].tdata[8U] << 8 | slotData[slot].tdata[9U];
            slotData[slot].dataContent = false;
            slotData[slot].CBF        = 0U;
            // dump(1U, "Call Emergency slotData[slot].CSBK", slotData[slot].tdata, 12U);
            break;

        default:
            slotData[slot].GI         = false;
            slotData[slot].srcId      = 0U;
            slotData[slot].dstId      = 0U;
            slotData[slot].dataContent = false;
            slotData[slot].CBF        = 0U;
            // dump("Unhandled slotData[slot].CSBK type", slotData[slot].tdata, 12U);
            return true;
    }

    return true;
}

void CSBK_encode(unsigned char* bytes, const uint8_t slot)
{
    assert(bytes != NULL);

    slotData[slot].tdata[10U] ^= CSBK_CRC_MASK[0U];
    slotData[slot].tdata[11U] ^= CSBK_CRC_MASK[1U];

    CRC_addCCITT162(slotData[slot].tdata, 12U);

    slotData[slot].tdata[10U] ^= CSBK_CRC_MASK[0U];
    slotData[slot].tdata[11U] ^= CSBK_CRC_MASK[1U];

    BPTC19696_encode(slotData[slot].tdata, bytes, slot);
}

unsigned int CSBK_getCSBKO(const uint8_t slot)
{
    return slotData[slot].CSBK;
}

unsigned char CSBK_getFID(const uint8_t slot)
{
    return slotData[slot].FID;
}

bool CSBK_getOVCM(const uint8_t slot)
{
    return slotData[slot].OVCM;
}

void CSBK_setOVCM(bool ovcm, const uint8_t slot)
{
    if (slotData[slot].CSBK == CSBKO_UUVREQ || slotData[slot].CSBK == CSBKO_UUANSRSP)
    {
        slotData[slot].OVCM = ovcm;

        if (ovcm)
            slotData[slot].tdata[2U] |= 0x04U;
        else
            slotData[slot].tdata[2U] &= 0xFBU;
    }
}

bool CSBK_getGI(const uint8_t slot)
{
    return slotData[slot].GI;
}

unsigned int CSBK_getBSId(const uint8_t slot)
{
    return slotData[slot].bsId;
}

unsigned int CSBK_getSrcId(const uint8_t slot)
{
    return slotData[slot].srcId;
}

unsigned int CSBK_getDstId(const uint8_t slot)
{
    return slotData[slot].dstId;
}

bool CSBK_getDataContent(const uint8_t slot)
{
    return slotData[slot].dataContent;
}

unsigned char CSBK_getCBF(const uint8_t slot)
{
    return slotData[slot].CBF;
}

void CSBK_setCBF(unsigned char cbf, const uint8_t slot)
{
    slotData[slot].CBF = slotData[slot].tdata[3U] = cbf;
}

bool DMRFullLC_decode(const unsigned char* data, unsigned char type, unsigned char* lcData, const uint8_t slot)
{
    assert(data != NULL);
    assert(lcData != NULL);

    //	unsigned char lcData[12U];
    BPTC19696_decode(data, lcData, slot);

    switch (type)
    {
        case DT_VOICE_LC_HEADER:
            lcData[9U] ^= VOICE_LC_HEADER_CRC_MASK[0U];
            lcData[10U] ^= VOICE_LC_HEADER_CRC_MASK[1U];
            lcData[11U] ^= VOICE_LC_HEADER_CRC_MASK[2U];
            break;

        case DT_TERMINATOR_WITH_LC:
            lcData[9U] ^= TERMINATOR_WITH_LC_CRC_MASK[0U];
            lcData[10U] ^= TERMINATOR_WITH_LC_CRC_MASK[1U];
            lcData[11U] ^= TERMINATOR_WITH_LC_CRC_MASK[2U];
            break;

        default:
            //	::LogError("Unsupported LC type - %d", int(type));
            return false;
    }

    if (!RS129_check(lcData))
        return false;

    return true;
}

void DMRFullLC_encode(unsigned char* data, unsigned char type, const uint8_t slot)
{
    assert(data != NULL);

    unsigned char lcData[12U];
    DMRLC_encodeBytes(lcData, slot);

    unsigned char parity[4U];
    RS129_encode(lcData, 9U, parity);

    switch (type)
    {
        case DT_VOICE_LC_HEADER:
            lcData[9U]  = parity[2U] ^ VOICE_LC_HEADER_CRC_MASK[0U];
            lcData[10U] = parity[1U] ^ VOICE_LC_HEADER_CRC_MASK[1U];
            lcData[11U] = parity[0U] ^ VOICE_LC_HEADER_CRC_MASK[2U];
            break;

        case DT_TERMINATOR_WITH_LC:
            lcData[9U]  = parity[2U] ^ TERMINATOR_WITH_LC_CRC_MASK[0U];
            lcData[10U] = parity[1U] ^ TERMINATOR_WITH_LC_CRC_MASK[1U];
            lcData[11U] = parity[0U] ^ TERMINATOR_WITH_LC_CRC_MASK[2U];
            break;

        default:
            //	::LogError("Unsupported LC type - %d", int(type));
            return;
    }

    BPTC19696_encode(lcData, data, slot);
}

void DMRLC_decode(const unsigned char* bytes, uint8_t slot)
{
	assert(bytes != NULL);

	slotData[slot].PF = (bytes[0U] & 0x80U) == 0x80U;
	slotData[slot].R  = (bytes[0U] & 0x40U) == 0x40U;

	slotData[slot].FLCO = bytes[0U] & 0x3FU;

	slotData[slot].FID = bytes[1U];

	slotData[slot].options = bytes[2U];

	slotData[slot].dstId = bytes[3U] << 16 | bytes[4U] << 8 | bytes[5U];
	slotData[slot].srcId = bytes[6U] << 16 | bytes[7U] << 8 | bytes[8U];
}

void DMRLC_encodeBytes(unsigned char* bytes, const uint8_t slot)
{
    assert(bytes != NULL);

    bytes[0U] = (unsigned char)slotData[slot].FLCO;

    if (slotData[slot].PF)
        bytes[0U] |= 0x80U;

    if (slotData[slot].R)
        bytes[0U] |= 0x40U;

    bytes[1U] = slotData[slot].FID;

    bytes[2U] = slotData[slot].options;

    bytes[3U] = slotData[slot].dstId >> 16;
    bytes[4U] = slotData[slot].dstId >> 8;
    bytes[5U] = slotData[slot].dstId >> 0;

    bytes[6U] = slotData[slot].srcId >> 16;
    bytes[7U] = slotData[slot].srcId >> 8;
    bytes[8U] = slotData[slot].srcId >> 0;
}

void DMRLC_encodeBits(bool* bits, const uint8_t slot)
{
    assert(bits != NULL);

    unsigned char bytes[9U];
    DMRLC_encodeBytes(bytes, slot);

    byteToBitsBE(bytes[0U], bits + 0U);
    byteToBitsBE(bytes[1U], bits + 8U);
    byteToBitsBE(bytes[2U], bits + 16U);
    byteToBitsBE(bytes[3U], bits + 24U);
    byteToBitsBE(bytes[4U], bits + 32U);
    byteToBitsBE(bytes[5U], bits + 40U);
    byteToBitsBE(bytes[6U], bits + 48U);
    byteToBitsBE(bytes[7U], bits + 56U);
    byteToBitsBE(bytes[8U], bits + 64U);
}

bool DMRLC_getPF(const uint8_t slot)
{
    return slotData[slot].PF;
}

void DMRLC_setPF(bool pf, const uint8_t slot)
{
    slotData[slot].PF = pf;
}

unsigned int DMRLC_getFLCO(const uint8_t slot)
{
    return slotData[slot].FLCO;
}

void DMRLC_setFLCO(unsigned int flco, const uint8_t slot)
{
    slotData[slot].FLCO = flco;
}

unsigned char DMRLC_getFID(const uint8_t slot)
{
    return slotData[slot].FID;
}

void DMRLC_setFID(unsigned char fid, const uint8_t slot)
{
    slotData[slot].FID = fid;
}

bool DMRLC_getOVCM(const uint8_t slot)
{
    return (slotData[slot].options & 0x04U) == 0x04U;
}

void DMRLC_setOVCM(bool ovcm, const uint8_t slot)
{
    if (ovcm)
        slotData[slot].options |= 0x04U;
    else
        slotData[slot].options &= 0xFBU;
}

unsigned int DMRLC_getSrcId(const uint8_t slot)
{
    return slotData[slot].srcId;
}

void DMRLC_setSrcId(unsigned int id, const uint8_t slot)
{
    slotData[slot].srcId = id;
}

unsigned int DMRLC_getDstId(const uint8_t slot)
{
    return slotData[slot].dstId;
}

void DMRLC_setDstId(unsigned int id, const uint8_t slot)
{
    slotData[slot].dstId = id;
}

void addDMRAudioSync(unsigned char* data, bool duplex)
{
    assert(data != NULL);

    if (duplex)
    {
        for (unsigned int i = 0U; i < 7U; i++)
            data[i + 13U] = (data[i + 13U] & ~SYNC_MASK[i]) | BS_SOURCED_AUDIO_SYNC[i];
    }
    else
    {
        for (unsigned int i = 0U; i < 7U; i++)
            data[i + 13U] = (data[i + 13U] & ~SYNC_MASK[i]) | MS_SOURCED_AUDIO_SYNC[i];
    }
}

void addDMRDataSync(unsigned char* data, bool duplex)
{
    assert(data != NULL);

    if (duplex)
    {
        for (unsigned int i = 0U; i < 7U; i++)
            data[i + 13U] = (data[i + 13U] & ~SYNC_MASK[i]) | BS_SOURCED_DATA_SYNC[i];
    }
    else
    {
        for (unsigned int i = 0U; i < 7U; i++)
            data[i + 13U] = (data[i + 13U] & ~SYNC_MASK[i]) | MS_SOURCED_DATA_SYNC[i];
    }
}

void DMREMB_decode(const unsigned char* data, const uint8_t slot)
{
    assert(data != NULL);

    unsigned char DMREMB[2U];
    DMREMB[0U] = (data[13U] << 4) & 0xF0U;
    DMREMB[0U] |= (data[14U] >> 4) & 0x0FU;
    DMREMB[1U] = (data[18U] << 4) & 0xF0U;
    DMREMB[1U] |= (data[19U] >> 4) & 0x0FU;

    unsigned char code = QR1676_decode(DMREMB);

    slotData[slot].colorCode = (code >> 4) & 0x0FU;
    slotData[slot].piFlag    = (code & 0x08U) == 0x08U;
    slotData[slot].LCSS      = (code >> 1) & 0x03U;
}

void DMREMB_encode(unsigned char* data, const uint8_t slot)
{
    assert(data != NULL);

    unsigned char DMREMB[2U];
    DMREMB[0U] = (slotData[slot].colorCode << 4) & 0xF0U;
    DMREMB[0U] |= slotData[slot].piFlag ? 0x08U : 0x00U;
    DMREMB[0U] |= (slotData[slot].LCSS << 1) & 0x06U;
    DMREMB[1U] = 0x00U;

    QR1676_encode(DMREMB);

    data[13U] = (data[13U] & 0xF0U) | ((DMREMB[0U] >> 4U) & 0x0FU);
    data[14U] = (data[14U] & 0x0FU) | ((DMREMB[0U] << 4U) & 0xF0U);
    data[18U] = (data[18U] & 0xF0U) | ((DMREMB[1U] >> 4U) & 0x0FU);
    data[19U] = (data[19U] & 0x0FU) | ((DMREMB[1U] << 4U) & 0xF0U);
}

void DMREmbeddedData_encodeEmbeddedData(const uint8_t slot)
{
    unsigned int crc;
    CRC_encodeFiveBit(slotData[slot].EMB_data, &crc);

    bool data[128U];
    memset(data, 0x00U, 128U * sizeof(bool));

    data[106U] = (crc & 0x01U) == 0x01U;
    data[90U]  = (crc & 0x02U) == 0x02U;
    data[74U]  = (crc & 0x04U) == 0x04U;
    data[58U]  = (crc & 0x08U) == 0x08U;
    data[42U]  = (crc & 0x10U) == 0x10U;

    unsigned int b = 0U;
    for (unsigned int a = 0U; a < 11U; a++, b++)
        data[a] = slotData[slot].EMB_data[b];
    for (unsigned int a = 16U; a < 27U; a++, b++)
        data[a] = slotData[slot].EMB_data[b];
    for (unsigned int a = 32U; a < 42U; a++, b++)
        data[a] = slotData[slot].EMB_data[b];
    for (unsigned int a = 48U; a < 58U; a++, b++)
        data[a] = slotData[slot].EMB_data[b];
    for (unsigned int a = 64U; a < 74U; a++, b++)
        data[a] = slotData[slot].EMB_data[b];
    for (unsigned int a = 80U; a < 90U; a++, b++)
        data[a] = slotData[slot].EMB_data[b];
    for (unsigned int a = 96U; a < 106U; a++, b++)
        data[a] = slotData[slot].EMB_data[b];

    // Hamming (16,11,4) check each row except the last one
    for (unsigned int a = 0U; a < 112U; a += 16U)
        hamming_encode16114(data + a);

    // Add the parity bits for each column
    for (unsigned int a = 0U; a < 16U; a++)
        data[a + 112U] = data[a + 0U] ^ data[a + 16U] ^ data[a + 32U] ^ data[a + 48U] ^ data[a + 64U] ^ data[a + 80U] ^ data[a + 96U];

    // The data is packed downwards in columns
    b = 0U;
    for (unsigned int a = 0U; a < 128U; a++)
    {
        slotData[slot].EMB_raw[a] = data[b];
        b += 16U;
        if (b > 127U)
            b -= 127U;
    }
}

// Unpack and error check an embedded LC
void DMREmbeddedData_decodeEmbeddedData(const uint8_t slot)
{
    // The data is unpacked downwards in columns
    bool data[128U];
    memset(data, 0x00U, 128U * sizeof(bool));

    unsigned int b = 0U;
    for (unsigned int a = 0U; a < 128U; a++)
    {
        data[b] = slotData[slot].EMB_raw[a];
        b += 16U;
        if (b > 127U)
            b -= 127U;
    }

    // Hamming (16,11,4) check each row except the last one
    for (unsigned int a = 0U; a < 112U; a += 16U)
    {
        if (!hamming_decode16114(data + a))
            return;
    }

    // Check the parity bits
    for (unsigned int a = 0U; a < 16U; a++)
    {
        bool parity = data[a + 0U] ^ data[a + 16U] ^ data[a + 32U] ^ data[a + 48U] ^ data[a + 64U] ^ data[a + 80U] ^ data[a + 96U] ^ data[a + 112U];
        if (parity)
            return;
    }

    // We have passed the Hamming check so extract the actual payload
    b = 0U;
    for (unsigned int a = 0U; a < 11U; a++, b++)
        slotData[slot].EMB_data[b] = data[a];
    for (unsigned int a = 16U; a < 27U; a++, b++)
        slotData[slot].EMB_data[b] = data[a];
    for (unsigned int a = 32U; a < 42U; a++, b++)
        slotData[slot].EMB_data[b] = data[a];
    for (unsigned int a = 48U; a < 58U; a++, b++)
        slotData[slot].EMB_data[b] = data[a];
    for (unsigned int a = 64U; a < 74U; a++, b++)
        slotData[slot].EMB_data[b] = data[a];
    for (unsigned int a = 80U; a < 90U; a++, b++)
        slotData[slot].EMB_data[b] = data[a];
    for (unsigned int a = 96U; a < 106U; a++, b++)
        slotData[slot].EMB_data[b] = data[a];

    // Extract the 5 bit CRC
    unsigned int crc = 0U;
    if (data[42]) crc += 16U;
    if (data[58]) crc += 8U;
    if (data[74]) crc += 4U;
    if (data[90]) crc += 2U;
    if (data[106]) crc += 1U;

    // Now CRC check this
    if (!CRC_checkFiveBit(slotData[slot].EMB_data, crc))
        return;

    slotData[slot].FLCO_valid = true;

    // Extract the FLCO
    unsigned char flco;
    bitsToByteBE(data + 0U, &flco);
    slotData[slot].FLCO = flco & 0x3FU;
}

// Add LC data (which may consist of 4 blocks) to the data store
bool DMREmbeddedData_decode(const unsigned char* data, unsigned char lcss, const uint8_t slot)
{
    assert(data != NULL);
    static LC_STATE state[2];

    bool rawData[40U];
    byteToBitsBE(data[14U], rawData + 0U);
    byteToBitsBE(data[15U], rawData + 8U);
    byteToBitsBE(data[16U], rawData + 16U);
    byteToBitsBE(data[17U], rawData + 24U);
    byteToBitsBE(data[18U], rawData + 32U);

    // Is this the first block of a 4 block embedded LC ?
    if (lcss == 1U)
    {
        for (unsigned int a = 0U; a < 32U; a++)
            slotData[slot].EMB_raw[a] = rawData[a + 4U];

        // Show we are ready for the next LC block
        state[slot] = LCS_FIRST;
        slotData[slot].FLCO_valid = false;

        return false;
    }

    // Is this the 2nd block of a 4 block embedded LC ?
    if (lcss == 3U && state[slot] == LCS_FIRST)
    {
        for (unsigned int a = 0U; a < 32U; a++)
            slotData[slot].EMB_raw[a + 32U] = rawData[a + 4U];

        // Show we are ready for the next LC block
        state[slot] = LCS_SECOND;

        return false;
    }

    // Is this the 3rd block of a 4 block embedded LC ?
    if (lcss == 3U && state[slot] == LCS_SECOND)
    {
        for (unsigned int a = 0U; a < 32U; a++)
            slotData[slot].EMB_raw[a + 64U] = rawData[a + 4U];

        // Show we are ready for the final LC block
        state[slot] = LCS_THIRD;

        return false;
    }

    // Is this the final block of a 4 block embedded LC ?
    if (lcss == 2U && state[slot] == LCS_THIRD)
    {
        for (unsigned int a = 0U; a < 32U; a++)
            slotData[slot].EMB_raw[a + 96U] = rawData[a + 4U];

        // Show that we're not ready for any more data
        state[slot] = LCS_NONE;

        // Process the complete data block
        DMREmbeddedData_decodeEmbeddedData(slot);
        if (slotData[slot].FLCO_valid)
            DMREmbeddedData_encodeEmbeddedData(slot);

        return slotData[slot].FLCO_valid;
    }

    return false;
}

bool DMREmbeddedData_getRawData(unsigned char* data, const uint8_t slot)
{
    assert(data != NULL);

    if (!slotData[slot].FLCO_valid)
        return false;

    bitsToByteBE(slotData[slot].EMB_data + 0U, &data[0U]);
    bitsToByteBE(slotData[slot].EMB_data + 8U, &data[1U]);
    bitsToByteBE(slotData[slot].EMB_data + 16U, &data[2U]);
    bitsToByteBE(slotData[slot].EMB_data + 24U, &data[3U]);
    bitsToByteBE(slotData[slot].EMB_data + 32U, &data[4U]);
    bitsToByteBE(slotData[slot].EMB_data + 40U, &data[5U]);
    bitsToByteBE(slotData[slot].EMB_data + 48U, &data[6U]);
    bitsToByteBE(slotData[slot].EMB_data + 56U, &data[7U]);
    bitsToByteBE(slotData[slot].EMB_data + 64U, &data[8U]);

    return true;
}

uint8_t getColorCode(const uint8_t slot)
{
	return slotData[slot].colorCode;
}

void setColorCode(uint8_t code, const uint8_t slot)
{
	slotData[slot].colorCode = code;
}

bool getPI(const uint8_t slot)
{
	return slotData[slot].piFlag;
}

void setPI(bool pi, const uint8_t slot)
{
	slotData[slot].piFlag = pi;
}

uint8_t getLCSS(const uint8_t slot)
{
	return slotData[slot].LCSS;
}

void setLCSS(uint8_t lcss, const uint8_t slot)
{
	slotData[slot].LCSS = lcss;
}

void decodeGPS(const unsigned char* data,  const uint8_t slot)
{
    unsigned int errorI = (data[2U] & 0x0E) >> 1U;

    const char* error;
    switch (errorI)
    {
        case 0U:
            error = "< 2m";
            break;
        case 1U:
            error = "< 20m";
            break;
        case 2U:
            error = "< 200m";
            break;
        case 3U:
            error = "< 2km";
            break;
        case 4U:
            error = "< 20km";
            break;
        case 5U:
            error = "< 200km";
            break;
        case 6U:
            error = "> 200km";
            break;
        default:
            error = "not known";
            break;
    }

    int32_t longitudeI = ((data[2U] & 0x01U) << 31) | (data[3U] << 23) | (data[4U] << 15) | (data[5U] << 7);
    longitudeI >>= 7;

    int32_t latitudeI = (data[6U] << 24) | (data[7U] << 16) | (data[8U] << 8);
    latitudeI >>= 8;

    float longitude = 360.0F / 33554432.0F;  // 360/2^25 steps
    float latitude  = 180.0F / 16777216.0F;  // 180/2^24 steps

    longitude *= (float)(longitudeI);
    latitude *= (float)(latitudeI);
}
