/***************************************************************************
 *   Copyright (C) 2026 by Rick KD0OSS                                     *
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
 ***************************************************************************/

#ifndef FM_DSP_H
#define FM_DSP_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FM_FS 24000
#define FM_CT_DECIM 20
#define FM_CT_FS (FM_FS / FM_CT_DECIM)
#define FM_CT_N 360
#define FM_CT_HOP 60
#define FM_CT_HOP_MS (FM_CT_HOP * 1000 / FM_CT_FS)
#define FM_CW_MAX_EVENTS 400

typedef struct
{
    float b0, b1, b2, a1, a2;
    float z1, z2;
} FmBiquad;

typedef struct
{
    FmBiquad lp[2];
    float    freq;
    float    offset;
    float    threshold;
    float    buf[FM_CT_N];
    uint16_t pos;
    uint16_t filled;
    uint16_t hopCount;
    uint8_t  decimCount;
    uint8_t  openHops;
    uint8_t  closeHops;
    uint8_t  posRun;
    uint8_t  negRun;
    bool     raw;
    bool     weak;
    bool     detected;
    float    lastFraction;
    float    lastRatio;
} FmCtcssDet;

typedef struct
{
    FmBiquad hp[2];
    float    level;
    float    alpha;
    float    openLevel;
    float    closeLevel;
    bool     open;
} FmNoiseSq;

typedef struct
{
    FmBiquad hp[2];
    FmBiquad lp[2];
    float    dcX;
    float    dcY;
} FmAudio;

typedef struct
{
    float phase;
    float step;
} FmTone;

typedef struct
{
    uint32_t len[FM_CW_MAX_EVENTS];
    uint8_t  on[FM_CW_MAX_EVENTS];
    uint16_t count;
    uint16_t index;
    uint32_t left;
    float    phase;
    float    step;
    float    env;
    bool     active;
} FmCw;

void  fmBiquadLowPass(FmBiquad* b, float fc, float q);
void  fmBiquadHighPass(FmBiquad* b, float fc, float q);
float fmBiquadRun(FmBiquad* b, float x);

void fmCtcssInit(FmCtcssDet* d, float freq, float thresholdPercent, uint32_t holdMs);
void fmCtcssReset(FmCtcssDet* d);
bool fmCtcssProcess(FmCtcssDet* d, float x);
void fmCtcssScan(const FmCtcssDet* d, float* bestFreq, float* bestFraction);

void  fmNoiseInit(FmNoiseSq* n, float openPercent, float closePercent);
void  fmNoiseReset(FmNoiseSq* n);
bool  fmNoiseProcess(FmNoiseSq* n, float x);

void  fmAudioInit(FmAudio* a);
float fmAudioDc(FmAudio* a, float x);
float fmAudioShape(FmAudio* a, float x);

void  fmToneInit(FmTone* t, float freq);
float fmToneNext(FmTone* t);

void fmCwStart(FmCw* c, const char* text, uint8_t wpm, float freq);
bool fmCwNext(FmCw* c, float* out);

#ifdef __cplusplus
}
#endif

#endif
