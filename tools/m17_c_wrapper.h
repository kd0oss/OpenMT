/***************************************************************************
 *   Copyright (C) 2025 by Rick KD0OSS                                     *
 *                                                                         *
 *   C Wrapper Interface for OpenRTX M17 C++ Implementation                *
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

#ifndef M17_C_WRAPPER_H
#define M17_C_WRAPPER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * This header provides a C-compatible interface to the OpenRTX M17 C++ implementation.
 * The actual M17 processing is done in C++ code located in the M17/ subdirectory.
 * 
 * These wrapper functions allow pure C code to interact with the M17 functionality
 * without needing C++ language features.
 */

/* Opaque handle types - actual implementations are C++ objects */
typedef void* M17Modulator_t;
typedef void* M17Demodulator_t;
typedef void* M17FrameEncoder_t;
typedef void* M17FrameDecoder_t;

/* M17 Callsign functions */
bool m17_callsign_encode(const char* callsign, uint8_t* encoded);
bool m17_callsign_decode(const uint8_t* encoded, char* callsign, size_t callsign_size);

/* M17 Modulator functions */
M17Modulator_t m17_modulator_create(void);
void m17_modulator_destroy(M17Modulator_t modulator);
void m17_modulator_process(M17Modulator_t modulator, const uint8_t* data, size_t length, int16_t* output);

/* M17 Demodulator functions */
M17Demodulator_t m17_demodulator_create(void);
void m17_demodulator_destroy(M17Demodulator_t demodulator);
bool m17_demodulator_process(M17Demodulator_t demodulator, const int16_t* samples, size_t length);

/* M17 Frame Encoder functions */
M17FrameEncoder_t m17_frame_encoder_create(void);
void m17_frame_encoder_destroy(M17FrameEncoder_t encoder);
void m17_frame_encoder_encode_stream(M17FrameEncoder_t encoder, const uint8_t* data, uint8_t* output);
void m17_frame_encoder_encode_packet(M17FrameEncoder_t encoder, const uint8_t* data, size_t length, uint8_t* output);

/* M17 Frame Decoder functions */
M17FrameDecoder_t m17_frame_decoder_create(void);
void m17_frame_decoder_destroy(M17FrameDecoder_t decoder);
bool m17_frame_decoder_decode_stream(M17FrameDecoder_t decoder, const uint8_t* data, uint8_t* output);
bool m17_frame_decoder_decode_packet(M17FrameDecoder_t decoder, const uint8_t* data, uint8_t* output, size_t* length);

/* Golay error correction */
uint32_t m17_golay_encode(uint16_t data);
uint16_t m17_golay_decode(uint32_t encoded);

#ifdef __cplusplus
}
#endif

#endif /* M17_C_WRAPPER_H */
