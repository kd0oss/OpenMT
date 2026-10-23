/* Created by Rick KD0OSS */
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

/*
 * dsp_tools.h
 *
 * The 4-level FSK modem engine shared by the NXDN and YSF hosts: pulse
 * shaping transmitter, matched filter receiver that finds the frame sync
 * word and slices the frames, and the receiver for the hard bit stream of
 * the ADF7021.
 *
 * The engine knows nothing about a protocol.  Each protocol describes itself
 * with an Fsk4Cfg (samples per symbol, frame size, sync word, how many frames
 * may be missed, ...) and gives a function that checks a sliced frame.  The
 * protocol files nxdn_dsp.c and ysf_dsp.c only hold that description and
 * thin wrappers that keep their own function names.
 *
 * Sample polarity follows the P25 host: a negative sample is the +3 symbol.
 * Dibits 01 -> +3, 00 -> +1, 10 -> -1, 11 -> -3.
 */

#ifndef DSP_TOOLS_H
#define DSP_TOOLS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FSK4_SHAPE_RRC          0U      /* root raised cosine pulse */
#define FSK4_SHAPE_RC           1U      /* raised cosine pulse */

#define FSK4_MAX_SPAN           6U      /* pulse length: symbols each side of the centre */
#define FSK4_MAX_TAPS           121U    /* 2 * span * samples per symbol + 1 for the largest protocol */
#define FSK4_RX_RING            8192U   /* samples kept by the receiver (power of two) */
#define FSK4_RX_MAX_CAND        32U     /* sync word candidates kept while hunting */
#define FSK4_MAX_FRAME_BYTES    120U    /* largest frame of any protocol */

/* Called for every frame received (frame set, lost false) and when the signal is lost (frame NULL, lost true). */
typedef void (*Fsk4FrameFn)(void* ctx, const uint8_t* frame, bool lost);

/* Checks a sliced frame (as it came off the air) with the protocol's channel coding. */
typedef bool (*Fsk4ValidFn)(const uint8_t* frame);

/*
 * Description of one protocol.  The protocol fills in the first group; the
 * rest is worked out by fsk4CfgPrepare().
 */
typedef struct
{
    /* Set by the protocol */
    unsigned int sps;               /* samples per symbol */
    unsigned int filterSpan;        /* pulse length, symbols each side of the centre (up to FSK4_MAX_SPAN) */
    unsigned int frameSymbols;      /* symbols in a frame, sync word included */
    unsigned int syncSymbols;       /* symbols of the sync word seen by the sample receiver */
    const float* syncQ;             /* the sync symbols in the sample domain (-symbol), syncSymbols values */
    unsigned int maxMiss;           /* frames in a row without a sync word before the signal is lost */
    bool holdOnMiss;                /* hold a frame without a sync word until the next one has one */
    Fsk4ValidFn valid;              /* channel code check of a sliced frame */

    /* Bit stream receiver */
    uint64_t syncBitsValue;         /* the sync word as bits */
    uint64_t syncBitsMask;          /* mask selecting the sync word bits in the bit window */
    unsigned int syncBits;          /* length of the sync word in bits */
    unsigned int frameBits;         /* length of a frame in bits */
    unsigned int startErrs;         /* bit errors allowed in the sync word to start a lock */
    unsigned int runErrs;           /* bit errors allowed once locked */
    unsigned int maxFrames;         /* frames without a sync word before the signal is lost */
    const uint8_t* syncBytes;       /* sync word bytes, copied to the start of every frame */

    /* Worked out by fsk4CfgPrepare() */
    bool ready;
    unsigned int filterTaps;        /* 2 * filterSpan * sps + 1 */
    unsigned int frameBytes;        /* frameSymbols / 4 */
    unsigned int samplesPerFrame;   /* sps * frameSymbols */
    unsigned int syncSpan;          /* samples from the first to the last sync symbol */
    unsigned int frameLast;         /* samples from the last sync symbol to the last frame symbol */
    float syncEnergy;               /* sum of the squares of the sync symbols */
    unsigned int syncHi;            /* number of +3 symbols (in the sample domain) in the sync word */
    unsigned int syncLo;            /* number of -3 symbols (in the sample domain) in the sync word */
} Fsk4Cfg;

