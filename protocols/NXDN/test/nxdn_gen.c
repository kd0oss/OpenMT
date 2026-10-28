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

#include <string.h>

#include "nxdn_func.h"
#include "nxdn_gen.h"

static uint32_t rngNext(uint32_t* s)
{
    *s = *s * 1664525U + 1013904223U;
    return *s >> 8;
}

static void l3Call(uint8_t* l3, uint8_t type, uint16_t src, uint16_t dst, bool group)
{
    memset(l3, 0x00, 22U);

    l3[0] = type;
    l3[1] = 0x00U;                      /* cipher, no encryption          */
    l3[2] = group ? 0x20U : 0x40U;      /* call type: 1 = group (conference), 2 = unit to unit */
    l3[3] = (src >> 8) & 0xFFU;
    l3[4] = src & 0xFFU;
    l3[5] = (dst >> 8) & 0xFFU;
    l3[6] = dst & 0xFFU;
}

static void facchFrame(uint8_t ran, const uint8_t* l3, uint8_t* frame)
{
    uint8_t sacch[5];

    memset(frame, 0x00, NXDN_FRAME_LENGTH_BYTES);
    nxdnAddSync(frame);
    nxdnLichEncode(nxdnLichMake(NXDN_LICH_RFCT_RDCH, NXDN_LICH_USC_SACCH_NS, NXDN_LICH_STEAL_FACCH,
                                NXDN_LICH_DIRECTION_INBOUND), frame);

    memset(sacch, 0x00, sizeof(sacch));
    nxdnSacchSetRAN(sacch, ran);
    nxdnSacchSetStructure(sacch, NXDN_SR_SINGLE);
    nxdnSacchSetData(sacch, (const uint8_t[]){ NXDN_MESSAGE_TYPE_IDLE, 0x00U, 0x00U });
    nxdnSacchEncode(sacch, frame);

    nxdnFacch1Encode(l3, frame, NXDN_FACCH1_OFFSET_1);
    nxdnFacch1Encode(l3, frame, NXDN_FACCH1_OFFSET_2);

    nxdnScramble(frame);
}

/*
 * Copies the 49 bit block 'index' of a stream of 7 byte blocks into bit
 * position 'bitOffset' of the network form voice data.
 */
static void putBlock(uint8_t* net, unsigned int bitOffset, const uint8_t* block)
{
    for (unsigned int i = 0U; i < 49U; i++)
    {
        bool v = (block[i >> 3] & (0x80U >> (i & 7U))) != 0U;
        unsigned int pos = bitOffset + i;

        if (v)
            net[pos >> 3] |= (uint8_t)(0x80U >> (pos & 7U));
        else
            net[pos >> 3] &= (uint8_t)~(0x80U >> (pos & 7U));
    }
}

