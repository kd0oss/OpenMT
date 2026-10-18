/*
 *   Copyright (C) 2026 by Rick KD0OSS
 *
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
 *   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

/*
 * Drop-in replacement for the parts of <arm_math.h> (CMSIS-DSP) that the hosts use.  Replace
 *     #include <arm_math.h>
 * with
 *     #include <dsp_compat.h>
 * and do not link CMSIS-DSP.  Do not include both.
 *
 * Provided (same names, same structure fields, same results as the portable C code of CMSIS-DSP):
 *     q7_t q15_t q31_t q63_t  __SSAT()
 *     arm_q15_to_q31()
 *     arm_fir_instance_q15            arm_fir_q15()  arm_fir_fast_q15()
 *     arm_fir_interpolate_instance_q15  arm_fir_interpolate_q15()
 *     arm_biquad_casd_df1_inst_q31    arm_biquad_cascade_df1_q31()
 *
 * The instances are filled in by hand by the hosts (numTaps, pState, pCoeffs, ...), so no arm_*_init functions
 * are needed.
 */

#ifndef DSP_COMPAT_H
#define DSP_COMPAT_H

#include <stdint.h>
#include <string.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int8_t  q7_t;
typedef int16_t q15_t;
typedef int32_t q31_t;
typedef int64_t q63_t;

/* arm_math.h pulls these in as well; the hosts rely on them */
#ifndef PI
#define PI 3.14159265358979f
#endif

/* Signed saturate to 'sat' bits (1..32), same as the ARM SSAT instruction */
#ifndef __SSAT
static inline int32_t __SSAT(int32_t val, uint32_t sat)
{
    if (sat >= 1U && sat <= 32U)
    {
        const int32_t max = (int32_t)((1U << (sat - 1U)) - 1U);
        const int32_t min = -1 - max;
        if (val > max)
            return max;
        else if (val < min)
            return min;
    }
    return val;
}
#endif

static inline q15_t dspSat15(q63_t v)
{
    if (v > 32767)
        return 32767;
    if (v < -32768)
        return -32768;
    return (q15_t)v;
}

static inline void arm_q15_to_q31(const q15_t* pSrc, q31_t* pDst, uint32_t blockSize)
{
    while (blockSize-- > 0U)
        *pDst++ = (q31_t)*pSrc++ * 65536;
}

/* ---------- FIR, q15 ---------- */

typedef struct
{
    uint16_t       numTaps;  /* number of filter coefficients */
    q15_t*         pState;   /* numTaps + blockSize - 1 samples */
    const q15_t*   pCoeffs;  /* numTaps coefficients, time reversed: {b[N-1], ..., b[1], b[0]} */
} arm_fir_instance_q15;

/* acc32: true = 32 bit accumulator that wraps (the "fast" version), false = 64 bit accumulator */
static inline void dspFirQ15(const arm_fir_instance_q15* S, const q15_t* pSrc, q15_t* pDst, uint32_t blockSize, int acc32)
{
    q15_t*       pState   = S->pState;
    const q15_t* pCoeffs  = S->pCoeffs;
    q15_t*       pCurrent = &S->pState[S->numTaps - 1U];
    uint32_t     numTaps  = S->numTaps;

    while (blockSize-- > 0U)
    {
        *pCurrent++ = *pSrc++;

        const q15_t* px = pState;
        const q15_t* pb = pCoeffs;
        q63_t        acc = 0;
        uint32_t     acc32u = 0U;

        for (uint32_t i = 0U; i < numTaps; i++)
        {
            if (acc32)
                acc32u += (uint32_t)((q31_t)px[i] * pb[i]);
            else
                acc += (q63_t)px[i] * pb[i];
        }

        if (acc32)
            *pDst++ = dspSat15((q63_t)((q31_t)acc32u >> 15));
        else
            *pDst++ = dspSat15(acc >> 15);

        pState++;
    }

    /* keep the last numTaps - 1 samples for the next call */
    memmove(S->pState, pState, (numTaps - 1U) * sizeof(q15_t));
}

static inline void arm_fir_q15(const arm_fir_instance_q15* S, const q15_t* pSrc, q15_t* pDst, uint32_t blockSize)
{
    dspFirQ15(S, pSrc, pDst, blockSize, 0);
}

