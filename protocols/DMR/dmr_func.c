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


/* --- DMR Slot Type (BPTC 20,8) encode/decode -----------------------------
 * Ported from MMDVM's DMRSlotType.cpp (Jonathan Naylor G4KLX). Recovers the
 * colour code and data type carried in a burst's slot type field -- needed
 * by RX frame classification, which this codebase did not previously have.
 */

static const uint16_t DMR_ENCODING_TABLE_2087[] =
	{0x0000U, 0xB08EU, 0xE093U, 0x501DU, 0x70A9U, 0xC027U, 0x903AU, 0x20B4U, 0x60DCU, 0xD052U, 0x804FU, 0x30C1U,
	 0x1075U, 0xA0FBU, 0xF0E6U, 0x4068U, 0x7036U, 0xC0B8U, 0x90A5U, 0x202BU, 0x009FU, 0xB011U, 0xE00CU, 0x5082U,
	 0x10EAU, 0xA064U, 0xF079U, 0x40F7U, 0x6043U, 0xD0CDU, 0x80D0U, 0x305EU, 0xD06CU, 0x60E2U, 0x30FFU, 0x8071U,
	 0xA0C5U, 0x104BU, 0x4056U, 0xF0D8U, 0xB0B0U, 0x003EU, 0x5023U, 0xE0ADU, 0xC019U, 0x7097U, 0x208AU, 0x9004U,
	 0xA05AU, 0x10D4U, 0x40C9U, 0xF047U, 0xD0F3U, 0x607DU, 0x3060U, 0x80EEU, 0xC086U, 0x7008U, 0x2015U, 0x909BU,
	 0xB02FU, 0x00A1U, 0x50BCU, 0xE032U, 0x90D9U, 0x2057U, 0x704AU, 0xC0C4U, 0xE070U, 0x50FEU, 0x00E3U, 0xB06DU,
	 0xF005U, 0x408BU, 0x1096U, 0xA018U, 0x80ACU, 0x3022U, 0x603FU, 0xD0B1U, 0xE0EFU, 0x5061U, 0x007CU, 0xB0F2U,
	 0x9046U, 0x20C8U, 0x70D5U, 0xC05BU, 0x8033U, 0x30BDU, 0x60A0U, 0xD02EU, 0xF09AU, 0x4014U, 0x1009U, 0xA087U,
	 0x40B5U, 0xF03BU, 0xA026U, 0x10A8U, 0x301CU, 0x8092U, 0xD08FU, 0x6001U, 0x2069U, 0x90E7U, 0xC0FAU, 0x7074U,
	 0x50C0U, 0xE04EU, 0xB053U, 0x00DDU, 0x3083U, 0x800DU, 0xD010U, 0x609EU, 0x402AU, 0xF0A4U, 0xA0B9U, 0x1037U,
	 0x505FU, 0xE0D1U, 0xB0CCU, 0x0042U, 0x20F6U, 0x9078U, 0xC065U, 0x70EBU, 0xA03DU, 0x10B3U, 0x40AEU, 0xF020U,
	 0xD094U, 0x601AU, 0x3007U, 0x8089U, 0xC0E1U, 0x706FU, 0x2072U, 0x90FCU, 0xB048U, 0x00C6U, 0x50DBU, 0xE055U,
	 0xD00BU, 0x6085U, 0x3098U, 0x8016U, 0xA0A2U, 0x102CU, 0x4031U, 0xF0BFU, 0xB0D7U, 0x0059U, 0x5044U, 0xE0CAU,
	 0xC07EU, 0x70F0U, 0x20EDU, 0x9063U, 0x7051U, 0xC0DFU, 0x90C2U, 0x204CU, 0x00F8U, 0xB076U, 0xE06BU, 0x50E5U,
	 0x108DU, 0xA003U, 0xF01EU, 0x4090U, 0x6024U, 0xD0AAU, 0x80B7U, 0x3039U, 0x0067U, 0xB0E9U, 0xE0F4U, 0x507AU,
	 0x70CEU, 0xC040U, 0x905DU, 0x20D3U, 0x60BBU, 0xD035U, 0x8028U, 0x30A6U, 0x1012U, 0xA09CU, 0xF081U, 0x400FU,
	 0x30E4U, 0x806AU, 0xD077U, 0x60F9U, 0x404DU, 0xF0C3U, 0xA0DEU, 0x1050U, 0x5038U, 0xE0B6U, 0xB0ABU, 0x0025U,
	 0x2091U, 0x901FU, 0xC002U, 0x708CU, 0x40D2U, 0xF05CU, 0xA041U, 0x10CFU, 0x307BU, 0x80F5U, 0xD0E8U, 0x6066U,
	 0x200EU, 0x9080U, 0xC09DU, 0x7013U, 0x50A7U, 0xE029U, 0xB034U, 0x00BAU, 0xE088U, 0x5006U, 0x001BU, 0xB095U,
	 0x9021U, 0x20AFU, 0x70B2U, 0xC03CU, 0x8054U, 0x30DAU, 0x60C7U, 0xD049U, 0xF0FDU, 0x4073U, 0x106EU, 0xA0E0U,
	 0x90BEU, 0x2030U, 0x702DU, 0xC0A3U, 0xE017U, 0x5099U, 0x0084U, 0xB00AU, 0xF062U, 0x40ECU, 0x10F1U, 0xA07FU,
	 0x80CBU, 0x3045U, 0x6058U, 0xD0D6U};

