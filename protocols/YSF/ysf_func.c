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

#include <stdio.h>
#include <string.h>

#include <dsp_tools.h>

#include "ysf_func.h"

#define READ_BIT1(p, i)     ((p)[(i) >> 3] & BIT_MASK_TABLE[(i) & 7])
#define WRITE_BIT1(p, i, b) ((p)[(i) >> 3] = (b) ? ((p)[(i) >> 3] | BIT_MASK_TABLE[(i) & 7]) : ((p)[(i) >> 3] & (uint8_t)~BIT_MASK_TABLE[(i) & 7]))

static const uint8_t BIT_MASK_TABLE[] = {0x80U, 0x40U, 0x20U, 0x10U, 0x08U, 0x04U, 0x02U, 0x01U};

static const uint8_t YSF_SYNC_BYTES[] = {0xD4U, 0x71U, 0xC9U, 0x63U, 0x4DU};

/* Data whitening of the data channels */
static const uint8_t WHITENING_DATA[] = {0x93U, 0xD7U, 0x51U, 0x21U, 0x9CU, 0x2FU, 0x6CU, 0xD0U, 0xEFU, 0x0FU,
                                         0xF8U, 0x3DU, 0xF1U, 0x73U, 0x20U, 0x94U, 0xEDU, 0x1EU, 0x7CU, 0xD8U};

static unsigned int bitCount(unsigned int v)
{
    unsigned int n = 0U;

    while (v != 0U)
    {
        n += v & 1U;
        v >>= 1;
    }

    return n;
}

/* ------------------------------------------------------------------------ */
/* Interleavers                                                              */
/* ------------------------------------------------------------------------ */

/*
 * The FICH (100 dibits, 5 columns) and the data channels (180 dibits, 9
 * columns) are interleaved by writing the dibits in rows and reading them in
 * columns of a 20 row table: dibit i goes to dibit position 2 * (i / cols) +
 * 40 * (i % cols), which is the bit position given by the table below.
 */
static unsigned int interleave5(unsigned int i)
{
    return 2U * (i / 5U) + 40U * (i % 5U);
}

static unsigned int interleave9(unsigned int i)
{
    return 2U * (i / 9U) + 40U * (i % 9U);
}

/* V/D mode 2 voice channel: 26 columns, bit interleaved */
static unsigned int interleave26(unsigned int i)
{
    return (i / 26U) + 4U * (i % 26U);
}

/* ------------------------------------------------------------------------ */
/* Convolutional code                                                        */
/* ------------------------------------------------------------------------ */

#define CONV_STATES_D2  8U
#define CONV_STATES     16U
#define CONV_M          2U
#define CONV_K          5U

static const uint8_t BRANCH_TABLE1[] = {0U, 0U, 0U, 0U, 1U, 1U, 1U, 1U};
static const uint8_t BRANCH_TABLE2[] = {0U, 1U, 1U, 0U, 0U, 1U, 1U, 0U};

typedef struct
{
    uint16_t metrics1[20];
    uint16_t metrics2[20];
    uint16_t* oldMetrics;
    uint16_t* newMetrics;
    uint64_t decisions[180];
    unsigned int dp;
} Viterbi;

static void viterbiStart(Viterbi* v)
{
    memset(v->metrics1, 0x00, CONV_STATES * sizeof(uint16_t));
    memset(v->metrics2, 0x00, CONV_STATES * sizeof(uint16_t));

    v->oldMetrics = v->metrics1;
    v->newMetrics = v->metrics2;
    v->dp         = 0U;
}

static void viterbiDecode(Viterbi* v, uint8_t s0, uint8_t s1)
{
    uint64_t dec = 0U;

    for (uint8_t i = 0U; i < CONV_STATES_D2; i++)
    {
        uint8_t j = (uint8_t)(i * 2U);
        uint16_t metric = (uint16_t)((BRANCH_TABLE1[i] ^ s0) + (BRANCH_TABLE2[i] ^ s1));

        uint16_t m0 = (uint16_t)(v->oldMetrics[i] + metric);
        uint16_t m1 = (uint16_t)(v->oldMetrics[i + CONV_STATES_D2] + (CONV_M - metric));
        uint8_t decision0 = (m0 >= m1) ? 1U : 0U;
        v->newMetrics[j + 0U] = decision0 != 0U ? m1 : m0;

        m0 = (uint16_t)(v->oldMetrics[i] + (CONV_M - metric));
        m1 = (uint16_t)(v->oldMetrics[i + CONV_STATES_D2] + metric);
        uint8_t decision1 = (m0 >= m1) ? 1U : 0U;
        v->newMetrics[j + 1U] = decision1 != 0U ? m1 : m0;

        dec |= ((uint64_t)decision1 << (j + 1U)) | ((uint64_t)decision0 << (j + 0U));
    }

    v->decisions[v->dp++] = dec;

    uint16_t* tmp = v->oldMetrics;
    v->oldMetrics = v->newMetrics;
    v->newMetrics = tmp;
}

static unsigned int viterbiChainback(Viterbi* v, uint8_t* out, unsigned int nBits)
{
    uint32_t state = 0U;

    while (nBits-- > 0U)
    {
        --v->dp;

        uint32_t i  = state >> (9U - CONV_K);
        uint8_t bit = (uint8_t)((v->decisions[v->dp] >> i) & 1U);
        state = ((uint32_t)bit << 7) | (state >> 1);

        WRITE_BIT1(out, nBits, bit != 0U);
    }

    unsigned int minCost = v->oldMetrics[0];

    for (unsigned int i = 0U; i < CONV_STATES; i++)
        if (v->oldMetrics[i] < minCost)
            minCost = v->oldMetrics[i];

    return minCost / (CONV_M >> 1);
}

/*
 * ysfConvEncode()
 *   Encodes nBits bits of 'in' (most significant bit first) into 2 * nBits bits.
 */