static inline void arm_fir_fast_q15(const arm_fir_instance_q15* S, const q15_t* pSrc, q15_t* pDst, uint32_t blockSize)
{
    dspFirQ15(S, pSrc, pDst, blockSize, 1);
}

/* ---------- FIR interpolator, q15 ---------- */

typedef struct
{
    uint8_t        L;            /* upsample factor */
    uint16_t       phaseLength;  /* number of coefficients in each polyphase filter */
    const q15_t*   pCoeffs;      /* L * phaseLength coefficients */
    q15_t*         pState;       /* phaseLength + blockSize - 1 samples */
} arm_fir_interpolate_instance_q15;

static inline void arm_fir_interpolate_q15(const arm_fir_interpolate_instance_q15* S, const q15_t* pSrc, q15_t* pDst,
                                           uint32_t blockSize)
{
    const q15_t* pCoeffs  = S->pCoeffs;
    q15_t*       pState   = S->pState;
    q15_t*       pCurrent = S->pState + (S->phaseLength - 1U);
    uint32_t     phaseLen = S->phaseLength;
    uint32_t     L        = S->L;

    while (blockSize-- > 0U)
    {
        *pCurrent++ = *pSrc++;

        for (uint32_t j = 1U; j <= L; j++)
        {
            const q15_t* ptr1 = pState;
            const q15_t* ptr2 = pCoeffs + (L - j);
            q63_t        sum  = 0;

            for (uint32_t t = 0U; t < phaseLen; t++)
            {
                sum += (q63_t)*ptr1++ * *ptr2;
                ptr2 += L;
            }

            *pDst++ = dspSat15(sum >> 15);
        }

        pState++;
    }

    memmove(S->pState, pState, (phaseLen - 1U) * sizeof(q15_t));
}

/* ---------- Biquad cascade direct form I, q31 ---------- */

typedef struct
{
    uint32_t       numStages;  /* number of 2nd order stages */
    q31_t*         pState;     /* 4 * numStages values: {x[n-1], x[n-2], y[n-1], y[n-2]} per stage */
    const q31_t*   pCoeffs;    /* 5 * numStages values: {b0, b1, b2, a1, a2} per stage (1.31 format) */
    uint8_t        postShift;  /* additional shift of the output, in bits */
} arm_biquad_casd_df1_inst_q31;

static inline void arm_biquad_cascade_df1_q31(const arm_biquad_casd_df1_inst_q31* S, const q31_t* pSrc, q31_t* pDst,
                                              uint32_t blockSize)
{
    const q31_t* pIn     = pSrc;
    q31_t*       pState  = S->pState;
    const q31_t* pCoeffs = S->pCoeffs;
    uint32_t     uShift  = (uint32_t)S->postShift + 1U;
    uint32_t     lShift  = 32U - uShift;
    uint32_t     stage   = S->numStages;

    do
    {
        q31_t b0 = *pCoeffs++;
        q31_t b1 = *pCoeffs++;
        q31_t b2 = *pCoeffs++;
        q31_t a1 = *pCoeffs++;
        q31_t a2 = *pCoeffs++;

        q31_t Xn1 = pState[0];
        q31_t Xn2 = pState[1];
        q31_t Yn1 = pState[2];
        q31_t Yn2 = pState[3];

        for (uint32_t n = 0U; n < blockSize; n++)
        {
            q31_t Xn = *pIn++;

            /* 2.62 accumulator; CMSIS lets it wrap, so do the sum unsigned to avoid undefined behavior */
            uint64_t acc = (uint64_t)((q63_t)b0 * Xn) + (uint64_t)((q63_t)b1 * Xn1) + (uint64_t)((q63_t)b2 * Xn2) +
                           (uint64_t)((q63_t)a1 * Yn1) + (uint64_t)((q63_t)a2 * Yn2);

            q31_t Yn = (q31_t)((q63_t)acc >> lShift);

            *pDst++ = Yn;

            Xn2 = Xn1;
            Xn1 = Xn;
            Yn2 = Yn1;
            Yn1 = Yn;
        }

        pState[0] = Xn1;
        pState[1] = Xn2;
        pState[2] = Yn1;
        pState[3] = Yn2;
        pState += 4;

        /* the next stage works on this stage's output */
        pIn  = pDst - blockSize;
        pDst = pDst - blockSize;
    } while (--stage > 0U);
}

#ifdef __cplusplus
}
#endif

#endif /* DSP_COMPAT_H */
