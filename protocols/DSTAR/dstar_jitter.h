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

#ifndef DSTAR_JITTER_H
#define DSTAR_JITTER_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/*
 * Jitter buffer for the D-Star voice frames that come from the reflector.
 *
 * The frames carry a sequence number 0..20 (one superframe) and arrive from
 * the network in bursts and out of order.  Passed on as they come, a block
 * that arrives 200 ms late is played 200 ms out of place (garbled audio), and
 * a gap followed by a burst runs the modem dry, which ends the transmission
 * on the air (the beep).  Here the frames are put back in order, held for
 * jitterMs and released one every 20 ms; a frame that has not arrived by its
 * turn is replaced by silence so that the stream keeps its timing.
 *
 * Sequence numbers repeat every 21 frames (420 ms), and the network holds
 * frames back by 200 ms and more, so the number alone cannot tell a frame that
 * is new from one that is late.  The time of arrival does: the network only
 * delays frames, so every frame lies on or behind a line that the earliest
 * frames define (see jbClassify).
 */

#define JB_FRAME_MS      20
#ifndef JB_SLOTS
#define JB_SLOTS         256        /* a power of two: 5 seconds, a stall of the network is delivered in one go */
#endif
#define JB_SEQ_COUNT     21
#define JB_FWD           3          /* this many frames ahead of the highest is the next frame      */
#define JB_EARLY_MS      60         /* a frame is not further ahead of the line than this           */
#define JB_LATE_MS       360        /* nor further behind it than this                              */
#define JB_LOCK_MS       500        /* no frame ahead for this long: take the next one as new audio */
#define JB_IDLE_MS       2000       /* nothing for this long after the buffer ran dry: the end      */
#define JB_STALL_MS      250        /* no frame at all for this long is counted and logged as a stall */
#define JB_BEHIND_MS     60         /* behind the clock by this much: the clock is set again        */
#define JB_FRAME_BYTES   12         /* 9 bytes of AMBE, 3 of slow data                              */

typedef struct
{
    bool     active;
    bool     started;               /* playing out                                */
    bool     haveFrames;
    bool     endKnown;              /* the last frame has been seen               */
    uint32_t jitterMs;
    int64_t  hi;                    /* highest frame seen                         */
    int64_t  playPos;               /* next frame to play                         */
    int64_t  base;                  /* the frame the clock started at             */
    int64_t  endPos;                /* the frame that carried the end flag        */
    uint64_t playStart;             /* time of frame 'base'                       */
    uint64_t lastArrival;
    uint64_t lastForward;           /* when the highest frame last moved           */
    uint64_t anchor;                /* the time frame 0 would have come, if no delay */
    bool     valid[JB_SLOTS];
    uint8_t  data[JB_SLOTS][JB_FRAME_BYTES];

    /* statistics */
    uint32_t played;                /* frames of the stream that were played      */
    uint32_t concealed;             /* frames that never came                      */
    uint32_t late;                  /* frames that came after their turn          */
    uint32_t duplicate;
    uint32_t reordered;             /* frames that came out of order and were in time */
    uint32_t resyncs;
    uint32_t stalls;                /* times no frame at all came for JB_STALL_MS or more */
    uint32_t skipped;               /* frames thrown away because more were waiting than the buffer holds */
    uint32_t stallMs;               /* the time of all stalls together             */
    uint32_t longestStall;          /* the longest one                             */
    uint32_t lastStall;             /* the one that just ended, 0 once the caller has read it */
} DJitter;

typedef enum
{
    JB_RUNNING = 0,
    JB_END_FLAG,                    /* the end frame has been reached             */
    JB_END_IDLE                     /* the stream stopped without an end frame    */
} JbResult;

/* Called for every frame that is due; 'concealed' frames are silence made here */
typedef void (*JbEmit)(void* ctx, const uint8_t* frame, bool concealed);

static inline void jbReset(DJitter* j, uint32_t jitterMs)
{
    memset(j, 0x00, sizeof(*j));
    j->jitterMs = jitterMs;
    j->active   = true;
}

