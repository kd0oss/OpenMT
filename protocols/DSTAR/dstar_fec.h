/*
 * dstar_fec.h -- D-Star AMBE frame error count, MMDVMHost style.
 *
 * Every D-Star voice frame carries 72 AMBE bits.  Bit groups "a" and "b"
 * (24 bits each) are Golay(24,12) protected, "b" additionally scrambled with
 * a PRNG sequence selected by the decoded "a" data; "c" is unprotected.
 * Decoding a and b, re-encoding them and counting the bits that differ from
 * what was received gives the number of channel bit errors in the 48
 * protected bits of that frame -- the same figure MMDVMHost uses for its BER.
 *
 * The Golay coding and the scrambling table (AMBE_PRNG_TABLE) are in
 * fec_tools.  Only the D-Star specific part is here: the bit positions of the
 * three groups.
 *
 * Based on MMDVMHost AMBEFEC.cpp:
 *   Copyright (C) 2010,2014,2016,2018,2021,2025 Jonathan Naylor G4KLX
 * Distributed under the GNU General Public License v2 or later.
 */
#ifndef DSTAR_FEC_H
#define DSTAR_FEC_H

#include <stdbool.h>
#include <stdint.h>

#include <fec_tools.h>

#define DSTAR_FEC_BITS_PER_FRAME 48U /* protected bits that are checked */

/* Regeneration only rewrites a frame when the FEC found at most this many
 * errors.  Each Golay(24,12) word corrects up to 3 bit errors, so 6 is the
 * most a correctable frame can have; an uncorrectable word is reported as 10
 * errors.  Anything above the limit is relayed as received instead of being
 * "corrected" into the wrong data. */
#ifndef DSTAR_FEC_MAX_ERRORS
#define DSTAR_FEC_MAX_ERRORS 6U
#endif


static const unsigned int DSTAR_FEC_A[] = {0U,  6U, 12U, 18U, 24U, 30U, 36U, 42U, 48U, 54U, 60U, 66U,
									  1U,  7U, 13U, 19U, 25U, 31U, 37U, 43U, 49U, 55U, 61U, 67U};
static const unsigned int DSTAR_FEC_B[] = {2U,  8U, 14U, 20U, 26U, 32U, 38U, 44U, 50U, 56U, 62U, 68U,
									  3U,  9U, 15U, 21U, 27U, 33U, 39U, 45U, 51U, 57U, 63U, 69U};
static const unsigned int DSTAR_FEC_C[] = {4U, 10U, 16U, 22U, 28U, 34U, 40U, 46U, 52U, 58U, 64U, 70U,
									  5U, 11U, 17U, 23U, 29U, 35U, 41U, 47U, 53U, 59U, 65U, 71U};

#define DSTAR_FEC_READ(p, i)    ((p)[(i) >> 3] & (0x80U >> ((i) & 7U)))
#define DSTAR_FEC_WRITE(p, i, b) \
    ((p)[(i) >> 3] = (b) ? (uint8_t)((p)[(i) >> 3] | (0x80U >> ((i) & 7U))) \
                         : (uint8_t)((p)[(i) >> 3] & ~(0x80U >> ((i) & 7U))))

/*
 * Count the channel errors in the 48 protected bits of one 9-byte AMBE frame
 * (as it comes off the air, 72 bits, MSB first).  A word that differs from
 * its nearest codeword in more than 3 of its 24 bits had 4 or more errors
 * (the extended code detects 4), so it is uncorrectable and counts as 10
 * errors.  When "regenerate" is true and the frame is within
 * DSTAR_FEC_MAX_ERRORS the corrected bits are written back into the frame.
 */
static inline unsigned int dstarFecErrors(uint8_t *ambe, bool regenerate)
{
    unsigned int a = 0U;
    unsigned int b = 0U;
    unsigned int c = 0U;
    unsigned int mask = 0x800000U;
    unsigned int data;
    unsigned int datb;
    unsigned int p;
    unsigned int errA;
    unsigned int errB;
    unsigned int errors;
    unsigned int i;

    for (i = 0U; i < 24U; i++, mask >>= 1)
    {
        if (DSTAR_FEC_READ(ambe, DSTAR_FEC_A[i]))
        {
            a |= mask;
        }
        if (DSTAR_FEC_READ(ambe, DSTAR_FEC_B[i]))
        {
            b |= mask;
        }
        if (DSTAR_FEC_READ(ambe, DSTAR_FEC_C[i]))
        {
            c |= mask;
        }
    }

    errA = golay24128_errors24128(a, &data);
    p    = AMBE_PRNG_TABLE[data];
    errB = golay24128_errors24128(b ^ p, &datb);

    a = golay24128_encode24128(data);
    b = golay24128_encode24128(datb) ^ p;

    if (errA > 3U || errB > 3U)
    {
        errors = 10U;
    }
    else
    {
        errors = errA + errB;
    }

    if (regenerate && errors <= DSTAR_FEC_MAX_ERRORS)
    {
        mask = 0x800000U;
        for (i = 0U; i < 24U; i++, mask >>= 1)
        {
            DSTAR_FEC_WRITE(ambe, DSTAR_FEC_A[i], a & mask);
            DSTAR_FEC_WRITE(ambe, DSTAR_FEC_B[i], b & mask);
            DSTAR_FEC_WRITE(ambe, DSTAR_FEC_C[i], c & mask);
        }
    }
    return errors;
}

#endif /* DSTAR_FEC_H */
