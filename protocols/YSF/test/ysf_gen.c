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

#include "ysf_gen.h"

#define FL YSF_FRAME_LENGTH_BYTES

static const uint8_t WHITE[] = {0x93U, 0xD7U, 0x51U, 0x21U, 0x9CU, 0x2FU, 0x6CU, 0xD0U, 0xEFU, 0x0FU, 0xF8U, 0x3DU, 0xF1U};

static uint32_t g_seed = 1U;

static uint32_t rnd(void)
{
    g_seed = g_seed * 1664525U + 1013904223U;
    return g_seed >> 8;
}

static void pad(uint8_t* d, const char* s)
{
    size_t n = strlen(s);

    memset(d, ' ', YSF_CALLSIGN_LENGTH);
    memcpy(d, s, n > YSF_CALLSIGN_LENGTH ? YSF_CALLSIGN_LENGTH : n);
}

static void mkHeader(uint8_t* f, uint8_t fi, uint8_t dt, uint8_t dgid, const char* dest, const char* src)
{
    uint8_t csd1[20], csd2[20];
    YsfFich h;

    memset(f, 0, FL);
    ysfAddSync(f);
    ysfFichInit(&h);
    ysfFichSetFI(&h, fi);
    ysfFichSetCM(&h, YSF_CM_INDIVIDUAL);
    ysfFichSetFT(&h, 7);
    ysfFichSetDT(&h, dt);
    ysfFichSetDGId(&h, dgid);
    ysfFichEncode(&h, f);
    pad(csd1, dest);
    pad(csd1 + 10, src);
    pad(csd2, "");
    pad(csd2 + 10, "");
    ysfPayloadWriteHeader(f, csd1, csd2);
}

/* The short data channel of a V/D mode 2 frame: ten bytes, whitened, CRC, rate 1/2 code, interleaved */
static void mkShort(uint8_t* frame, const uint8_t* ten)
{
    uint8_t out[13] = {0}, conv[25], bytes[25];

    memset(bytes, 0, sizeof(bytes));
    for (int i = 0; i < 10; i++)
        out[i] = ten[i] ^ WHITE[i];
    CRC_addCCITT162(out, 12);
    out[12] = 0;
    ysfConvEncode(out, conv, 100);

    unsigned int j = 0U;
    for (unsigned int i = 0U; i < 100U; i++)
    {
        unsigned int n = 2U * (i / 5U) + 40U * (i % 5U);
        int s0 = (conv[j >> 3] >> (7 - (j & 7))) & 1; j++;
        int s1 = (conv[j >> 3] >> (7 - (j & 7))) & 1; j++;
        if (s0) bytes[n >> 3] |= (uint8_t)(0x80 >> (n & 7));
        n++;
        if (s1) bytes[n >> 3] |= (uint8_t)(0x80 >> (n & 7));
    }
    for (int i = 0; i < 5; i++)
        memcpy(frame + 30 + i * 18, bytes + i * 5, 5);
}

