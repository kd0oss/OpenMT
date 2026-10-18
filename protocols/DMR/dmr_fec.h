/*
 * dmr_fec.h -- DMR AMBE+2 voice burst error count and regeneration,
 * MMDVMHost style.
 *
 * A DMR voice burst is 33 bytes (264 bits).  The middle 48 bits (108..155)
 * hold the sync pattern or embedded signalling, the other 216 bits hold three
 * AMBE+2 vocoder frames of 72 bits each:
 *
 *      frame 0   burst bits   0..71
 *      frame 1   burst bits  72..107 followed by 156..191
 *      frame 2   burst bits 192..263
 *
 * Inside a vocoder frame the bits are interleaved into three groups:
 *
 *      a   24 bits   Golay(24,12)
 *      b   23 bits   Golay(23,12), scrambled with a sequence chosen by the
 *                    12 data bits of "a"
 *      c   25 bits   unprotected
 *
 * Decoding a and b, re-encoding them and counting the bits that differ from
 * what was received gives the number of channel bit errors in the 47
 * protected bits of the frame (141 per burst), the same figure MMDVMHost
 * uses for its DMR BER.
 *
 * The Golay coding and the scrambling table (AMBE_PRNG_TABLE) are in
 * dsp_tools, shared with D-Star and P25.  Only the DMR specific part is
 * here: where the three groups sit in a burst.
 *
 * Based on MMDVMHost AMBEFEC.cpp:
 *   Copyright (C) 2010,2014,2016,2018,2021,2025 Jonathan Naylor G4KLX
 * Distributed under the GNU General Public License v2 or later.
 */
#ifndef DMR_FEC_H
#define DMR_FEC_H

#include <stdbool.h>
#include <stdint.h>

#include <dsp_tools.h>

#define DMR_FEC_BITS_PER_FRAME 47U  /* protected bits checked per vocoder frame */
#define DMR_FEC_BITS_PER_BURST 141U /* three vocoder frames per voice burst */

/* A vocoder frame is only rewritten when the FEC found at most this many
 * errors in it.  Each Golay word corrects up to 3 bit errors, so 6 is the
 * most a correctable frame can show.  An "a" word with 4 or more errors is
 * detected and counts as 10.  The 23 bit "b" word has no parity bit, so four
 * or more errors in it are silently "corrected" to a wrong codeword; that is
 * why frames near the limit are relayed as received rather than rewritten
 * into clean looking but wrong audio. */
#ifndef DMR_FEC_MAX_ERRORS
#define DMR_FEC_MAX_ERRORS 6U
#endif

/* Bit positions inside the 72 bit vocoder frame, in transmission order of
 * the code word (first entry = most significant bit). */
static const uint8_t DMR_FEC_A[24U] = {0U, 4U,  8U, 12U, 16U, 20U, 24U, 28U, 32U, 36U, 40U, 44U,
                                       48U, 52U, 56U, 60U, 64U, 68U, 1U,  5U,  9U, 13U, 17U, 21U};
static const uint8_t DMR_FEC_B[23U] = {25U, 29U, 33U, 37U, 41U, 45U, 49U, 53U, 57U, 61U, 65U, 69U,
                                       2U,  6U, 10U, 14U, 18U, 22U, 26U, 30U, 34U, 38U, 42U};
static const uint8_t DMR_FEC_C[25U] = {46U, 50U, 54U, 58U, 62U, 66U, 70U, 3U,  7U, 11U, 15U, 19U, 23U,
                                       27U, 31U, 35U, 39U, 43U, 47U, 51U, 55U, 59U, 63U, 67U, 71U};

/* Position in the 264 bit burst of bit i (0..71) of vocoder frame n (0..2). */
static inline unsigned int dmrFecPos(unsigned int n, unsigned int i)
{
    if (n == 0U)
    {
        return i;
    }
    if (n == 1U)
    {
        return (i < 36U) ? 72U + i : 156U + (i - 36U);
    }
    return 192U + i;
}

#define DMR_FEC_READ(p, i) (((p)[(i) >> 3] & (0x80U >> ((i) & 7U))) != 0U)
#define DMR_FEC_WRITE(p, i, b) \
    ((p)[(i) >> 3] = (b) ? (uint8_t)((p)[(i) >> 3] | (0x80U >> ((i) & 7U))) \
                         : (uint8_t)((p)[(i) >> 3] & ~(0x80U >> ((i) & 7U))))

/*
 * Check one 72 bit vocoder frame (n = 0..2) of a 33 byte burst.  Returns the
 * number of channel errors in its 47 protected bits.  When "regenerate" is
 * true and the count is within DMR_FEC_MAX_ERRORS the corrected a and b bits
 * are written back into the burst.
 */
static inline unsigned int dmrFecFrame(uint8_t *burst, unsigned int n, bool regenerate)
{
    unsigned int a = 0U;
    unsigned int b = 0U;
    unsigned int data;
    unsigned int datb;
    unsigned int p;
    unsigned int errA;
    unsigned int errB;
    unsigned int errors;
    unsigned int i;

    for (i = 0U; i < 24U; i++)
    {
        a = (a << 1) | (DMR_FEC_READ(burst, dmrFecPos(n, DMR_FEC_A[i])) ? 1U : 0U);
    }
    for (i = 0U; i < 23U; i++)
    {
        b = (b << 1) | (DMR_FEC_READ(burst, dmrFecPos(n, DMR_FEC_B[i])) ? 1U : 0U);
    }

    errA = golay24128_errors24128(a, &data);
    p    = AMBE_PRNG_TABLE[data] >> 1; /* 23 bit sequence for the 23 bit "b" */
    errB = golay24128_errors23127(b ^ p, &datb);

    /* An "a" word with more than 3 errors is beyond the extended code: its
     * data, and so the sequence that scrambled "b", can not be trusted. */
    errors = (errA > 3U) ? 10U : errA + errB;

    if (regenerate && errors <= DMR_FEC_MAX_ERRORS)
    {
        a = golay24128_encode24128(data);
        b = (golay24128_encode23127(datb) >> 1) ^ p;

        for (i = 0U; i < 24U; i++)
        {
            DMR_FEC_WRITE(burst, dmrFecPos(n, DMR_FEC_A[i]), (a >> (23U - i)) & 1U);
        }
        for (i = 0U; i < 23U; i++)
        {
            DMR_FEC_WRITE(burst, dmrFecPos(n, DMR_FEC_B[i]), (b >> (22U - i)) & 1U);
        }
    }
    return errors;
}

/*
 * Check the three vocoder frames of a 33 byte voice burst.  Returns the total
 * channel error count; each frame is regenerated on its own merits.
 */
static inline unsigned int dmrFecErrors(uint8_t *burst, bool regenerate)
{
    unsigned int errors = 0U;

    for (unsigned int n = 0U; n < 3U; n++)
    {
        errors += dmrFecFrame(burst, n, regenerate);
    }
    return errors;
}

#endif /* DMR_FEC_H */