static inline void jbStop(DJitter* j)
{
    j->active = false;
}

/* AMBE silence with the slow data of a frame without a message (sync pattern in frame 0) */
static inline void jbSilence(uint8_t seq, uint8_t* frame)
{
    static const uint8_t silence[9] = { 0x9E, 0x8D, 0x32, 0x32, 0x26, 0x1A, 0x3F, 0x61, 0xE8 };

    memcpy(frame, silence, 9U);

    if (seq == 0U)
    {
        frame[9]  = 0x55;
        frame[10] = 0x2D;
        frame[11] = 0x16;
    }
    else
    {
        frame[9]  = 0x16;
        frame[10] = 0x29;
        frame[11] = 0xF5;
    }
}

/*
 * jbClassify()
 *   Works out which of the frames with this sequence number it is.  The number
 *   repeats every 21 frames (420 ms): the frame is either the one just ahead of
 *   the highest seen (c0) or the same number 21 frames earlier (c1).  A frame
 *   only a few ahead of the highest is the next one.  Otherwise the time of
 *   arrival decides: frames are sent 20 ms apart and the network only ever
 *   delays them, so every frame lies on or behind a line that the earliest
 *   frames define; one cannot be more than JB_EARLY_MS ahead of it, and is not
 *   more than JB_LATE_MS behind it.  A stream starts at frame 0, so a frame
 *   cannot be before that.
 */
static inline int64_t jbClassify(const DJitter* j, uint8_t seq, uint64_t now)
{
    int delta = (int)((seq + JB_SEQ_COUNT - (int)(j->hi % JB_SEQ_COUNT)) % JB_SEQ_COUNT);
    int64_t c0 = j->hi + delta;
    int64_t c1 = c0 - JB_SEQ_COUNT;
    int64_t l0;
    int64_t l1;
    bool f0;
    bool f1;

    if (delta <= JB_FWD || c1 < 0 || now - j->lastForward > JB_LOCK_MS)
        return c0;

    l0 = (int64_t)now - (int64_t)j->anchor - c0 * JB_FRAME_MS;
    l1 = l0 + (int64_t)JB_SEQ_COUNT * JB_FRAME_MS;
    f0 = l0 >= -JB_EARLY_MS && l0 <= JB_LATE_MS;
    f1 = l1 >= -JB_EARLY_MS && l1 <= JB_LATE_MS;

    if (f0 != f1)
        return f0 ? c0 : c1;

    /* neither or both: the one nearest to a typical delay */
    l0 = l0 > 100 ? l0 - 100 : 100 - l0;
    l1 = l1 > 100 ? l1 - 100 : 100 - l1;
    return l0 <= l1 ? c0 : c1;
}

/*
 * jbPut()
 *   One frame from the network.  'seq' is 0..20, 'last' is set on the frame
 *   that ends the stream (its data is not used).  Returns false when the
 *   frame is thrown away (late, duplicate or not a valid number).
 */