static void mkVd2(uint8_t* f, uint8_t fn, uint8_t dgid, const char* dest, const char* src, const uint8_t* blocks)
{
    YsfFich h;
    uint8_t ten[10];

    for (int i = 0; i < FL; i++)
        f[i] = (uint8_t)rnd();
    ysfAddSync(f);
    ysfFichInit(&h);
    ysfFichSetFI(&h, YSF_FI_COMMUNICATIONS);
    ysfFichSetCM(&h, YSF_CM_INDIVIDUAL);
    ysfFichSetFN(&h, fn);
    ysfFichSetFT(&h, 6);
    ysfFichSetDT(&h, YSF_DT_VD_MODE2);
    ysfFichSetDGId(&h, dgid);
    ysfFichEncode(&h, f);

    memset(ten, ' ', 10);
    if (fn == 0U) pad(ten, dest);
    else if (fn == 1U) pad(ten, src);
    mkShort(f, ten);

    for (int j = 0; j < 5; j++)
    {
        unsigned int offset = 40U + 144U * (unsigned int)j;
        uint8_t vch[13] = {0};

        if (blocks != NULL)
        {
            /* the 49 bits of the vocoder: the first 27 sent three times, the next 22 once, then a bit that is always 0 */
            const uint8_t* u = blocks + 7 * j;

            for (int i = 0; i < 27; i++)
                if ((u[i >> 3] >> (7 - (i & 7))) & 1)
                    for (int k = 0; k < 3; k++)
                        vch[(3 * i + k) >> 3] |= (uint8_t)(0x80 >> ((3 * i + k) & 7));
            for (int i = 0; i < 22; i++)
                if ((u[(27 + i) >> 3] >> (7 - ((27 + i) & 7))) & 1)
                    vch[(81 + i) >> 3] |= (uint8_t)(0x80 >> ((81 + i) & 7));
        }
        else
        {
            for (int i = 0; i < 81; i += 3)
            {
                int b = (int)(rnd() & 1U);
                for (int k = 0; k < 3; k++)
                    if (b) vch[(i + k) >> 3] |= (uint8_t)(0x80 >> ((i + k) & 7));
            }
        }
        for (int i = 0; i < 13; i++)
            vch[i] ^= WHITE[i];
        for (int i = 0; i < 104; i++)
        {
            unsigned int n = (unsigned int)(i / 26) + 4U * (unsigned int)(i % 26);
            unsigned int p = offset + n;
            int s = (vch[i >> 3] >> (7 - (i & 7))) & 1;
            if (s) f[30 + (p >> 3)] |= (uint8_t)(0x80 >> (p & 7)); else f[30 + (p >> 3)] &= (uint8_t)~(0x80 >> (p & 7));
        }
    }
}

unsigned int ysfGenVoiceCallAudio(const char* dest, const char* src, uint8_t dgid, unsigned int frames, const uint8_t* ambe,
                                  uint8_t out[][YSF_FRAME_LENGTH_BYTES], unsigned int max)
{
    unsigned int k = 0U;

    if (frames + 2U > max)
        return 0U;

    g_seed = 1U;
    mkHeader(out[k++], YSF_FI_HEADER, YSF_DT_VD_MODE2, dgid, dest, src);
    for (unsigned int i = 0U; i < frames; i++, k++)
        mkVd2(out[k], (uint8_t)(i % 7U), dgid, dest, src, ambe != NULL ? ambe + (size_t)i * 35U : NULL);
    mkHeader(out[k++], YSF_FI_TERMINATOR, YSF_DT_VD_MODE2, dgid, dest, src);

    return k;
}

unsigned int ysfGenVoiceCall(const char* dest, const char* src, uint8_t dgid, unsigned int frames,
                             uint8_t out[][YSF_FRAME_LENGTH_BYTES], unsigned int max)
{
    return ysfGenVoiceCallAudio(dest, src, dgid, frames, NULL, out, max);
}

unsigned int ysfGenDataCall(const char* dest, const char* src, uint8_t dgid, unsigned int frames,
                            uint8_t out[][YSF_FRAME_LENGTH_BYTES], unsigned int max)
{
    unsigned int k = 0U;

    if (frames + 2U > max)
        return 0U;

    g_seed = 1U;
    mkHeader(out[k++], YSF_FI_HEADER, YSF_DT_DATA_FR_MODE, dgid, dest, src);
    for (unsigned int i = 0U; i < frames; i++, k++)
    {
        uint8_t csd1[20], csd2[20];
        YsfFich h;

        for (int b = 0; b < 20; b++)
        {
            csd1[b] = (uint8_t)rnd();
            csd2[b] = (uint8_t)rnd();
        }
        if (i % 8U == 0U)
        {
            pad(csd1, dest);
            pad(csd1 + 10, src);
        }
        memset(out[k], 0, FL);
        ysfAddSync(out[k]);
        ysfFichInit(&h);
        ysfFichSetFI(&h, YSF_FI_COMMUNICATIONS);
        ysfFichSetCM(&h, YSF_CM_INDIVIDUAL);
        ysfFichSetFN(&h, (uint8_t)((i + 1U) % 8U));
        ysfFichSetFT(&h, 7);
        ysfFichSetDT(&h, YSF_DT_DATA_FR_MODE);
        ysfFichSetDGId(&h, dgid);
        ysfFichEncode(&h, out[k]);
        ysfPayloadWriteHeader(out[k], csd1, csd2);
    }
    mkHeader(out[k++], YSF_FI_TERMINATOR, YSF_DT_DATA_FR_MODE, dgid, dest, src);

    return k;
}
