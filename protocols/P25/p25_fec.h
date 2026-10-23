/* Created by Rick KD0OSS */
/*
 * p25_fec.h -- P25 IMBE voice frame error count and regeneration,
 * MMDVMHost style.
 *
 * Every P25 voice frame carries 144 bits, in the order they are used after
 * de-interleaving:
 *
 *      c0   23 bits   Golay(23,12)     12 data bits, not whitened
 *      c1   23 bits   Golay(23,12)  \
 *      c2   23 bits   Golay(23,12)   |  whitened with a sequence
 *      c3   23 bits   Golay(23,12)   |  seeded by the data bits of c0
 *      c4   15 bits   Hamming(15,11) |
 *      c5   15 bits   Hamming(15,11) |
 *      c6   15 bits   Hamming(15,11)/
 *      c7    7 bits   unprotected
 *
 * Decoding the protected blocks, re-encoding them and counting the bits that
 * differ from what was received gives the number of channel bit errors in the
 * 137 protected bits of the frame, which is also what MMDVMHost reports for
 * its P25 BER (1233 bits per LDU of nine frames).
 *
 * All the coding is done by fec_tools: golay24128_decode23127() /
 * golay24128_encode23127() for c0 to c3 and hamming_decode15113_1() for c4 to
 * c6, the same functions the transmit side uses, so the bit conventions
 * can not differ.  The only thing added here is the frame layout and the
 * whitening.
 *
 * Distributed under the GNU General Public License v2 or later.
 */
#ifndef P25_FEC_H
#define P25_FEC_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <fec_tools.h>

#define P25_FEC_BITS_PER_FRAME 137U

/* A frame is only rewritten when the FEC found at most this many errors.
 * Each Golay word corrects up to 3 bit errors and each Hamming word 1, so a
 * frame can show at most 15.  Both codes are perfect: a damaged word is
 * always "corrected" to some valid codeword, right or wrong.  Simulated on
 * random frames, a frame needing 6 corrections is rewritten correctly about
 * 54% of the time, 7 about 41%, 8 about 30% and 10 about 13%.  Above the
 * limit the frame is relayed as received, so the radio's own error handling
 * still sees that it was damaged instead of being handed a clean looking
 * but wrong frame. */
#ifndef P25_FEC_MAX_ERRORS
#define P25_FEC_MAX_ERRORS 7U
#endif

#define P25_FEC_REWRITE_OK(errors) ((errors) <= P25_FEC_MAX_ERRORS)

static inline unsigned int p25FecGetBits(const bool* bit, unsigned int count)
{
    unsigned int v = 0U;

    for (unsigned int i = 0U; i < count; i++)
    {
        v = (v << 1) | (bit[i] ? 1U : 0U);
    }

    return v;
}

static inline void p25FecPutBits(bool* bit, unsigned int count, unsigned int v)
{
    for (unsigned int i = 0U; i < count; i++)
    {
        bit[count - 1U - i] = ((v >> i) & 1U) != 0U;
    }
}

/*
 * Check one voice frame given as 144 de-interleaved bits.  Returns the number
 * of bit errors found in the 137 protected bits.  When "rewrite" is true and
 * the count is within P25_FEC_MAX_ERRORS the corrected bits (re-encoded and
 * whitened again) are written back into bit[].
 */
static inline unsigned int p25FecFrame(bool* bit, bool rewrite)
{
    bool         out[144U];
    bool         prn[114U];
    unsigned int errors = 0U;
    unsigned int word;
    unsigned int data;

    memcpy(out, bit, sizeof out);

    /* c0, not whitened.  Its data bits seed the whitening sequence. */
    word = p25FecGetBits(bit, 23U);
    errors += golay24128_errors23127(word, &data);
    p25FecPutBits(out, 23U, golay24128_encode23127(data) >> 1);

    unsigned int p = 16U * data;
    for (unsigned int i = 0U; i < 114U; i++)
    {
        p      = (173U * p + 13849U) % 65536U;
        prn[i] = p >= 32768U;
    }

    /* c1 to c3, Golay(23,12) */
    for (unsigned int k = 0U; k < 3U; k++)
    {
        unsigned int base = 23U + 23U * k;
        bool         w[23U];

        for (unsigned int i = 0U; i < 23U; i++)
        {
            w[i] = bit[base + i] ^ prn[base - 23U + i];
        }

        word = p25FecGetBits(w, 23U);
        errors += golay24128_errors23127(word, &data);

        p25FecPutBits(w, 23U, golay24128_encode23127(data) >> 1);
        for (unsigned int i = 0U; i < 23U; i++)
        {
            out[base + i] = w[i] ^ prn[base - 23U + i];
        }
    }

    /* c4 to c6, Hamming(15,11) */
    for (unsigned int k = 0U; k < 3U; k++)
    {
        unsigned int base = 92U + 15U * k;
        bool         h[15U];

        for (unsigned int i = 0U; i < 15U; i++)
        {
            h[i] = bit[base + i] ^ prn[base - 23U + i];
        }

        errors += hamming_decode15113_1(h) ? 1U : 0U;

        for (unsigned int i = 0U; i < 15U; i++)
        {
            out[base + i] = h[i] ^ prn[base - 23U + i];
        }
    }

    /* c7, 7 unprotected bits, left as received */

    if (rewrite && P25_FEC_REWRITE_OK(errors))
    {
        memcpy(bit, out, sizeof out);
    }

    return errors;
}

#endif /* P25_FEC_H */