static const uint32_t DMR_DECODING_TABLE_1987[] =
	{0x00000U, 0x00001U, 0x00002U, 0x00003U, 0x00004U, 0x00005U, 0x00006U, 0x00007U, 0x00008U, 0x00009U, 0x0000AU, 0x0000BU, 0x0000CU, 
	 0x0000DU, 0x0000EU, 0x24020U, 0x00010U, 0x00011U, 0x00012U, 0x00013U, 0x00014U, 0x00015U, 0x00016U, 0x00017U, 0x00018U, 0x00019U, 
	 0x0001AU, 0x0001BU, 0x0001CU, 0x0001DU, 0x48040U, 0x01480U, 0x00020U, 0x00021U, 0x00022U, 0x00023U, 0x00024U, 0x00025U, 0x00026U, 
	 0x24008U, 0x00028U, 0x00029U, 0x0002AU, 0x24004U, 0x0002CU, 0x24002U, 0x24001U, 0x24000U, 0x00030U, 0x00031U, 0x00032U, 0x08180U, 
	 0x00034U, 0x00C40U, 0x00036U, 0x00C42U, 0x00038U, 0x43000U, 0x0003AU, 0x43002U, 0x02902U, 0x24012U, 0x02900U, 0x24010U, 0x00040U, 
	 0x00041U, 0x00042U, 0x00043U, 0x00044U, 0x00045U, 0x00046U, 0x00047U, 0x00048U, 0x00049U, 0x0004AU, 0x02500U, 0x0004CU, 0x0004DU, 
	 0x48010U, 0x48011U, 0x00050U, 0x00051U, 0x00052U, 0x21200U, 0x00054U, 0x00C20U, 0x48008U, 0x48009U, 0x00058U, 0x00059U, 0x48004U, 
	 0x48005U, 0x48002U, 0x48003U, 0x48000U, 0x48001U, 0x00060U, 0x00061U, 0x00062U, 0x00063U, 0x00064U, 0x00C10U, 0x10300U, 0x0B000U, 
	 0x00068U, 0x00069U, 0x01880U, 0x01881U, 0x40181U, 0x40180U, 0x24041U, 0x24040U, 0x00070U, 0x00C04U, 0x00072U, 0x00C06U, 0x00C01U, 
	 0x00C00U, 0x00C03U, 0x00C02U, 0x05204U, 0x00C0CU, 0x48024U, 0x48025U, 0x05200U, 0x00C08U, 0x48020U, 0x48021U, 0x00080U, 0x00081U, 
	 0x00082U, 0x00083U, 0x00084U, 0x00085U, 0x00086U, 0x00087U, 0x00088U, 0x00089U, 0x0008AU, 0x50200U, 0x0008CU, 0x0A800U, 0x01411U, 
	 0x01410U, 0x00090U, 0x00091U, 0x00092U, 0x08120U, 0x00094U, 0x00095U, 0x04A00U, 0x01408U, 0x00098U, 0x00099U, 0x01405U, 0x01404U, 
	 0x01403U, 0x01402U, 0x01401U, 0x01400U, 0x000A0U, 0x000A1U, 0x000A2U, 0x08110U, 0x000A4U, 0x000A5U, 0x42400U, 0x42401U, 0x000A8U, 
	 0x000A9U, 0x01840U, 0x01841U, 0x40141U, 0x40140U, 0x24081U, 0x24080U, 0x000B0U, 0x08102U, 0x08101U, 0x08100U, 0x000B4U, 0x08106U, 
	 0x08105U, 0x08104U, 0x20A01U, 0x20A00U, 0x08109U, 0x08108U, 0x01423U, 0x01422U, 0x01421U, 0x01420U, 0x000C0U, 0x000C1U, 0x000C2U, 
	 0x000C3U, 0x000C4U, 0x000C5U, 0x000C6U, 0x000C7U, 0x000C8U, 0x000C9U, 0x01820U, 0x01821U, 0x20600U, 0x40120U, 0x16000U, 0x16001U, 
	 0x000D0U, 0x000D1U, 0x42801U, 0x42800U, 0x03100U, 0x18200U, 0x03102U, 0x18202U, 0x000D8U, 0x000D9U, 0x48084U, 0x01444U, 0x48082U, 
	 0x01442U, 0x48080U, 0x01440U, 0x000E0U, 0x32000U, 0x01808U, 0x04600U, 0x40109U, 0x40108U, 0x0180CU, 0x4010AU, 0x01802U, 0x40104U, 
	 0x01800U, 0x01801U, 0x40101U, 0x40100U, 0x01804U, 0x40102U, 0x0A408U, 0x08142U, 0x08141U, 0x08140U, 0x00C81U, 0x00C80U, 0x00C83U, 
	 0x00C82U, 0x0A400U, 0x0A401U, 0x01810U, 0x01811U, 0x40111U, 0x40110U, 0x01814U, 0x40112U, 0x00100U, 0x00101U, 0x00102U, 0x00103U, 
	 0x00104U, 0x00105U, 0x00106U, 0x41800U, 0x00108U, 0x00109U, 0x0010AU, 0x02440U, 0x0010CU, 0x0010DU, 0x0010EU, 0x02444U, 0x00110U, 
	 0x00111U, 0x00112U, 0x080A0U, 0x00114U, 0x00115U, 0x00116U, 0x080A4U, 0x00118U, 0x00119U, 0x15000U, 0x15001U, 0x02822U, 0x02823U, 
	 0x02820U, 0x02821U, 0x00120U, 0x00121U, 0x00122U, 0x08090U, 0x00124U, 0x00125U, 0x10240U, 0x10241U, 0x00128U, 0x00129U, 0x0012AU, 
	 0x24104U, 0x09400U, 0x400C0U, 0x02810U, 0x24100U, 0x00130U, 0x08082U, 0x08081U, 0x08080U, 0x31001U, 0x31000U, 0x02808U, 0x08084U, 
	 0x02806U, 0x0808AU, 0x02804U, 0x08088U, 0x02802U, 0x02803U, 0x02800U, 0x02801U, 0x00140U, 0x00141U, 0x00142U, 0x02408U, 0x00144U, 
	 0x00145U, 0x10220U, 0x10221U, 0x00148U, 0x02402U, 0x02401U, 0x02400U, 0x400A1U, 0x400A0U, 0x02405U, 0x02404U, 0x00150U, 0x00151U, 
	 0x00152U, 0x02418U, 0x03080U, 0x03081U, 0x03082U, 0x03083U, 0x09801U, 0x09800U, 0x02411U, 0x02410U, 0x48102U, 0x09804U, 0x48100U, 
	 0x48101U, 0x00160U, 0x00161U, 0x10204U, 0x10205U, 0x10202U, 0x40088U, 0x10200U, 0x10201U, 0x40085U, 0x40084U, 0x02421U, 0x02420U, 
	 0x40081U, 0x40080U, 0x10208U, 0x40082U, 0x41402U, 0x080C2U, 0x41400U, 0x080C0U, 0x00D01U, 0x00D00U, 0x10210U, 0x10211U, 0x40095U, 
	 0x40094U, 0x02844U, 0x080C8U, 0x40091U, 0x40090U, 0x02840U, 0x02841U, 0x00180U, 0x00181U, 0x00182U, 0x08030U, 0x00184U, 0x14400U, 
	 0x22201U, 0x22200U, 0x00188U, 0x00189U, 0x0018AU, 0x08038U, 0x40061U, 0x40060U, 0x40063U, 0x40062U, 0x00190U, 0x08022U, 0x08021U, 
	 0x08020U, 0x03040U, 0x03041U, 0x08025U, 0x08024U, 0x40C00U, 0x40C01U, 0x08029U, 0x08028U, 0x2C000U, 0x2C001U, 0x01501U, 0x01500U, 
	 0x001A0U, 0x08012U, 0x08011U, 0x08010U, 0x40049U, 0x40048U, 0x08015U, 0x08014U, 0x06200U, 0x40044U, 0x30400U, 0x08018U, 0x40041U, 
	 0x40040U, 0x40043U, 0x40042U, 0x08003U, 0x08002U, 0x08001U, 0x08000U, 0x08007U, 0x08006U, 0x08005U, 0x08004U, 0x0800BU, 0x0800AU, 
	 0x08009U, 0x08008U, 0x40051U, 0x40050U, 0x02880U, 0x0800CU, 0x001C0U, 0x001C1U, 0x64000U, 0x64001U, 0x03010U, 0x40028U, 0x08C00U, 
	 0x08C01U, 0x40025U, 0x40024U, 0x02481U, 0x02480U, 0x40021U, 0x40020U, 0x40023U, 0x40022U, 0x03004U, 0x03005U, 0x08061U, 0x08060U, 
	 0x03000U, 0x03001U, 0x03002U, 0x03003U, 0x0300CU, 0x40034U, 0x30805U, 0x30804U, 0x03008U, 0x40030U, 0x30801U, 0x30800U, 0x4000DU, 
	 0x4000CU, 0x08051U, 0x08050U, 0x40009U, 0x40008U, 0x10280U, 0x4000AU, 0x40005U, 0x40004U, 0x01900U, 0x40006U, 0x40001U, 0x40000U, 
	 0x40003U, 0x40002U, 0x14800U, 0x08042U, 0x08041U, 0x08040U, 0x03020U, 0x40018U, 0x08045U, 0x08044U, 0x40015U, 0x40014U, 0x08049U, 
	 0x08048U, 0x40011U, 0x40010U, 0x40013U, 0x40012U, 0x00200U, 0x00201U, 0x00202U, 0x00203U, 0x00204U, 0x00205U, 0x00206U, 0x00207U, 
	 0x00208U, 0x00209U, 0x0020AU, 0x50080U, 0x0020CU, 0x0020DU, 0x0020EU, 0x50084U, 0x00210U, 0x00211U, 0x00212U, 0x21040U, 0x00214U, 
	 0x00215U, 0x04880U, 0x04881U, 0x00218U, 0x00219U, 0x0E001U, 0x0E000U, 0x0021CU, 0x0021DU, 0x04888U, 0x0E004U, 0x00220U, 0x00221U, 
	 0x00222U, 0x00223U, 0x00224U, 0x00225U, 0x10140U, 0x10141U, 0x00228U, 0x00229U, 0x0022AU, 0x24204U, 0x12401U, 0x12400U, 0x24201U, 
	 0x24200U, 0x00230U, 0x00231U, 0x00232U, 0x21060U, 0x2A000U, 0x2A001U, 0x2A002U, 0x2A003U, 0x20881U, 0x20880U, 0x20883U, 0x20882U, 
	 0x05040U, 0x05041U, 0x05042U, 0x24210U, 0x00240U, 0x00241U, 0x00242U, 0x21010U, 0x00244U, 0x46000U, 0x10120U, 0x10121U, 0x00248U, 
	 0x00249U, 0x0024AU, 0x21018U, 0x20480U, 0x20481U, 0x20482U, 0x20483U, 0x00250U, 0x21002U, 0x21001U, 0x21000U, 0x18081U, 0x18080U, 
	 0x21005U, 0x21004U, 0x12800U, 0x12801U, 0x21009U, 0x21008U, 0x05020U, 0x05021U, 0x48200U, 0x48201U, 0x00260U, 0x00261U, 0x10104U, 
	 0x04480U, 0x10102U, 0x10103U, 0x10100U, 0x10101U, 0x62002U, 0x62003U, 0x62000U, 0x62001U, 0x05010U, 0x05011U, 0x10108U, 0x10109U, 
	 0x0500CU, 0x21022U, 0x21021U, 0x21020U, 0x05008U, 0x00E00U, 0x10110U, 0x10111U, 0x05004U, 0x05005U, 0x05006U, 0x21028U, 0x05000U, 
	 0x05001U, 0x05002U, 0x05003U, 0x00280U, 0x00281U, 0x00282U, 0x50008U, 0x00284U, 0x00285U, 0x04810U, 0x22100U, 0x00288U, 0x50002U, 
	 0x50001U, 0x50000U, 0x20440U, 0x20441U, 0x50005U, 0x50004U, 0x00290U, 0x00291U, 0x04804U, 0x04805U, 0x04802U, 0x18040U, 0x04800U, 
	 0x04801U, 0x20821U, 0x20820U, 0x50011U, 0x50010U, 0x0480AU, 0x01602U, 0x04808U, 0x01600U, 0x002A0U, 0x002A1U, 0x04441U, 0x04440U, 
	 0x002A4U, 0x002A5U, 0x04830U, 0x04444U, 0x06100U, 0x20810U, 0x50021U, 0x50020U, 0x06104U, 0x20814U, 0x50025U, 0x50024U, 0x20809U, 
	 0x20808U, 0x13000U, 0x08300U, 0x04822U, 0x2080CU, 0x04820U, 0x04821U, 0x20801U, 0x20800U, 0x20803U, 0x20802U, 0x20805U, 0x20804U, 
	 0x04828U, 0x20806U, 0x002C0U, 0x002C1U, 0x04421U, 0x04420U, 0x20408U, 0x18010U, 0x2040AU, 0x18012U, 0x20404U, 0x20405U, 0x50041U, 
	 0x50040U, 0x20400U, 0x20401U, 0x20402U, 0x20403U, 0x18005U, 0x18004U, 0x21081U, 0x21080U, 0x18001U, 0x18000U, 0x04840U, 0x18002U, 
	 0x20414U, 0x1800CU, 0x21089U, 0x21088U, 0x20410U, 0x18008U, 0x20412U, 0x1800AU, 0x04403U, 0x04402U, 0x04401U, 0x04400U, 0x10182U, 
	 0x04406U, 0x10180U, 0x04404U, 0x01A02U, 0x0440AU, 0x01A00U, 0x04408U, 0x20420U, 0x40300U, 0x20422U, 0x40302U, 0x04413U, 0x04412U, 
	 0x04411U, 0x04410U, 0x18021U, 0x18020U, 0x10190U, 0x18022U, 0x20841U, 0x20840U, 0x01A10U, 0x20842U, 0x05080U, 0x05081U, 0x05082U, 
	 0x05083U, 0x00300U, 0x00301U, 0x00302U, 0x00303U, 0x00304U, 0x00305U, 0x10060U, 0x22080U, 0x00308U, 0x00309U, 0x28800U, 0x28801U, 
	 0x44402U, 0x44403U, 0x44400U, 0x44401U, 0x00310U, 0x00311U, 0x10C01U, 0x10C00U, 0x00314U, 0x00315U, 0x10070U, 0x10C04U, 0x00318U, 
	 0x00319U, 0x28810U, 0x10C08U, 0x44412U, 0x00000U, 0x44410U, 0x44411U, 0x00320U, 0x60400U, 0x10044U, 0x10045U, 0x10042U, 0x0C800U, 
	 0x10040U, 0x10041U, 0x06080U, 0x06081U, 0x06082U, 0x06083U, 0x1004AU, 0x0C808U, 0x10048U, 0x10049U, 0x58008U, 0x08282U, 0x08281U, 
	 0x08280U, 0x10052U, 0x0C810U, 0x10050U, 0x10051U, 0x58000U, 0x58001U, 0x58002U, 0x08288U, 0x02A02U, 0x02A03U, 0x02A00U, 0x02A01U, 
	 0x00340U, 0x00341U, 0x10024U, 0x10025U, 0x10022U, 0x10023U, 0x10020U, 0x10021U, 0x34001U, 0x34000U, 0x02601U, 0x02600U, 0x1002AU, 
	 0x34004U, 0x10028U, 0x10029U, 0x0C400U, 0x0C401U, 0x21101U, 0x21100U, 0x60800U, 0x60801U, 0x10030U, 0x10031U, 0x0C408U, 0x34010U, 
	 0x21109U, 0x21108U, 0x60808U, 0x60809U, 0x10038U, 0x28420U, 0x10006U, 0x10007U, 0x10004U, 0x10005U, 0x10002U, 0x10003U, 0x10000U, 
	 0x10001U, 0x1000EU, 0x40284U, 0x1000CU, 0x1000DU, 0x1000AU, 0x40280U, 0x10008U, 0x10009U, 0x10016U, 0x10017U, 0x10014U, 0x10015U, 
	 0x10012U, 0x10013U, 0x10010U, 0x10011U, 0x05104U, 0x44802U, 0x44801U, 0x44800U, 0x05100U, 0x05101U, 0x10018U, 0x28400U, 0x00380U, 
	 0x00381U, 0x22005U, 0x22004U, 0x22003U, 0x22002U, 0x22001U, 0x22000U, 0x06020U, 0x06021U, 0x50101U, 0x50100U, 0x11800U, 0x11801U, 
	 0x22009U, 0x22008U, 0x45001U, 0x45000U, 0x08221U, 0x08220U, 0x04902U, 0x22012U, 0x04900U, 0x22010U, 0x06030U, 0x45008U, 0x08229U, 
	 0x08228U, 0x11810U, 0x11811U, 0x04908U, 0x22018U, 0x06008U, 0x06009U, 0x08211U, 0x08210U, 0x100C2U, 0x22022U, 0x100C0U, 0x22020U, 
	 0x06000U, 0x06001U, 0x06002U, 0x06003U, 0x06004U, 0x40240U, 0x06006U, 0x40242U, 0x08203U, 0x08202U, 0x08201U, 0x08200U, 0x08207U, 
	 0x08206U, 0x08205U, 0x08204U, 0x06010U, 0x20900U, 0x08209U, 0x08208U, 0x61002U, 0x20904U, 0x61000U, 0x61001U, 0x29020U, 0x29021U, 
	 0x100A4U, 0x22044U, 0x100A2U, 0x22042U, 0x100A0U, 0x22040U, 0x20504U, 0x40224U, 0x0D005U, 0x0D004U, 0x20500U, 0x40220U, 0x0D001U, 
	 0x0D000U, 0x03204U, 0x18104U, 0x08261U, 0x08260U, 0x03200U, 0x18100U, 0x03202U, 0x18102U, 0x11421U, 0x11420U, 0x00000U, 0x11422U, 
	 0x03208U, 0x18108U, 0x0D011U, 0x0D010U, 0x29000U, 0x29001U, 0x10084U, 0x04500U, 0x10082U, 0x40208U, 0x10080U, 0x10081U, 0x06040U, 
	 0x40204U, 0x06042U, 0x40206U, 0x40201U, 0x40200U, 0x10088U, 0x40202U, 0x29010U, 0x08242U, 0x08241U, 0x08240U, 0x10092U, 0x40218U, 
	 0x10090U, 0x10091U, 0x11401U, 0x11400U, 0x11403U, 0x11402U, 0x40211U, 0x40210U, 0x10098U, 0x40212U, 0x00400U, 0x00401U, 0x00402U, 
	 0x00403U, 0x00404U, 0x00405U, 0x00406U, 0x00407U, 0x00408U, 0x00409U, 0x0040AU, 0x02140U, 0x0040CU, 0x0040DU, 0x01091U, 0x01090U, 
	 0x00410U, 0x00411U, 0x00412U, 0x00413U, 0x00414U, 0x00860U, 0x01089U, 0x01088U, 0x00418U, 0x38000U, 0x01085U, 0x01084U, 0x01083U, 
	 0x01082U, 0x01081U, 0x01080U, 0x00420U, 0x00421U, 0x00422U, 0x00423U, 0x00424U, 0x00850U, 0x42080U, 0x42081U, 0x00428U, 0x00429U, 
	 0x48801U, 0x48800U, 0x09100U, 0x12200U, 0x24401U, 0x24400U, 0x00430U, 0x00844U, 0x00432U, 0x00846U, 0x00841U, 0x00840U, 0x1C000U, 
	 0x00842U, 0x00438U, 0x0084CU, 0x010A5U, 0x010A4U, 0x00849U, 0x00848U, 0x010A1U, 0x010A0U, 0x00440U, 0x00441U, 0x00442U, 0x02108U, 
	 0x00444U, 0x00830U, 0x70001U, 0x70000U, 0x00448U, 0x02102U, 0x02101U, 0x02100U, 0x20280U, 0x20281U, 0x02105U, 0x02104U, 0x00450U, 
	 0x00824U, 0x00452U, 0x00826U, 0x00821U, 0x00820U, 0x00823U, 0x00822U, 0x24802U, 0x02112U, 0x24800U, 0x02110U, 0x00829U, 0x00828U, 
	 0x48400U, 0x010C0U, 0x00460U, 0x00814U, 0x04281U, 0x04280U, 0x00811U, 0x00810U, 0x00813U, 0x00812U, 0x54000U, 0x54001U, 0x02121U, 
	 0x02120U, 0x00819U, 0x00818U, 0x0081BU, 0x0081AU, 0x00805U, 0x00804U, 0x41100U, 0x00806U, 0x00801U, 0x00800U, 0x00803U, 0x00802U, 
	 0x0A080U, 0x0080CU, 0x0A082U, 0x0080EU, 0x00809U, 0x00808U, 0x0080BU, 0x0080AU, 0x00480U, 0x00481U, 0x00482U, 0x00483U, 0x00484U, 
	 0x14100U, 0x42020U, 0x01018U, 0x00488U, 0x00489U, 0x01015U, 0x01014U, 0x20240U, 0x01012U, 0x01011U, 0x01010U, 0x00490U, 0x00491U, 
	 0x0100DU, 0x0100CU, 0x0100BU, 0x0100AU, 0x01009U, 0x01008U, 0x40900U, 0x01006U, 0x01005U, 0x01004U, 0x01003U, 0x01002U, 0x01001U, 
	 0x01000U, 0x004A0U, 0x004A1U, 0x42004U, 0x04240U, 0x42002U, 0x42003U, 0x42000U, 0x42001U, 0x30102U, 0x30103U, 0x30100U, 0x30101U, 
	 0x4200AU, 0x01032U, 0x42008U, 0x01030U, 0x25000U, 0x25001U, 0x08501U, 0x08500U, 0x008C1U, 0x008C0U, 0x42010U, 0x01028U, 0x0A040U, 
	 0x0A041U, 0x01025U, 0x01024U, 0x01023U, 0x01022U, 0x01021U, 0x01020U, 0x004C0U, 0x49000U, 0x04221U, 0x04220U, 0x20208U, 0x20209U, 
	 0x08900U, 0x08901U, 0x20204U, 0x20205U, 0x02181U, 0x02180U, 0x20200U, 0x20201U, 0x20202U, 0x01050U, 0x0A028U, 0x008A4U, 0x0104DU, 
	 0x0104CU, 0x008A1U, 0x008A0U, 0x01049U, 0x01048U, 0x0A020U, 0x0A021U, 0x01045U, 0x01044U, 0x20210U, 0x01042U, 0x01041U, 0x01040U, 
	 0x04203U, 0x04202U, 0x04201U, 0x04200U, 0x00891U, 0x00890U, 0x42040U, 0x04204U, 0x0A010U, 0x0A011U, 0x01C00U, 0x04208U, 0x20220U, 
	 0x40500U, 0x20222U, 0x40502U, 0x0A008U, 0x00884U, 0x04211U, 0x04210U, 0x00881U, 0x00880U, 0x00883U, 0x00882U, 0x0A000U, 0x0A001U, 
	 0x0A002U, 0x0A003U, 0x0A004U, 0x00888U, 0x01061U, 0x01060U, 0x00500U, 0x00501U, 0x00502U, 0x02048U, 0x00504U, 0x14080U, 0x00506U, 
	 0x14082U, 0x00508U, 0x02042U, 0x02041U, 0x02040U, 0x09020U, 0x09021U, 0x44200U, 0x02044U, 0x00510U, 0x00511U, 0x10A01U, 0x10A00U, 
	 0x4A001U, 0x4A000U, 0x4A003U, 0x4A002U, 0x40880U, 0x40881U, 0x02051U, 0x02050U, 0x40884U, 0x01182U, 0x01181U, 0x01180U, 0x00520U, 
	 0x60200U, 0x00522U, 0x60202U, 0x09008U, 0x09009U, 0x0900AU, 0x0900BU, 0x09004U, 0x09005U, 0x30080U, 0x02060U, 0x09000U, 0x09001U, 
	 0x09002U, 0x09003U, 0x41042U, 0x08482U, 0x41040U, 0x08480U, 0x00941U, 0x00940U, 0x41044U, 0x00942U, 0x09014U, 0x09015U, 0x02C04U, 
	 0x08488U, 0x09010U, 0x09011U, 0x02C00U, 0x02C01U, 0x00540U, 0x0200AU, 0x02009U, 0x02008U, 0x08882U, 0x0200EU, 0x08880U, 0x0200CU, 
	 0x02003U, 0x02002U, 0x02001U, 0x02000U, 0x02007U, 0x02006U, 0x02005U, 0x02004U, 0x0C200U, 0x0C201U, 0x41020U, 0x02018U, 0x00921U, 
	 0x00920U, 0x41024U, 0x00922U, 0x02013U, 0x02012U, 0x02011U, 0x02010U, 0x02017U, 0x02016U, 0x02015U, 0x02014U, 0x41012U, 0x0202AU, 
	 0x41010U, 0x02028U, 0x26000U, 0x00910U, 0x10600U, 0x10601U, 0x02023U, 0x02022U, 0x02021U, 0x02020U, 0x09040U, 0x40480U, 0x02025U, 
	 0x02024U, 0x41002U, 0x00904U, 0x41000U, 0x41001U, 0x00901U, 0x00900U, 0x41004U, 0x00902U, 0x4100AU, 0x02032U, 0x41008U, 0x02030U, 
	 0x00909U, 0x00908U, 0x28201U, 0x28200U, 0x00580U, 0x14004U, 0x00582U, 0x14006U, 0x14001U, 0x14000U, 0x08840U, 0x14002U, 0x40810U, 
	 0x40811U, 0x30020U, 0x020C0U, 0x14009U, 0x14008U, 0x01111U, 0x01110U, 0x40808U, 0x40809U, 0x08421U, 0x08420U, 0x14011U, 0x14010U, 
	 0x01109U, 0x01108U, 0x40800U, 0x40801U, 0x40802U, 0x01104U, 0x40804U, 0x01102U, 0x01101U, 0x01100U, 0x03801U, 0x03800U, 0x30008U, 
	 0x08410U, 0x14021U, 0x14020U, 0x42100U, 0x42101U, 0x30002U, 0x30003U, 0x30000U, 0x30001U, 0x09080U, 0x40440U, 0x30004U, 0x30005U, 
	 0x08403U, 0x08402U, 0x08401U, 0x08400U, 0x08407U, 0x08406U, 0x08405U, 0x08404U, 0x40820U, 0x40821U, 0x30010U, 0x08408U, 0x40824U, 
	 0x01122U, 0x01121U, 0x01120U, 0x08806U, 0x0208AU, 0x08804U, 0x02088U, 0x08802U, 0x14040U, 0x08800U, 0x08801U, 0x02083U, 0x02082U, 
	 0x02081U, 0x02080U, 0x20300U, 0x40420U, 0x08808U, 0x02084U, 0x03404U, 0x03405U, 0x08814U, 0x02098U, 0x03400U, 0x03401U, 0x08810U, 
	 0x08811U, 0x40840U, 0x40841U, 0x02091U, 0x02090U, 0x40844U, 0x01142U, 0x01141U, 0x01140U, 0x04303U, 0x04302U, 0x04301U, 0x04300U, 
	 0x40409U, 0x40408U, 0x08820U, 0x08821U, 0x40405U, 0x40404U, 0x30040U, 0x020A0U, 0x40401U, 0x40400U, 0x40403U, 0x40402U, 0x41082U, 
	 0x08442U, 0x41080U, 0x08440U, 0x00981U, 0x00980U, 0x41084U, 0x00982U, 0x0A100U, 0x11200U, 0x0A102U, 0x11202U, 0x40411U, 0x40410U, 
	 0x40413U, 0x40412U, 0x00600U, 0x00601U, 0x00602U, 0x00603U, 0x00604U, 0x00605U, 0x00606U, 0x00607U, 0x00608U, 0x05800U, 0x0060AU, 
	 0x05802U, 0x200C0U, 0x12020U, 0x44100U, 0x44101U, 0x00610U, 0x00611U, 0x10901U, 0x10900U, 0x51000U, 0x51001U, 0x51002U, 0x10904U, 
	 0x00618U, 0x05810U, 0x01285U, 0x01284U, 0x51008U, 0x01282U, 0x01281U, 0x01280U, 0x00620U, 0x60100U, 0x040C1U, 0x040C0U, 0x12009U, 
	 0x12008U, 0x21800U, 0x21801U, 0x12005U, 0x12004U, 0x12007U, 0x12006U, 0x12001U, 0x12000U, 0x12003U, 0x12002U, 0x00630U, 0x00A44U, 
	 0x040D1U, 0x040D0U, 0x00A41U, 0x00A40U, 0x21810U, 0x00A42U, 0x12015U, 0x12014U, 0x00000U, 0x12016U, 0x12011U, 0x12010U, 0x12013U, 
	 0x12012U, 0x00640U, 0x00641U, 0x040A1U, 0x040A0U, 0x20088U, 0x20089U, 0x2008AU, 0x040A4U, 0x20084U, 0x20085U, 0x19000U, 0x02300U, 
	 0x20080U, 0x20081U, 0x20082U, 0x20083U, 0x0C100U, 0x0C101U, 0x21401U, 0x21400U, 0x00A21U, 0x00A20U, 0x00A23U, 0x00A22U, 0x20094U, 
	 0x20095U, 0x19010U, 0x21408U, 0x20090U, 0x20091U, 0x20092U, 0x28120U, 0x04083U, 0x04082U, 0x04081U, 0x04080U, 0x00A11U, 0x00A10U, 
	 0x10500U, 0x04084U, 0x200A4U, 0x0408AU, 0x04089U, 0x04088U, 0x200A0U, 0x12040U, 0x200A2U, 0x12042U, 0x00A05U, 0x00A04U, 0x04091U, 
	 0x04090U, 0x00A01U, 0x00A00U, 0x00A03U, 0x00A02U, 0x05404U, 0x00A0CU, 0x28105U, 0x28104U, 0x05400U, 0x00A08U, 0x28101U, 0x28100U, 
	 0x00680U, 0x00681U, 0x04061U, 0x04060U, 0x20048U, 0x20049U, 0x2004AU, 0x04064U, 0x20044U, 0x20045U, 0x50401U, 0x50400U, 0x20040U, 
	 0x20041U, 0x20042U, 0x01210U, 0x68002U, 0x68003U, 0x68000U, 0x68001U, 0x04C02U, 0x0120AU, 0x04C00U, 0x01208U, 0x20054U, 0x01206U, 
	 0x01205U, 0x01204U, 0x20050U, 0x01202U, 0x01201U, 0x01200U, 0x18800U, 0x04042U, 0x04041U, 0x04040U, 0x42202U, 0x04046U, 0x42200U, 
	 0x04044U, 0x20064U, 0x0404AU, 0x04049U, 0x04048U, 0x20060U, 0x12080U, 0x20062U, 0x12082U, 0x18810U, 0x04052U, 0x04051U, 0x04050U, 
	 0x4C009U, 0x4C008U, 0x42210U, 0x04054U, 0x20C01U, 0x20C00U, 0x20C03U, 0x20C02U, 0x4C001U, 0x4C000U, 0x01221U, 0x01220U, 0x2000CU, 
	 0x04022U, 0x04021U, 0x04020U, 0x20008U, 0x20009U, 0x2000AU, 0x04024U, 0x20004U, 0x20005U, 0x20006U, 0x04028U, 0x20000U, 0x20001U, 
	 0x20002U, 0x20003U, 0x2001CU, 0x04032U, 0x04031U, 0x04030U, 0x20018U, 0x18400U, 0x2001AU, 0x18402U, 0x20014U, 0x20015U, 0x20016U, 
	 0x01244U, 0x20010U, 0x20011U, 0x20012U, 0x01240U, 0x04003U, 0x04002U, 0x04001U, 0x04000U, 0x20028U, 0x04006U, 0x04005U, 0x04004U, 
	 0x20024U, 0x0400AU, 0x04009U, 0x04008U, 0x20020U, 0x20021U, 0x20022U, 0x0400CU, 0x04013U, 0x04012U, 0x04011U, 0x04010U, 0x00A81U, 
	 0x00A80U, 0x04015U, 0x04014U, 0x0A200U, 0x11100U, 0x04019U, 0x04018U, 0x20030U, 0x20031U, 0x50800U, 0x50801U, 0x00700U, 0x60020U, 
	 0x10811U, 0x10810U, 0x4400AU, 0x60024U, 0x44008U, 0x44009U, 0x44006U, 0x02242U, 0x44004U, 0x02240U, 0x44002U, 0x44003U, 0x44000U, 
	 0x44001U, 0x0C040U, 0x10802U, 0x10801U, 0x10800U, 0x0C044U, 0x10806U, 0x10805U, 0x10804U, 0x23000U, 0x23001U, 0x10809U, 0x10808U, 
	 0x44012U, 0x44013U, 0x44010U, 0x44011U, 0x60001U, 0x60000U, 0x60003U, 0x60002U, 0x60005U, 0x60004U, 0x10440U, 0x10441U, 0x60009U, 
	 0x60008U, 0x44024U, 0x6000AU, 0x09200U, 0x12100U, 0x44020U, 0x44021U, 0x60011U, 0x60010U, 0x10821U, 0x10820U, 0x07003U, 0x07002U, 
	 0x07001U, 0x07000U, 0x23020U, 0x60018U, 0x28045U, 0x28044U, 0x09210U, 0x28042U, 0x28041U, 0x28040U, 0x0C010U, 0x0C011U, 0x02209U, 
	 0x02208U, 0x10422U, 0x10423U, 0x10420U, 0x10421U, 0x02203U, 0x02202U, 0x02201U, 0x02200U, 0x20180U, 0x20181U, 0x44040U, 0x02204U, 
	 0x0C000U, 0x0C001U, 0x0C002U, 0x10840U, 0x0C004U, 0x0C005U, 0x0C006U, 0x10844U, 0x0C008U, 0x0C009U, 0x02211U, 0x02210U, 0x0C00CU, 
	 0x28022U, 0x28021U, 0x28020U, 0x60041U, 0x60040U, 0x10404U, 0x04180U, 0x10402U, 0x10403U, 0x10400U, 0x10401U, 0x02223U, 0x02222U, 
	 0x02221U, 0x02220U, 0x1040AU, 0x28012U, 0x10408U, 0x28010U, 0x0C020U, 0x0C021U, 0x41200U, 0x41201U, 0x00B01U, 0x00B00U, 0x10410U, 
	 0x28008U, 0x11081U, 0x11080U, 0x28005U, 0x28004U, 0x28003U, 0x28002U, 0x28001U, 0x28000U, 0x52040U, 0x14204U, 0x22405U, 0x22404U, 
	 0x14201U, 0x14200U, 0x22401U, 0x22400U, 0x20144U, 0x20145U, 0x44084U, 0x022C0U, 0x20140U, 0x20141U, 0x44080U, 0x44081U, 0x40A08U, 
	 0x10882U, 0x10881U, 0x10880U, 0x14211U, 0x14210U, 0x1A008U, 0x10884U, 0x40A00U, 0x40A01U, 0x40A02U, 0x01304U, 0x1A002U, 0x01302U, 
	 0x1A000U, 0x01300U, 0x60081U, 0x60080U, 0x04141U, 0x04140U, 0x60085U, 0x60084U, 0x104C0U, 0x04144U, 0x06400U, 0x06401U, 0x30200U, 
	 0x30201U, 0x06404U, 0x40640U, 0x30204U, 0x30205U, 0x08603U, 0x08602U, 0x08601U, 0x08600U, 0x00000U, 0x08606U, 0x08605U, 0x08604U, 
	 0x11041U, 0x11040U, 0x30210U, 0x11042U, 0x11045U, 0x11044U, 0x1A020U, 0x01320U, 0x52000U, 0x52001U, 0x04121U, 0x04120U, 0x20108U, 
	 0x20109U, 0x08A00U, 0x08A01U, 0x20104U, 0x20105U, 0x02281U, 0x02280U, 0x20100U, 0x20101U, 0x20102U, 0x20103U, 0x0C080U, 0x0C081U, 
	 0x0C082U, 0x04130U, 0x0C084U, 0x06808U, 0x08A10U, 0x08A11U, 0x11021U, 0x11020U, 0x11023U, 0x11022U, 0x20110U, 0x06800U, 0x20112U, 
	 0x06802U, 0x04103U, 0x04102U, 0x04101U, 0x04100U, 0x10482U, 0x04106U, 0x10480U, 0x04104U, 0x11011U, 0x11010U, 0x04109U, 0x04108U, 
	 0x20120U, 0x40600U, 0x20122U, 0x40602U, 0x11009U, 0x11008U, 0x22800U, 0x04110U, 0x1100DU, 0x1100CU, 0x22804U, 0x04114U, 0x11001U, 
	 0x11000U, 0x11003U, 0x11002U, 0x11005U, 0x11004U, 0x28081U, 0x28080U};