unsigned int nxdnGenVoiceCallAudio(uint8_t ran, uint16_t src, uint16_t dst, bool group, unsigned int voiceFrames,
                                   const uint8_t* ambe, uint8_t frames[][48], unsigned int max, uint32_t seed)
{
    uint8_t l3[22];
    unsigned int n = 0U;

    if (max < voiceFrames + 2U)
        return 0U;

    l3Call(l3, NXDN_MESSAGE_TYPE_VCALL, src, dst, group);
    facchFrame(ran, l3, frames[n++]);

    for (unsigned int f = 0U; f < voiceFrames; f++)
    {
        uint8_t* frame = frames[n++];
        uint8_t net[28];
        uint8_t sacch[5];
        uint8_t chunk[3];
        static const uint8_t order[4] = { NXDN_SR_1_4, NXDN_SR_2_4, NXDN_SR_3_4, NXDN_SR_4_4 };
        unsigned int part = f & 3U;

        memset(frame, 0x00, NXDN_FRAME_LENGTH_BYTES);
        nxdnAddSync(frame);
        nxdnLichEncode(nxdnLichMake(NXDN_LICH_RFCT_RDCH, NXDN_LICH_USC_SACCH_SS, NXDN_LICH_STEAL_NONE,
                                    NXDN_LICH_DIRECTION_INBOUND), frame);

        /* The SACCH superblock carries the 72 bits of the call information */
        memset(chunk, 0x00, sizeof(chunk));
        for (unsigned int b = 0U; b < 18U; b++)
        {
            unsigned int bit = part * 18U + b;
            bool v = (l3[bit >> 3] & (0x80U >> (bit & 7U))) != 0U;
            if (v)
                chunk[b >> 3] |= 0x80U >> (b & 7U);
        }

        memset(sacch, 0x00, sizeof(sacch));
        nxdnSacchSetRAN(sacch, ran);
        nxdnSacchSetStructure(sacch, order[part]);
        nxdnSacchSetData(sacch, chunk);
        nxdnSacchEncode(sacch, frame);

        /* four voice blocks: test data, or 49 bit AMBE blocks of 7 bytes */
        memset(net, 0x00, sizeof(net));
        if (ambe != NULL)
        {
            const uint8_t* b = ambe + (size_t)f * 4U * 7U;

            putBlock(net, 0U,   b + 0U);
            putBlock(net, 49U,  b + 7U);
            putBlock(net + 14U, 0U,  b + 14U);
            putBlock(net + 14U, 49U, b + 21U);
        }
        else
        {
            for (unsigned int i = 0U; i < 28U; i++)
                net[i] = (uint8_t)rngNext(&seed);
        }

        nxdnAudioEncode(net, frame + NXDN_FSW_LICH_SACCH_LENGTH_BYTES + 0U);
        nxdnAudioEncode(net + 14U, frame + NXDN_FSW_LICH_SACCH_LENGTH_BYTES + 18U);

        nxdnScramble(frame);
    }

    l3Call(l3, NXDN_MESSAGE_TYPE_TX_REL, src, dst, group);
    facchFrame(ran, l3, frames[n++]);

    return n;
}

unsigned int nxdnGenVoiceCall(uint8_t ran, uint16_t src, uint16_t dst, bool group, unsigned int voiceFrames,
                              uint8_t frames[][48], unsigned int max, uint32_t seed)
{
    return nxdnGenVoiceCallAudio(ran, src, dst, group, voiceFrames, NULL, frames, max, seed);
}

unsigned int nxdnGenDataCall(uint8_t ran, uint16_t src, uint16_t dst, bool group, unsigned int blocks,
                             uint8_t frames[][48], unsigned int max, uint32_t seed)
{
    unsigned int n = 0U;

    if (max < blocks + 2U)
        return 0U;

    for (unsigned int f = 0U; f < blocks + 2U; f++)
    {
        uint8_t udch[23];
        uint8_t* frame = frames[n++];
        uint8_t type;

        if (f == 0U)
            type = NXDN_MESSAGE_TYPE_DCALL_HDR;
        else if (f == blocks + 1U)
            type = NXDN_MESSAGE_TYPE_TX_REL;
        else
            type = NXDN_MESSAGE_TYPE_DCALL_DATA;

        memset(udch, 0x00, sizeof(udch));
        udch[0] = ran;
        l3Call(udch + 1U, type, src, dst, group);

        if (type == NXDN_MESSAGE_TYPE_DCALL_HDR)
            udch[1U + 8U] = (uint8_t)(blocks > 0U ? blocks - 1U : 0U) & 0x0FU;

        if (type == NXDN_MESSAGE_TYPE_DCALL_DATA)
            for (unsigned int i = 8U; i < 22U; i++)
                udch[1U + i] = (uint8_t)rngNext(&seed);

        memset(frame, 0x00, NXDN_FRAME_LENGTH_BYTES);
        nxdnAddSync(frame);
        nxdnLichEncode(nxdnLichMake(NXDN_LICH_RFCT_RDCH, NXDN_LICH_USC_UDCH, NXDN_LICH_STEAL_NONE,
                                    NXDN_LICH_DIRECTION_INBOUND), frame);
        nxdnUdchEncode(udch, frame);
        nxdnScramble(frame);
    }

    return n;
}