static inline bool jbPut(DJitter* j, uint8_t seq, bool last, const uint8_t* frame, uint64_t now)
{
    int64_t ext;

    if (!j->active || seq >= JB_SEQ_COUNT)
        return false;

    /* the time since the previous frame of any kind: what the network really delivered, whatever the numbers say */
    if (j->haveFrames && now - j->lastArrival >= JB_STALL_MS)
    {
        uint32_t gap = (uint32_t)(now - j->lastArrival);

        j->stalls++;
        j->stallMs += gap;
        if (gap > j->longestStall)
            j->longestStall = gap;
        j->lastStall = gap;
    }

    j->lastArrival = now;

    if (!j->haveFrames)
    {
        j->haveFrames  = true;
        j->hi          = seq;
        j->playPos     = seq;
        j->playStart   = now + j->jitterMs;
        j->lastForward = now;
        j->anchor      = now - (uint64_t)seq * JB_FRAME_MS;
        ext            = seq;
    }
    else
    {
        int64_t line;

        if (now - j->lastForward > JB_LOCK_MS)
            j->resyncs++;

        ext = jbClassify(j, seq, now);

        if (ext == j->hi)
        {
            j->duplicate++;
            return false;
        }

        if (!j->started && ext < j->playPos)
            j->playPos = ext;       /* before the clock starts, an earlier frame moves the start back */

        /* the line of the earliest frames: arrival time minus the frame's place in the stream */
        line = (int64_t)now - ext * JB_FRAME_MS;

        if (line < (int64_t)j->anchor)
            j->anchor = (uint64_t)line;
    }

    if (ext < j->playPos)
    {
        j->late++;
        return false;
    }

    if (ext > j->hi)
    {
        j->hi          = ext;
        j->lastForward = now;

        /* a stream that runs far ahead of the clock: skip what can no longer be played */
        if (j->hi - j->playPos >= JB_SLOTS - 4)
        {
            int64_t skip = j->hi - (JB_SLOTS - 4) - j->playPos + 1;

            for (int64_t p = j->playPos; p < j->playPos + skip; p++)
                j->valid[p & (JB_SLOTS - 1)] = false;

            j->playPos += skip;
            j->skipped += (uint32_t)skip;

            /* The clock gives every position its time (playStart + (playPos - base) * 20 ms).  Moving playPos on
               without moving the clock back makes the next frame due 'skip' frames later than it was, every frame
               that arrives meanwhile adds to the pile, the next skip is longer still, and the audio stops for
               seconds.  Move the clock back by the same amount and the next frame stays due when it was. */
            if (j->started)
                j->playStart -= (uint64_t)skip * JB_FRAME_MS;
        }
    }
    else if (j->valid[ext & (JB_SLOTS - 1)])
    {
        j->duplicate++;
        return false;
    }
    else if (ext < j->hi)
    {
        j->reordered++;     /* the first frame of a stream is equal to the highest: it is not out of order */
    }

    if (last)
    {
        j->endKnown = true;
        j->endPos   = ext;
        return true;
    }

    memcpy(j->data[ext & (JB_SLOTS - 1)], frame, JB_FRAME_BYTES);
    j->valid[ext & (JB_SLOTS - 1)] = true;
    return true;
}

/*
 * jbService()
 *   Releases the frames that are due.  Call it often (every few milliseconds
 *   at most); returns JB_END_FLAG when the end frame has been reached and
 *   JB_END_IDLE when nothing has come for a second after the buffer ran dry.
 */
static inline JbResult jbService(DJitter* j, uint64_t now, JbEmit emit, void* ctx)
{
    if (!j->active || !j->haveFrames)
        return JB_RUNNING;

    if (!j->started)
    {
        if (now < j->playStart)
            return JB_RUNNING;

        j->started = true;
        j->base    = j->playPos;
    }

    for (int n = 0; n < 3; n++)
    {
        uint64_t due = j->playStart + (uint64_t)(j->playPos - j->base) * JB_FRAME_MS;

        if (now < due)
            return JB_RUNNING;

        if (now > due + JB_BEHIND_MS)
        {
            /* the stream was held up: start the clock again from this frame */
            j->playStart = now - (uint64_t)(j->playPos - j->base) * JB_FRAME_MS;
        }

        if (j->endKnown && j->playPos >= j->endPos)
            return JB_END_FLAG;

        if (j->playPos > j->hi)
        {
            if (now - j->lastArrival > JB_IDLE_MS)
                return JB_END_IDLE;

            return JB_RUNNING;
        }

        {
            int slot = (int)(j->playPos & (JB_SLOTS - 1));
            uint8_t seq = (uint8_t)(j->playPos % JB_SEQ_COUNT);

            if (j->valid[slot])
            {
                emit(ctx, j->data[slot], false);
                j->valid[slot] = false;
                j->played++;
            }
            else
            {
                uint8_t frame[JB_FRAME_BYTES];

                jbSilence(seq, frame);
                emit(ctx, frame, true);
                j->concealed++;
            }

            j->playPos++;
        }
    }

    return JB_RUNNING;
}

#endif