/* Transmitter */
typedef struct
{
    const Fsk4Cfg* cfg;
    float sym[FSK4_MAX_SPAN * 2U + 1U];     /* newest first */
    float level;                            /* amplitude of the +-3 symbol */
    float taps[FSK4_MAX_TAPS];
    uint8_t shape;
} Fsk4Tx;

/* A sync word found while hunting */
typedef struct
{
    uint64_t nb;
    uint64_t due;
    float r;
} Fsk4Cand;

/* Sample receiver */
typedef struct
{
    const Fsk4Cfg* cfg;
    float taps[FSK4_MAX_TAPS];
    float inbuf[2U * FSK4_MAX_TAPS];
    unsigned int inpos;
    float y[FSK4_RX_RING];
    float r[FSK4_RX_RING];
    uint64_t n;

    Fsk4Cand cand[FSK4_RX_MAX_CAND];
    unsigned int candCount;

    bool tracking;
    bool prov;                  /* locked on one frame, waiting for a second */
    uint8_t provFrame[FSK4_MAX_FRAME_BYTES];
    uint64_t exp;               /* expected last sync symbol of the next frame  */
    uint64_t nb;                /* last sync symbol of the frame being decoded */
    uint64_t due;
    bool dueValid;
    bool windowDone;
    bool found;
    bool held;                  /* a frame whose sync was missed, waiting for the next sync */
    uint8_t heldFrame[FSK4_MAX_FRAME_BYTES];
    unsigned int miss;
    float centre;
    float amp;
    float minAmp;               /* smallest outer symbol amplitude accepted */

    Fsk4FrameFn cb;
    void* ctx;

    uint32_t statCandidates;
    uint32_t statLocks;
    uint32_t statFrames;
    uint32_t statLost;
    uint32_t statDropped;
    float statLastR;
    float statAmp;
} Fsk4Rx;

/* Bit stream receiver */
typedef struct
{
    const Fsk4Cfg* cfg;
    bool inFrame;
    uint64_t window;            /* last bits received */
    uint8_t frame[FSK4_MAX_FRAME_BYTES];
    unsigned int ptr;           /* bits in the frame */
    unsigned int lostCount;
    Fsk4FrameFn cb;
    void* ctx;
    uint32_t statFrames;
    uint32_t statLost;
} Fsk4BitRx;

void fsk4CfgPrepare(Fsk4Cfg* cfg);

void fsk4TxInit(Fsk4Tx* tx, Fsk4Cfg* cfg, float level, uint8_t shape);
void fsk4TxSetLevel(Fsk4Tx* tx, float level);
unsigned int fsk4TxPreamble(Fsk4Tx* tx, unsigned int symbols, int16_t* out);
unsigned int fsk4TxFrame(Fsk4Tx* tx, const uint8_t* frame, int16_t* out);
unsigned int fsk4TxFlush(Fsk4Tx* tx, int16_t* out);

void fsk4RxInit(Fsk4Rx* rx, Fsk4Cfg* cfg, Fsk4FrameFn cb, void* ctx, uint8_t shape);
void fsk4RxReset(Fsk4Rx* rx);
void fsk4RxSamples(Fsk4Rx* rx, const int16_t* samples, unsigned int length);

void fsk4BitRxInit(Fsk4BitRx* rx, Fsk4Cfg* cfg, Fsk4FrameFn cb, void* ctx);
void fsk4BitRxReset(Fsk4BitRx* rx);
void fsk4BitRxBytes(Fsk4BitRx* rx, const uint8_t* bytes, unsigned int length);

unsigned int fsk4BitTxPreamble(uint8_t* out, unsigned int bytes);

#ifdef __cplusplus
}
#endif

#endif