#define DMR_SLOTTYPE_X18     0x00040000U   /* vector representation of X^{18} */
#define DMR_SLOTTYPE_X11     0x00000800U   /* vector representation of X^{11} */
#define DMR_SLOTTYPE_MASK8   0xfffff800U   /* auxiliary vector for testing */
#define DMR_SLOTTYPE_GENPOL  0x00000c75U   /* generator polynomial, g(x) */

/*
 * Compute the syndrome corresponding to the given pattern, i.e., the
 * remainder after dividing the pattern (when considering it as the vector
 * representation of a polynomial) by the generator polynomial, GENPOL.
 */
static uint32_t DMRSlotType_getSyndrome1987(uint32_t pattern)
{
    uint32_t aux = DMR_SLOTTYPE_X18;

    if (pattern >= DMR_SLOTTYPE_X11)
    {
        while (pattern & DMR_SLOTTYPE_MASK8)
        {
            while (!(aux & pattern))
                aux = aux >> 1;

            pattern ^= (aux / DMR_SLOTTYPE_X11) * DMR_SLOTTYPE_GENPOL;
        }
    }

    return pattern;
}

static uint8_t DMRSlotType_decode2087(const unsigned char* data)
{
    uint32_t code = ((uint32_t)data[0U] << 11) + ((uint32_t)data[1U] << 3) + (data[2U] >> 5);
    uint32_t syndrome = DMRSlotType_getSyndrome1987(code);
    uint32_t error_pattern = DMR_DECODING_TABLE_1987[syndrome];

    if (error_pattern != 0x00U)
        code ^= error_pattern;

    return (uint8_t)(code >> 11);
}

