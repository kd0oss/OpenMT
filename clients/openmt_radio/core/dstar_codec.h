/* D-STAR header coding: see dstar_codec.c (generated from the host's own code). */
#ifndef OPENMT_DSTAR_CODEC_H
#define OPENMT_DSTAR_CODEC_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 41 byte header (flags 3, RPT2 8, RPT1 8, UR 8, MY 8, suffix 4, CRC 2 as the host takes it; the CRC is added by the
   caller with dstarHeaderCrc) -> 83 bytes as sent: convolutional code, interleave, scramble (bits are sent LSB first) */
void dstarTxHeader(const uint8_t* in, uint8_t* out);

/* 83 received bytes (changed) -> 41 byte header; true when the CRC is right */
bool dstarRxHeader(uint8_t* in, uint8_t* out);

/* The CRC of the first 39 bytes, put into bytes 39 and 40 */
void dstarHeaderCrc(uint8_t* header);

#ifdef __cplusplus
}
#endif

#endif