void ysfConvEncode(const uint8_t* in, uint8_t* out, unsigned int nBits)
{
    uint8_t d1 = 0U, d2 = 0U, d3 = 0U, d4 = 0U;
    uint32_t k = 0U;

    for (unsigned int i = 0U; i < nBits; i++)
    {
        uint8_t d = READ_BIT1(in, i) ? 1U : 0U;

        uint8_t g1 = (uint8_t)((d + d3 + d4) & 1U);
        uint8_t g2 = (uint8_t)((d + d1 + d2 + d4) & 1U);

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

/* ------------------------------------------------------------------------ */
/* Sync                                                                      */
/* ------------------------------------------------------------------------ */

void ysfAddSync(uint8_t* frame)
{
    memcpy(frame, YSF_SYNC_BYTES, YSF_SYNC_LENGTH_BYTES);
}

bool ysfHasSync(const uint8_t* frame, unsigned int maxErrors)
{
    unsigned int errs = 0U;

    for (unsigned int i = 0U; i < YSF_SYNC_LENGTH_BYTES; i++)
        errs += bitCount((unsigned int)(frame[i] ^ YSF_SYNC_BYTES[i]));

    return errs <= maxErrors;
}

/* ------------------------------------------------------------------------ */
/* Golay (24,12)                                                             */
/* ------------------------------------------------------------------------ */

/*
 * golayDecode()
 *   Decodes a 24 bit extended Golay word the way MMDVMHost does: the word is
 *   accepted when the syndrome is light or the corrected word has even weight.
 *   The 12 data bits are returned in *data.
 */
static bool golayDecode(const uint8_t* bytes, unsigned int* data)
{
    unsigned int word      = ((unsigned int)bytes[0] << 16) | ((unsigned int)bytes[1] << 8) | bytes[2];
    unsigned int syndrome  = get_syndrome_23127(word >> 1);
    unsigned int corrected = (golay24128_correct23127(word >> 1) << 1) | (word & 1U);

    *data = corrected >> 12;

    return bitCount(syndrome) < 3U || (bitCount(corrected) & 1U) == 0U;
}

/* ------------------------------------------------------------------------ */
/* FICH                                                                      */
/* ------------------------------------------------------------------------ */

void ysfFichInit(YsfFich* f)
{
    memset(f->b, 0x00, sizeof(f->b));
}

bool ysfFichDecode(YsfFich* f, const uint8_t* frame)
{
    Viterbi v;
    uint8_t output[13];
    unsigned int b0, b1, b2, b3;

    frame += YSF_SYNC_LENGTH_BYTES;

    viterbiStart(&v);

    for (unsigned int i = 0U; i < 100U; i++)
    {
        unsigned int n = interleave5(i);
        uint8_t s0 = READ_BIT1(frame, n) ? 1U : 0U;

        n++;
        uint8_t s1 = READ_BIT1(frame, n) ? 1U : 0U;

        viterbiDecode(&v, s0, s1);
    }

    memset(output, 0x00, sizeof(output));
    viterbiChainback(&v, output, 96U);

    bool valid0 = golayDecode(output + 0U, &b0);
    bool valid1 = golayDecode(output + 3U, &b1);
    bool valid2 = golayDecode(output + 6U, &b2);
    bool valid3 = golayDecode(output + 9U, &b3);

    if (!valid0 || !valid1 || !valid2 || !valid3)
        return false;

    f->b[0] = (uint8_t)((b0 >> 4) & 0xFFU);
    f->b[1] = (uint8_t)(((b0 << 4) & 0xF0U) | ((b1 >> 8) & 0x0FU));
    f->b[2] = (uint8_t)((b1 >> 0) & 0xFFU);
    f->b[3] = (uint8_t)((b2 >> 4) & 0xFFU);
    f->b[4] = (uint8_t)(((b2 << 4) & 0xF0U) | ((b3 >> 8) & 0x0FU));
    f->b[5] = (uint8_t)((b3 >> 0) & 0xFFU);

    return CRC_checkCCITT162(f->b, 6U);
}

void ysfFichEncode(YsfFich* f, uint8_t* frame)
{
    uint8_t conv[13];
    uint8_t convolved[25];

    frame += YSF_SYNC_LENGTH_BYTES;

    CRC_addCCITT162(f->b, 6U);

    unsigned int b0 = (((unsigned int)f->b[0] << 4) & 0xFF0U) | (((unsigned int)f->b[1] >> 4) & 0x00FU);
    unsigned int b1 = (((unsigned int)f->b[1] << 8) & 0xF00U) | (((unsigned int)f->b[2] >> 0) & 0x0FFU);
    unsigned int b2 = (((unsigned int)f->b[3] << 4) & 0xFF0U) | (((unsigned int)f->b[4] >> 4) & 0x00FU);
    unsigned int b3 = (((unsigned int)f->b[4] << 8) & 0xF00U) | (((unsigned int)f->b[5] >> 0) & 0x0FFU);

    unsigned int c0 = golay24128_encode24128(b0);
    unsigned int c1 = golay24128_encode24128(b1);
    unsigned int c2 = golay24128_encode24128(b2);
    unsigned int c3 = golay24128_encode24128(b3);

    conv[0]  = (uint8_t)((c0 >> 16) & 0xFFU);
    conv[1]  = (uint8_t)((c0 >> 8) & 0xFFU);
    conv[2]  = (uint8_t)((c0 >> 0) & 0xFFU);
    conv[3]  = (uint8_t)((c1 >> 16) & 0xFFU);
    conv[4]  = (uint8_t)((c1 >> 8) & 0xFFU);
    conv[5]  = (uint8_t)((c1 >> 0) & 0xFFU);
    conv[6]  = (uint8_t)((c2 >> 16) & 0xFFU);
    conv[7]  = (uint8_t)((c2 >> 8) & 0xFFU);
    conv[8]  = (uint8_t)((c2 >> 0) & 0xFFU);
    conv[9]  = (uint8_t)((c3 >> 16) & 0xFFU);
    conv[10] = (uint8_t)((c3 >> 8) & 0xFFU);
    conv[11] = (uint8_t)((c3 >> 0) & 0xFFU);
    conv[12] = 0x00U;

    ysfConvEncode(conv, convolved, 100U);

    unsigned int j = 0U;

    for (unsigned int i = 0U; i < 100U; i++)
    {
        unsigned int n = interleave5(i);

        bool s0 = READ_BIT1(convolved, j) != 0U;
        j++;
        bool s1 = READ_BIT1(convolved, j) != 0U;
        j++;

        WRITE_BIT1(frame, n, s0);
        n++;
        WRITE_BIT1(frame, n, s1);
    }
}

uint8_t ysfFichGetFI(const YsfFich* f)   { return (f->b[0] >> 6) & 0x03U; }
uint8_t ysfFichGetCM(const YsfFich* f)   { return (f->b[0] >> 2) & 0x03U; }
uint8_t ysfFichGetBN(const YsfFich* f)   { return f->b[0] & 0x03U; }
uint8_t ysfFichGetBT(const YsfFich* f)   { return (f->b[1] >> 6) & 0x03U; }
uint8_t ysfFichGetFN(const YsfFich* f)   { return (f->b[1] >> 3) & 0x07U; }
uint8_t ysfFichGetFT(const YsfFich* f)   { return f->b[1] & 0x07U; }
uint8_t ysfFichGetDT(const YsfFich* f)   { return f->b[2] & 0x03U; }
uint8_t ysfFichGetMR(const YsfFich* f)   { return (f->b[2] >> 3) & 0x03U; }
bool    ysfFichGetDev(const YsfFich* f)  { return (f->b[2] & 0x40U) == 0x40U; }
uint8_t ysfFichGetDGId(const YsfFich* f) { return f->b[3] & 0x7FU; }

void ysfFichSetFI(YsfFich* f, uint8_t fi)   { f->b[0] = (uint8_t)((f->b[0] & 0x3FU) | ((fi << 6) & 0xC0U)); }
void ysfFichSetCM(YsfFich* f, uint8_t cm)   { f->b[0] = (uint8_t)((f->b[0] & 0xF3U) | ((cm << 2) & 0x0CU)); }
void ysfFichSetBN(YsfFich* f, uint8_t bn)   { f->b[0] = (uint8_t)((f->b[0] & 0xFCU) | (bn & 0x03U)); }
void ysfFichSetBT(YsfFich* f, uint8_t bt)   { f->b[1] = (uint8_t)((f->b[1] & 0x3FU) | ((bt << 6) & 0xC0U)); }
void ysfFichSetFN(YsfFich* f, uint8_t fn)   { f->b[1] = (uint8_t)((f->b[1] & 0xC7U) | ((fn << 3) & 0x38U)); }
void ysfFichSetFT(YsfFich* f, uint8_t ft)   { f->b[1] = (uint8_t)((f->b[1] & 0xF8U) | (ft & 0x07U)); }
void ysfFichSetDT(YsfFich* f, uint8_t dt)   { f->b[2] = (uint8_t)((f->b[2] & 0xFCU) | (dt & 0x03U)); }
void ysfFichSetMR(YsfFich* f, uint8_t mr)   { f->b[2] = (uint8_t)((f->b[2] & 0xC7U) | ((mr << 3) & 0x38U)); }
void ysfFichSetVoIP(YsfFich* f, bool on)    { if (on) f->b[2] |= 0x04U; else f->b[2] &= 0xFBU; }
void ysfFichSetDev(YsfFich* f, bool on)     { if (on) f->b[2] |= 0x40U; else f->b[2] &= 0xBFU; }
void ysfFichSetDGId(YsfFich* f, uint8_t id) { f->b[3] = (uint8_t)((f->b[3] & 0x80U) | (id & 0x7FU)); }

/* ------------------------------------------------------------------------ */
/* Payload                                                                   */
/* ------------------------------------------------------------------------ */

#define PAYLOAD_OFFSET (YSF_SYNC_LENGTH_BYTES + YSF_FICH_LENGTH_BYTES)

/* Collect the 'n' byte pieces of a data channel, which sit every 18 bytes */
static void dchGather(const uint8_t* data, uint8_t* dch, unsigned int n)
{
    for (unsigned int i = 0U; i < 5U; i++)
    {
        memcpy(dch + i * n, data + i * 18U, n);
    }
}

static void dchScatter(uint8_t* data, const uint8_t* dch, unsigned int n)
{
    for (unsigned int i = 0U; i < 5U; i++)
    {
        memcpy(data + i * 18U, dch + i * n, n);
    }
}

/*
 * Decodes a long data channel (20 bytes, 9 bytes in each of 5 pieces) into
 * 'output' (23 bytes).  Returns true when the CRC is good.
 */
static bool dchDecode20(const uint8_t* dch, uint8_t* output)
{
    Viterbi v;

    viterbiStart(&v);

    for (unsigned int i = 0U; i < 180U; i++)
    {
        unsigned int n = interleave9(i);
        uint8_t s0 = READ_BIT1(dch, n) ? 1U : 0U;

        n++;
        uint8_t s1 = READ_BIT1(dch, n) ? 1U : 0U;

        viterbiDecode(&v, s0, s1);
    }

    viterbiChainback(&v, output, 176U);

    return CRC_checkCCITT162(output, 22U);
}

/* The reverse: 'output' holds 20 whitened bytes, the CRC is added here. */
static void dchEncode20(uint8_t* output, uint8_t* bytes)
{
    uint8_t convolved[45];

    CRC_addCCITT162(output, 22U);
    output[22] = 0x00U;

    ysfConvEncode(output, convolved, 180U);

    unsigned int j = 0U;

    for (unsigned int i = 0U; i < 180U; i++)
    {
        unsigned int n = interleave9(i);

        bool s0 = READ_BIT1(convolved, j) != 0U;
        j++;
        bool s1 = READ_BIT1(convolved, j) != 0U;
        j++;

        WRITE_BIT1(bytes, n, s0);
        n++;
        WRITE_BIT1(bytes, n, s1);
    }
}

static void unwhiten20(uint8_t* output)
{
    for (unsigned int i = 0U; i < 20U; i++)
        output[i] ^= WHITENING_DATA[i];
}

void ysfPayloadInit(YsfPayload* p)
{
    memset(p, 0x00, sizeof(*p));
}

static void setCall(uint8_t* dest, bool* has, const char* callsign)
{
    memset(dest, ' ', YSF_CALLSIGN_LENGTH);

    for (unsigned int i = 0U; i < YSF_CALLSIGN_LENGTH && callsign[i] != '\0'; i++)
        dest[i] = (uint8_t)callsign[i];

    *has = true;
}

void ysfPayloadSetUplink(YsfPayload* p, const char* callsign)
{
    setCall(p->uplink, &p->hasUplink, callsign);
}

void ysfPayloadSetDownlink(YsfPayload* p, const char* callsign)
{
    setCall(p->downlink, &p->hasDownlink, callsign);
}

void ysfPayloadReset(YsfPayload* p)
{
    p->hasSource = false;
    p->hasDest   = false;
}

static void takeDest(YsfPayload* p, const uint8_t* src)
{
    if (!p->hasDest)
    {
        memcpy(p->dest, src, YSF_CALLSIGN_LENGTH);
        p->hasDest = true;
    }
}

static void takeSource(YsfPayload* p, const uint8_t* src)
{
    if (!p->hasSource)
    {
        memcpy(p->source, src, YSF_CALLSIGN_LENGTH);
        p->hasSource = true;
    }
}

static void putGateway(const YsfPayload* p, uint8_t* output)
{
    if (p->hasDownlink)
        memcpy(output + 0U, p->downlink, YSF_CALLSIGN_LENGTH);

    if (p->hasUplink)
        memcpy(output + YSF_CALLSIGN_LENGTH, p->uplink, YSF_CALLSIGN_LENGTH);
}

/* Header and terminator: CSD1 in the first half, CSD2 in the second */
bool ysfPayloadHeader(YsfPayload* p, uint8_t* frame)
{
    uint8_t* data = frame + PAYLOAD_OFFSET;
    uint8_t dch[45], output[23], bytes[45];

    dchGather(data, dch, 9U);

    bool valid1 = dchDecode20(dch, output);

    if (valid1)
    {
        unwhiten20(output);

        takeDest(p, output + 0U);
        takeSource(p, output + YSF_CALLSIGN_LENGTH);

        unwhiten20(output);
        dchEncode20(output, bytes);
        dchScatter(data, bytes, 9U);
    }

    dchGather(data + 9U, dch, 9U);

    bool valid2 = dchDecode20(dch, output);

    if (valid2)
    {
        unwhiten20(output);
        putGateway(p, output);
        unwhiten20(output);
        dchEncode20(output, bytes);
        dchScatter(data + 9U, bytes, 9U);
    }

    return valid1;
}

bool ysfPayloadVDMode1Data(YsfPayload* p, uint8_t* frame, uint8_t fn, bool gateway)
{
    uint8_t* data = frame + PAYLOAD_OFFSET;
    uint8_t dch[45], output[23], bytes[45];

    dchGather(data, dch, 9U);

    bool ret = dchDecode20(dch, output);

    if (ret)
    {
        unwhiten20(output);

        switch (fn)
        {
        case 0U:
            takeDest(p, output + 0U);
            takeSource(p, output + YSF_CALLSIGN_LENGTH);
            break;

        case 1U:
            if (!gateway)
                putGateway(p, output);
            break;

        default:
            break;
        }

        unwhiten20(output);
        dchEncode20(output, bytes);
        dchScatter(data, bytes, 9U);
    }

    return ret && (fn == 0U);
}

bool ysfPayloadDataFRData(YsfPayload* p, uint8_t* frame, uint8_t fn, bool gateway)
{
    uint8_t* data = frame + PAYLOAD_OFFSET;
    uint8_t dch[45], output[23], bytes[45];

    dchGather(data, dch, 9U);

    bool ret1 = dchDecode20(dch, output);

    if (ret1)
    {
        unwhiten20(output);

        if (fn == 0U)
        {
            takeDest(p, output + 0U);
            takeSource(p, output + YSF_CALLSIGN_LENGTH);
        }

        unwhiten20(output);
        dchEncode20(output, bytes);
        dchScatter(data, bytes, 9U);
    }

    dchGather(data + 9U, dch, 9U);

    bool ret2 = dchDecode20(dch, output);

    if (ret2)
    {
        unwhiten20(output);

        if (fn == 0U && !gateway)
            putGateway(p, output);

        unwhiten20(output);
        dchEncode20(output, bytes);
        dchScatter(data + 9U, bytes, 9U);
    }

    return ret1 && (fn == 0U);
}

/* V/D mode 2 has a short data channel: 10 bytes, 5 bytes in each piece */
bool ysfPayloadVDMode2Data(YsfPayload* p, uint8_t* frame, uint8_t fn, bool gateway)
{
    uint8_t* data = frame + PAYLOAD_OFFSET;
    uint8_t dch[25], output[13], convolved[25], bytes[25];
    Viterbi v;

    dchGather(data, dch, 5U);

    viterbiStart(&v);

    for (unsigned int i = 0U; i < 100U; i++)
    {
        unsigned int n = interleave5(i);
        uint8_t s0 = READ_BIT1(dch, n) ? 1U : 0U;

        n++;
        uint8_t s1 = READ_BIT1(dch, n) ? 1U : 0U;

        viterbiDecode(&v, s0, s1);
    }

    viterbiChainback(&v, output, 96U);

    bool ret = CRC_checkCCITT162(output, 12U);

    if (ret)
    {
        for (unsigned int i = 0U; i < 10U; i++)
            output[i] ^= WHITENING_DATA[i];

        switch (fn)
        {
        case 0U:
            takeDest(p, output);
            break;

        case 1U:
            takeSource(p, output);
            break;

        case 2U:
            if (p->hasDownlink && !gateway)
                memcpy(output, p->downlink, YSF_CALLSIGN_LENGTH);
            break;

        case 3U:
            if (p->hasUplink && !gateway)
                memcpy(output, p->uplink, YSF_CALLSIGN_LENGTH);
            break;

        default:
            break;
        }

        for (unsigned int i = 0U; i < 10U; i++)
            output[i] ^= WHITENING_DATA[i];

        CRC_addCCITT162(output, 12U);
        output[12] = 0x00U;

        ysfConvEncode(output, convolved, 100U);

        unsigned int j = 0U;

        for (unsigned int i = 0U; i < 100U; i++)
        {
            unsigned int n = interleave5(i);

            bool s0 = READ_BIT1(convolved, j) != 0U;
            j++;
            bool s1 = READ_BIT1(convolved, j) != 0U;
            j++;

            WRITE_BIT1(bytes, n, s0);
            n++;
            WRITE_BIT1(bytes, n, s1);
        }

        dchScatter(data, bytes, 5U);
    }

    return ret && (fn == 0U || fn == 1U);
}

/* V/D mode 2 audio: each bit of the voice channels is sent three times */
unsigned int ysfPayloadVDMode2Audio(YsfPayload* p, uint8_t* frame)
{
    uint8_t* data = frame + PAYLOAD_OFFSET;
    unsigned int errors = 0U;
    unsigned int offset = 40U;

    (void)p;

    for (unsigned int j = 0U; j < 5U; j++, offset += 144U)
    {
        unsigned int errs = 0U;
        uint8_t vch[13];

        for (unsigned int i = 0U; i < 104U; i++)
        {
            unsigned int n = interleave26(i);
            bool s = READ_BIT1(data, offset + n) != 0U;
            WRITE_BIT1(vch, i, s);
        }

        for (unsigned int i = 0U; i < 13U; i++)
            vch[i] ^= WHITENING_DATA[i];

        for (unsigned int i = 0U; i < 81U; i += 3U)
        {
            uint8_t vote = 0U;

            vote += READ_BIT1(vch, i + 0U) ? 1U : 0U;
            vote += READ_BIT1(vch, i + 1U) ? 1U : 0U;
            vote += READ_BIT1(vch, i + 2U) ? 1U : 0U;

            if (vote == 1U || vote == 2U)
            {
                bool v = (vote == 2U);

                WRITE_BIT1(vch, i + 0U, v);
                WRITE_BIT1(vch, i + 1U, v);
                WRITE_BIT1(vch, i + 2U, v);
                errs++;
            }
        }

        if (errs > 0U)
        {
            errors += errs;

            for (unsigned int i = 0U; i < 13U; i++)
                vch[i] ^= WHITENING_DATA[i];

            for (unsigned int i = 0U; i < 104U; i++)
            {
                unsigned int n = interleave26(i);
                bool s = READ_BIT1(vch, i) != 0U;
                WRITE_BIT1(data, offset + n, s);
            }
        }
    }

    return errors;
}

/* Voice FR mode: the whole 180 dibits of the data channel are contiguous */
bool ysfPayloadVoiceFRData(YsfPayload* p, uint8_t* frame)
{
    uint8_t* data = frame + PAYLOAD_OFFSET;
    uint8_t output[23], bytes[45];

    (void)p;

    if (!dchDecode20(data, output))
        return false;

    dchEncode20(output, bytes);
    memcpy(data, bytes, 45U);

    return true;
}

static void writeCsd(const uint8_t* csd, uint8_t* data)
{
    uint8_t output[25], bytes[45];

    for (unsigned int i = 0U; i < 20U; i++)
        output[i] = csd[i] ^ WHITENING_DATA[i];

    dchEncode20(output, bytes);
    dchScatter(data, bytes, 9U);
}

bool ysfPayloadReadCsd(const uint8_t* frame, unsigned int half, uint8_t* csd)
{
    uint8_t dch[45], output[25];

    dchGather(frame + PAYLOAD_OFFSET + (half != 0U ? 9U : 0U), dch, 9U);

    if (!dchDecode20(dch, output))
    {
        memset(csd, ' ', 20U);
        return false;
    }

    unwhiten20(output);
    memcpy(csd, output, 20U);

    return true;
}

void ysfPayloadWriteCsd(uint8_t* frame, unsigned int half, const uint8_t* csd)
{
    writeCsd(csd, frame + PAYLOAD_OFFSET + (half != 0U ? 9U : 0U));
}

void ysfPayloadWriteHeader(uint8_t* frame, const uint8_t* csd1, const uint8_t* csd2)
{
    writeCsd(csd1, frame + PAYLOAD_OFFSET);
    writeCsd(csd2, frame + PAYLOAD_OFFSET + 9U);
}

/* ------------------------------------------------------------------------ */
/* Control                                                                   */
/* ------------------------------------------------------------------------ */

static const char BLANK_CALL[] = "          ";

void ysfCtrlSetCallsign(YsfCtrl* c, const char* callsign)
{
    memset(c->callsign, ' ', YSF_CALLSIGN_LENGTH);
    c->callsign[YSF_CALLSIGN_LENGTH] = '\0';

    for (unsigned int i = 0U; i < YSF_CALLSIGN_LENGTH && callsign[i] != '\0'; i++)
        c->callsign[i] = callsign[i];

    ysfPayloadSetUplink(&c->rfPayload, c->callsign);
    ysfPayloadSetDownlink(&c->rfPayload, c->callsign);
    ysfPayloadSetDownlink(&c->netPayload, c->callsign);
}

void ysfCtrlInit(YsfCtrl* c)
{
    memset(c, 0x00, sizeof(*c));

    c->rfTimeoutMs   = 180000U;
    c->netWatchdogMs = 1500U;
    c->rfState       = YSF_RF_LISTENING;
    c->netState      = YSF_NET_IDLE;
    c->rfBits        = 1U;

    ysfFichInit(&c->lastFich);
    ysfPayloadInit(&c->rfPayload);
    ysfPayloadInit(&c->netPayload);
    memset(c->rfSource, ' ', YSF_CALLSIGN_LENGTH);
    memset(c->rfDest, ' ', YSF_CALLSIGN_LENGTH);
    memset(c->netSource, ' ', YSF_CALLSIGN_LENGTH);
    memset(c->netDest, ' ', YSF_CALLSIGN_LENGTH);
    memset(c->netGateway, ' ', YSF_CALLSIGN_LENGTH);

    ysfCtrlSetCallsign(c, "");
}

void ysfCtrlReset(YsfCtrl* c)
{
    c->rfState = YSF_RF_LISTENING;
    c->netState = YSF_NET_IDLE;
    c->rfTimerRunning = false;
    c->netTimerRunning = false;
    c->netWatchdogRunning = false;
    c->netAnnounced = false;
    ysfPayloadReset(&c->rfPayload);
    ysfPayloadReset(&c->netPayload);
}

bool ysfCtrlBusy(const YsfCtrl* c)
{
    return c->rfState != YSF_RF_LISTENING || c->netState != YSF_NET_IDLE;
}

static void raise(YsfCtrl* c, YsfEvent ev, const char* source, const char* dest, const char* gateway, uint8_t dgid, uint8_t dt, bool data, float seconds, float ber)
{
    YsfCall call;

    if (c->event == NULL)
        return;

    memset(&call, 0x00, sizeof(call));
    call.event = ev;
    memcpy(call.source, source, YSF_CALLSIGN_LENGTH);
    memcpy(call.dest, dest, YSF_CALLSIGN_LENGTH);
    memcpy(call.gateway, gateway, YSF_CALLSIGN_LENGTH);
    call.dgid    = dgid;
    call.dt      = dt;
    call.data    = data;
    call.seconds = seconds;
    call.ber     = ber;

    c->event(c->ctx, &call);
}

static bool rfExpired(const YsfCtrl* c)
{
    return c->rfTimerRunning && c->rfTimeoutMs > 0U && c->rfTimer >= c->rfTimeoutMs;
}

static bool netExpired(const YsfCtrl* c)
{
    return c->netTimerRunning && c->rfTimeoutMs > 0U && c->netTimer >= c->rfTimeoutMs;
}

/* Send a frame (120 bytes) of the RF call to the gateway */
static void toNet(YsfCtrl* c, const uint8_t* frame, unsigned int count, bool end)
{
    uint8_t body[YSF_NET_BODY_LENGTH];

    if (c->sendNet == NULL || rfExpired(c))
        return;

    memcpy(body + 0U, c->callsign, YSF_CALLSIGN_LENGTH);
    memcpy(body + 10U, c->rfSource, YSF_CALLSIGN_LENGTH);
    memcpy(body + 20U, c->rfDest, YSF_CALLSIGN_LENGTH);
    body[30] = (uint8_t)(((count % 128U) << 1) | (end ? 1U : 0U));
    memcpy(body + 31U, frame, YSF_FRAME_LENGTH_BYTES);

    c->sendNet(c->ctx, body, end);
}

/* Repeat a received frame on RF; only when the network is not already using the transmitter */
static void toRf(YsfCtrl* c, const uint8_t* frame, const YsfFich* fich, bool eot)
{
    uint8_t out[YSF_FRAME_LENGTH_BYTES];
    YsfFich f = *fich;

    if (!c->localRepeat || c->sendRf == NULL || c->netState != YSF_NET_IDLE || rfExpired(c))
        return;

    memcpy(out, frame, YSF_FRAME_LENGTH_BYTES);

    ysfFichSetMR(&f, c->remoteGateway ? YSF_MR_NOT_BUSY : YSF_MR_BUSY);
    ysfFichSetDev(&f, c->lowDeviation);
    ysfFichEncode(&f, out);

    c->sendRf(c->ctx, out, eot);
}

static void endRf(YsfCtrl* c)
{
    c->rfState = YSF_RF_LISTENING;
    c->rfTimerRunning = false;
    ysfPayloadReset(&c->rfPayload);
}

static void endNet(YsfCtrl* c)
{
    c->netState = YSF_NET_IDLE;
    c->netTimerRunning = false;
    c->netWatchdogRunning = false;
    c->netAnnounced = false;
    c->netCsdLogged = false;
    c->netHeldValid = false;
    ysfPayloadReset(&c->netPayload);
}

static bool checkCallsign(const YsfCtrl* c, const char* source)
{
    unsigned int len = YSF_CALLSIGN_LENGTH;

    while (len > 0U && c->callsign[len - 1U] == ' ')
        len--;

    return memcmp(source, c->callsign, len) == 0;
}

static void startRf(YsfCtrl* c, bool data)
{
    c->rfFrames = 0U;
    c->rfErrs   = 0U;
    c->rfBits   = 1U;
    c->rfTimer  = 0U;
    c->rfTimerRunning = true;
    c->rfState  = data ? YSF_RF_DATA : YSF_RF_AUDIO;
}

static void rfSetCalls(YsfCtrl* c, uint8_t cm)
{
    if (c->rfPayload.hasSource)
        memcpy(c->rfSource, c->rfPayload.source, YSF_CALLSIGN_LENGTH);

    if (cm == YSF_CM_GROUP1 || cm == YSF_CM_GROUP2)
        memcpy(c->rfDest, "ALL       ", YSF_CALLSIGN_LENGTH);
    else if (c->rfPayload.hasDest)
        memcpy(c->rfDest, c->rfPayload.dest, YSF_CALLSIGN_LENGTH);
}

static float ratio(unsigned int errs, unsigned int bits)
{
    return bits > 0U ? (float)(errs * 100U) / (float)bits : 0.0F;
}

static void rfHeader(YsfCtrl* c, uint8_t* frame, bool valid, bool data)
{
    (void)valid;

    uint8_t dgid = ysfFichGetDGId(&c->lastFich);
    uint8_t dt   = ysfFichGetDT(&c->lastFich);

    if (c->rfState != YSF_RF_LISTENING)
        return;

    ysfPayloadReset(&c->rfPayload);

    if (!ysfPayloadHeader(&c->rfPayload, frame))
        return;

    memcpy(c->rfSource, c->rfPayload.source, YSF_CALLSIGN_LENGTH);

    if (c->selfOnly && !checkCallsign(c, c->rfSource))
    {
        c->rfState = YSF_RF_REJECTED;
        raise(c, YSF_EV_RF_REJECTED, c->rfSource, BLANK_CALL, c->callsign, dgid, dt, data, 0.0F, 0.0F);
        return;
    }

    rfSetCalls(c, ysfFichGetCM(&c->lastFich));
    startRf(c, data);

    raise(c, YSF_EV_RF_START, c->rfSource, c->rfDest, c->callsign, dgid, dt, data, 0.0F, 0.0F);

    YsfFich fich = c->lastFich;
    ysfAddSync(frame);
    ysfFichEncode(&fich, frame);

    toNet(c, frame, c->rfFrames, false);
    toRf(c, frame, &fich, false);
    c->rfFrames++;
}

static void rfTerminator(YsfCtrl* c, uint8_t* frame, bool data)
{
    uint8_t dgid = ysfFichGetDGId(&c->lastFich);
    uint8_t dt   = ysfFichGetDT(&c->lastFich);

    if (c->rfState == YSF_RF_REJECTED)
    {
        ysfPayloadReset(&c->rfPayload);
        c->rfState = YSF_RF_LISTENING;
        return;
    }

    if (c->rfState != (data ? YSF_RF_DATA : YSF_RF_AUDIO))
        return;

    ysfPayloadHeader(&c->rfPayload, frame);

    YsfFich fich = c->lastFich;
    ysfAddSync(frame);
    ysfFichEncode(&fich, frame);

    toNet(c, frame, c->rfFrames, true);
    toRf(c, frame, &fich, true);
    c->rfFrames++;

    raise(c, YSF_EV_RF_END, c->rfSource, c->rfDest, c->callsign, dgid, dt, data,
          (float)c->rfFrames / 10.0F, ratio(c->rfErrs, c->rfBits));

    endRf(c);
}

/* The frame number of a frame whose FICH could not be decoded follows the one before */
static void guessNext(YsfCtrl* c)
{
    uint8_t ft = ysfFichGetFT(&c->lastFich);
    uint8_t fn = (uint8_t)(ysfFichGetFN(&c->lastFich) + 1U);

    if (fn > ft)
    {
        fn = 0U;

        if (ysfFichGetBT(&c->lastFich) > 0U)
            ysfFichSetBN(&c->lastFich, (uint8_t)(ysfFichGetBN(&c->lastFich) + 1U));
    }

    ysfFichSetFN(&c->lastFich, fn);
}

static void rfVoiceFR(YsfCtrl* c, uint8_t* frame, bool valid)
{
    uint8_t fi = ysfFichGetFI(&c->lastFich);

    if (valid && fi == YSF_FI_HEADER)
    {
        rfHeader(c, frame, valid, false);
    }
    else if (valid && fi == YSF_FI_TERMINATOR)
    {
        rfTerminator(c, frame, false);
    }
    else if (c->rfState == YSF_RF_AUDIO)
    {
        if (!valid)
        {
            ysfFichSetFT(&c->lastFich, 0U);
            ysfFichSetFN(&c->lastFich, 0U);
        }

        ysfAddSync(frame);

        YsfFich fich = c->lastFich;

        if (ysfFichGetFN(&fich) == 0U && ysfFichGetFT(&fich) == 1U)
            ysfPayloadVoiceFRData(&c->rfPayload, frame);

        ysfFichEncode(&fich, frame);

        toNet(c, frame, c->rfFrames, false);
        toRf(c, frame, &fich, false);
        c->rfFrames++;
    }
}

static void rfVoiceDN(YsfCtrl* c, uint8_t* frame, bool valid)
{
    uint8_t fi   = ysfFichGetFI(&c->lastFich);
    uint8_t dgid = ysfFichGetDGId(&c->lastFich);

    if (valid && fi == YSF_FI_HEADER)
    {
        rfHeader(c, frame, valid, false);
    }
    else if (valid && fi == YSF_FI_TERMINATOR)
    {
        rfTerminator(c, frame, false);
    }
    else if (c->rfState == YSF_RF_AUDIO)
    {
        if (!valid)
            guessNext(c);

        ysfAddSync(frame);

        uint8_t fn = ysfFichGetFN(&c->lastFich);
        uint8_t dt = ysfFichGetDT(&c->lastFich);

        if (dt == YSF_DT_VD_MODE1)
        {
            ysfPayloadVDMode1Data(&c->rfPayload, frame, fn, false);
            c->rfBits += 235U;
        }
        else
        {
            ysfPayloadVDMode2Data(&c->rfPayload, frame, fn, false);
            c->rfErrs += ysfPayloadVDMode2Audio(&c->rfPayload, frame);
            c->rfBits += 405U;
        }

        YsfFich fich = c->lastFich;
        ysfFichEncode(&fich, frame);

        toNet(c, frame, c->rfFrames, false);
        toRf(c, frame, &fich, false);
        c->rfFrames++;
    }
    else if (valid && c->rfState == YSF_RF_LISTENING)
    {
        /* Late entry: only a clean frame will do */
        uint8_t fn = ysfFichGetFN(&c->lastFich);
        uint8_t dt = ysfFichGetDT(&c->lastFich);
        uint8_t cm = ysfFichGetCM(&c->lastFich);
        bool ok;

        if (dt == YSF_DT_VD_MODE1)
            ok = ysfPayloadVDMode1Data(&c->rfPayload, frame, fn, false);
        else if (dt == YSF_DT_VD_MODE2)
            ok = ysfPayloadVDMode2Data(&c->rfPayload, frame, fn, false);
        else
            ok = false;

        (void)ok;

        /* The callsigns are needed, and they are only complete after a few frames */
        if (!c->rfPayload.hasSource || !c->rfPayload.hasDest)
            return;

        rfSetCalls(c, cm);

        if (c->selfOnly && !checkCallsign(c, c->rfSource))
        {
            c->rfState = YSF_RF_REJECTED;
            raise(c, YSF_EV_RF_REJECTED, c->rfSource, BLANK_CALL, c->callsign, dgid, dt, false, 0.0F, 0.0F);
            return;
        }

        startRf(c, false);

        /* Build a new header and send it first */
        uint8_t buffer[YSF_FRAME_LENGTH_BYTES];
        uint8_t csd1[20], csd2[20];
        YsfFich fich = c->lastFich;

        memset(buffer, 0x00, sizeof(buffer));
        ysfAddSync(buffer);
        ysfFichSetFI(&fich, YSF_FI_HEADER);
        ysfFichEncode(&fich, buffer);

        memcpy(csd1 + YSF_CALLSIGN_LENGTH, c->rfSource, YSF_CALLSIGN_LENGTH);
        memset(csd2, ' ', 20U);

        if (cm == YSF_CM_GROUP1 || cm == YSF_CM_GROUP2)
            memset(csd1, '*', YSF_CALLSIGN_LENGTH);
        else
            memcpy(csd1, c->rfDest, YSF_CALLSIGN_LENGTH);

        ysfPayloadWriteHeader(buffer, csd1, csd2);

        toNet(c, buffer, c->rfFrames, false);
        toRf(c, buffer, &fich, false);

        raise(c, YSF_EV_RF_LATE, c->rfSource, c->rfDest, c->callsign, dgid, dt, false, 0.0F, 0.0F);

        /* ... and then the frame itself */
        ysfAddSync(frame);
        fich = c->lastFich;
        ysfFichEncode(&fich, frame);

        toNet(c, frame, c->rfFrames, false);
        toRf(c, frame, &fich, false);
        c->rfFrames++;
    }
}

static void rfDataFR(YsfCtrl* c, uint8_t* frame, bool valid)
{
    uint8_t fi = ysfFichGetFI(&c->lastFich);

    if (valid && fi == YSF_FI_HEADER)
    {
        rfHeader(c, frame, valid, true);
    }
    else if (valid && fi == YSF_FI_TERMINATOR)
    {
        rfTerminator(c, frame, true);
    }
    else if (c->rfState == YSF_RF_DATA)
    {
        if (!valid)
            guessNext(c);

        ysfAddSync(frame);

        ysfPayloadDataFRData(&c->rfPayload, frame, ysfFichGetFN(&c->lastFich), false);

        YsfFich fich = c->lastFich;
        ysfFichEncode(&fich, frame);

        toNet(c, frame, c->rfFrames, false);
        toRf(c, frame, &fich, false);
        c->rfFrames++;
    }
}

void ysfCtrlRf(YsfCtrl* c, const uint8_t* in, bool lost)
{
    uint8_t frame[YSF_FRAME_LENGTH_BYTES];

    if (lost)
    {
        if (c->rfState == YSF_RF_AUDIO || c->rfState == YSF_RF_DATA)
        {
            raise(c, YSF_EV_RF_LOST, c->rfSource, c->rfDest, c->callsign, ysfFichGetDGId(&c->lastFich),
                  ysfFichGetDT(&c->lastFich), c->rfState == YSF_RF_DATA,
                  (float)c->rfFrames / 10.0F, ratio(c->rfErrs, c->rfBits));
            endRf(c);
        }
        else
        {
            ysfPayloadReset(&c->rfPayload);
            c->rfState = YSF_RF_LISTENING;
        }

        return;
    }

    memcpy(frame, in, YSF_FRAME_LENGTH_BYTES);

    YsfFich fich;
    ysfFichInit(&fich);

    bool valid = ysfFichDecode(&fich, frame);

    if (valid)
    {
        c->lastFich = fich;
    }
    else
    {
        /* Carry on from the previous frame */
        ysfFichSetFI(&c->lastFich, YSF_FI_COMMUNICATIONS);
    }

    switch (ysfFichGetDT(&c->lastFich))
    {
    case YSF_DT_VOICE_FR_MODE:
        rfVoiceFR(c, frame, valid);
        break;

    case YSF_DT_VD_MODE1:
    case YSF_DT_VD_MODE2:
        rfVoiceDN(c, frame, valid);
        break;

    case YSF_DT_DATA_FR_MODE:
        rfDataFR(c, frame, valid);
        break;

    default:
        break;
    }
}

/* ------------------------------------------------------------------------ */
/* Network                                                                   */
/* ------------------------------------------------------------------------ */

static bool isBlank(const char* call)
{
    return memcmp(call, BLANK_CALL, YSF_CALLSIGN_LENGTH) == 0;
}

/* A terminator frame to the transmitter: the call has finished without one from the network */
static void netSendEot(YsfCtrl* c)
{
    uint8_t eot[YSF_FRAME_LENGTH_BYTES];
    YsfFich f;

    if (c->sendRf == NULL)
        return;

    memset(eot, 0x00, sizeof(eot));
    ysfFichInit(&f);
    ysfFichSetFI(&f, YSF_FI_TERMINATOR);
    ysfAddSync(eot);
    ysfFichEncode(&f, eot);
    c->sendRf(c->ctx, eot, true);
}

static void netAnnounce(YsfCtrl* c, uint8_t dgid, uint8_t dt)
{
    if (c->netAnnounced)
        return;

    if (isBlank(c->netSource) || isBlank(c->netDest))
        return;

    c->netAnnounced = true;
    raise(c, YSF_EV_NET_START, c->netSource, c->netDest, c->netGateway, dgid, dt, false, 0.0F, 0.0F);
}

/* The callsigns of a call that started without them come from the payload */
static void netCallsigns(YsfCtrl* c, uint8_t dgid, uint8_t dt)
{
    if (isBlank(c->netSource) && c->netPayload.hasSource)
        memcpy(c->netSource, c->netPayload.source, YSF_CALLSIGN_LENGTH);

    if (isBlank(c->netDest) && c->netPayload.hasDest)
        memcpy(c->netDest, c->netPayload.dest, YSF_CALLSIGN_LENGTH);

    netAnnounce(c, dgid, dt);
}

void ysfCtrlNet(YsfCtrl* c, const uint8_t* body)
{
    uint8_t frame[YSF_FRAME_LENGTH_BYTES];

    /* The transmitter is busy with RF */
    if (c->rfState != YSF_RF_LISTENING && c->netState == YSF_NET_IDLE)
        return;

    c->netWatchdog = 0U;
    c->netWatchdogRunning = true;

    bool gateway = memcmp(body + 0U, c->callsign, YSF_CALLSIGN_LENGTH) == 0;
    uint8_t n    = (uint8_t)((body[30] & 0xFEU) >> 1);
    bool end     = (body[30] & 0x01U) == 0x01U;

    /*
     * Another station in the middle of a call.  The end flag of a call does not always come (some gateways and
     * bridges never send it), and the next talker would then be heard, and logged, under the name of the one before.
     * One packet with a different callsign could be damaged, so it is kept; the next packet decides: from the same
     * new callsign, the old call is over and a new one starts with the kept packet; from the old one, the kept
     * packet is thrown away.
     */
    if (c->netTimerRunning && !end && !isBlank(c->netSource) && !isBlank((const char*)(body + 10U)) &&
        memcmp(c->netSource, body + 10U, YSF_CALLSIGN_LENGTH) != 0)
    {
        if (c->netHeldValid && memcmp(c->netHeld + 10U, body + 10U, YSF_CALLSIGN_LENGTH) == 0)
        {
            uint8_t held[YSF_NET_BODY_LENGTH];
            uint32_t total = c->netFrames + c->netLost;
            float loss = total > 0U ? (float)(c->netLost * 100U) / (float)total : 0.0F;

            memcpy(held, c->netHeld, sizeof(held));
            c->netHeldValid = false;

            raise(c, YSF_EV_NET_SWITCH, c->netSource, c->netDest, c->netGateway, 0U, 0U, false,
                  (float)c->netFrames / 10.0F, loss);
            netSendEot(c);
            endNet(c);

            /* the kept packet starts the new call, then this one goes on with it */
            ysfCtrlNet(c, held);
        }
        else
        {
            memcpy(c->netHeld, body, YSF_NET_BODY_LENGTH);
            c->netHeldValid = true;
            return;
        }
    }
    else if (c->netHeldValid)
    {
        c->netHeldValid = false;        /* the packet from the other callsign was a stray */
    }

    memcpy(frame, body + 31U, YSF_FRAME_LENGTH_BYTES);

    YsfFich fich;
    ysfFichInit(&fich);

    bool valid = ysfFichDecode(&fich, frame);
    uint8_t dgid = valid ? ysfFichGetDGId(&fich) : 0U;
    uint8_t dt   = valid ? ysfFichGetDT(&fich) : 0U;

    if (!c->netTimerRunning)
    {
        if (end)
            return;

        memcpy(c->netSource, body + 10U, YSF_CALLSIGN_LENGTH);
        memcpy(c->netDest, body + 20U, YSF_CALLSIGN_LENGTH);
        memcpy(c->netGateway, body + 0U, YSF_CALLSIGN_LENGTH);

        ysfPayloadReset(&c->netPayload);
        c->netTimer = 0U;
        c->netTimerRunning = true;
        c->netState  = YSF_NET_AUDIO;
        c->netFrames = 0U;
        c->netLost   = 0U;
        c->netN      = 0U;
        c->netAnnounced = false;
        c->netCsdLogged = false;

        netAnnounce(c, dgid, dt);
    }
    else
    {
        /* Duplicate frame */
        if (c->netN == n)
            return;

        uint8_t gap = (uint8_t)((n - c->netN) & 0x7FU);

        if (gap > 1U && gap < 64U)
            c->netLost += (uint32_t)(gap - 1U);
    }

    if (valid)
    {
        uint8_t fn = ysfFichGetFN(&fich);
        uint8_t ft = ysfFichGetFT(&fich);
        uint8_t fi = ysfFichGetFI(&fich);
        uint8_t cm = ysfFichGetCM(&fich);

        if (isBlank(c->netDest) && (cm == YSF_CM_GROUP1 || cm == YSF_CM_GROUP2))
            memcpy(c->netDest, "ALL       ", YSF_CALLSIGN_LENGTH);

        if (c->remoteGateway)
        {
            ysfFichSetVoIP(&fich, false);
            ysfFichSetMR(&fich, YSF_MR_DIRECT);
        }
        else
        {
            ysfFichSetVoIP(&fich, true);
            ysfFichSetMR(&fich, YSF_MR_BUSY);
        }

        ysfFichSetDev(&fich, c->lowDeviation);
        ysfFichEncode(&fich, frame);

        switch (fi)
        {
        case YSF_FI_HEADER:
            if (ysfPayloadHeader(&c->netPayload, frame))
                netCallsigns(c, dgid, dt);
            break;

        case YSF_FI_TERMINATOR:
            ysfPayloadHeader(&c->netPayload, frame);
            break;

        case YSF_FI_COMMUNICATIONS:
            switch (dt)
            {
            case YSF_DT_VD_MODE1:
                if (ysfPayloadVDMode1Data(&c->netPayload, frame, fn, gateway))
                    netCallsigns(c, dgid, dt);
                break;

            case YSF_DT_VD_MODE2:
                if (ysfPayloadVDMode2Data(&c->netPayload, frame, fn, gateway))
                    netCallsigns(c, dgid, dt);

                ysfPayloadVDMode2Audio(&c->netPayload, frame);
                break;

            case YSF_DT_DATA_FR_MODE:
                ysfPayloadDataFRData(&c->netPayload, frame, fn, gateway);
                break;

            case YSF_DT_VOICE_FR_MODE:
                if (fn == 0U && ft == 1U)
                    ysfPayloadVoiceFRData(&c->netPayload, frame);
                break;

            default:
                break;
            }
            break;

        default:
            break;
        }

        /* what the frames themselves say, as opposed to the packet header: this is what the transmitter sends */
        if (!c->netCsdLogged && c->netPayload.hasSource)
        {
            const char* dest = c->netPayload.hasDest ? (const char*)c->netPayload.dest : BLANK_CALL;

            c->netCsdLogged = true;
            raise(c, YSF_EV_NET_CSD, (const char*)c->netPayload.source, dest, c->netSource, dgid, dt, false, 0.0F, 0.0F);
        }
    }

    if (c->sendRf != NULL && !netExpired(c))
        c->sendRf(c->ctx, frame, end);

    c->netWatchdog = 0U;
    c->netFrames++;
    c->netN = n;

    if (end)
    {
        uint32_t total = c->netFrames + c->netLost;
        float loss = total > 0U ? (float)(c->netLost * 100U) / (float)total : 0.0F;

        raise(c, YSF_EV_NET_END, c->netSource, c->netDest, c->netGateway, dgid, dt, false,
              (float)c->netFrames / 10.0F, loss);
        endNet(c);
    }
}

void ysfCtrlClock(YsfCtrl* c, uint32_t ms)
{
    if (c->rfTimerRunning)
        c->rfTimer += ms;

    if (c->netTimerRunning)
        c->netTimer += ms;

    if (c->netState == YSF_NET_AUDIO && c->netWatchdogRunning)
    {
        c->netWatchdog += ms;

        if (c->netWatchdog >= c->netWatchdogMs)
        {
            uint32_t total = c->netFrames + c->netLost;
            float loss = total > 0U ? (float)(c->netLost * 100U) / (float)total : 0.0F;

            raise(c, YSF_EV_NET_LOST, c->netSource, c->netDest, c->netGateway, 0U, 0U, false,
                  (float)c->netFrames / 10.0F, loss);

            /* Let the transmitter know that the call has finished */
            netSendEot(c);

            endNet(c);
        }
    }
}