void DMRSlotType_decode(const unsigned char* frame, uint8_t* colorCode, uint8_t* dataType)
{
    unsigned char slotType[3U];
    slotType[0U]  = (frame[12U] << 2) & 0xFCU;
    slotType[0U] |= (frame[13U] >> 6) & 0x03U;

    slotType[1U]  = (frame[13U] << 2) & 0xC0U;
    slotType[1U] |= (frame[19U] << 2) & 0x3CU;
    slotType[1U] |= (frame[20U] >> 6) & 0x03U;

    slotType[2U]  = (frame[20U] << 2) & 0xF0U;

    uint8_t code = DMRSlotType_decode2087(slotType);

    *colorCode = (code >> 4) & 0x0FU;
    *dataType  = (code >> 0) & 0x0FU;
}

void DMRSlotType_encode(uint8_t colorCode, uint8_t dataType, unsigned char* frame)
{
    unsigned char slotType[3U];
    slotType[0U]  = (colorCode << 4) & 0xF0U;
    slotType[0U] |= (dataType  << 0) & 0x0FU;

    uint16_t cksum = DMR_ENCODING_TABLE_2087[slotType[0U]];

    slotType[1U] = (cksum >> 0) & 0xFFU;
    slotType[2U] = (cksum >> 8) & 0xFFU;

    frame[12U] = (frame[12U] & 0xC0U) | ((slotType[0U] >> 2) & 0x3FU);
    frame[13U] = (frame[13U] & 0x0FU) | ((slotType[0U] << 6) & 0xC0U) | ((slotType[1U] >> 2) & 0x30U);
    frame[19U] = (frame[19U] & 0xF0U) | ((slotType[1U] >> 2) & 0x0FU);
    frame[20U] = (frame[20U] & 0x03U) | ((slotType[1U] << 6) & 0xC0U) | ((slotType[2U] >> 2) & 0x3CU);
}
