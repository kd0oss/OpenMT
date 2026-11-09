/*
 * rf_spectrum.h - RF spectrum of the received channel, for a monitor program (OpenMT Radio's Monitor tab).
 *
 * Used by the HackRF and SX1255 bridges.  The bridge's RX chain works on 125 kS/s complex baseband with the wanted channel at 0 Hz.  This module
 * takes an FFT of that signal and sends it as a UDP datagram to the programs that asked for it.  It has no thread of its own:
 * the RX thread calls it, and it costs nothing while nobody has asked.
 *
 * Protocol (all numbers little endian)
 *   request, monitor -> bridge   "SPEC"   (4 bytes; repeat at least every 5 s, else the bridge stops sending)
 *   frame,   bridge -> monitor
 *       0  "SPEC"
 *       4  u8   version = 1
 *       5  u8   flags: bit 0 = the HackRF's 8 bit samples hit full scale since the last frame (overload)
 *       6  u16  number of bins n
 *       8  u32  centre frequency, Hz (the channel being received)
 *      12  u32  span, Hz (bin k is at centre + (k - n/2) * span / n)
 *      16  u8   largest magnitude of the HackRF samples since the last frame (0 to 127)
 *      17  u8   reserved (0)
 *      18  u16  share of the HackRF samples at full scale since the last frame, in hundredths of a percent
 *      20  n x u8  power per bin: 0.5 dB steps below full scale, 0 = 0 dBFS, 255 = -127.5 dBFS or less
 *   A full scale complex tone (|x| = 1) is 0 dBFS in its bin (the bins are calibrated for tones).  To add the bins of a wider
 *   signal into a power, take 1.76 dB off the sum: the noise bandwidth of the Hann window is 1.5 bins.
 *   Frames are sent about 8 times a second.
 *
 * IQ side channel, same socket and port (the narrow band signal itself, for an analysis of the real deviation and the eye)
 *   request, monitor -> bridge   "IQST"   (4 bytes; repeat at least every 5 s)
 *   frame,   bridge -> monitor   (about 200 a second)
 *       0  "IQFR"
 *       4  u8   version = 1
 *       5  u8   flags: bit 0 = the converter hit full scale since the last frame
 *       6  u16  number of complex samples n (300)
 *       8  u32  centre frequency, Hz
 *      12  u32  sample rate, Hz (the span / 2: low passed at 22 kHz, the channel at 0 Hz)
 *      16  u32  sequence number of the frame (a gap = frames lost)
 *      20  u8   shift: the samples were multiplied by 2^shift before they were made 16 bit (divide it out)
 *      21  u8   1 = the frequency axis was turned round (RX invert): the samples are the true ones either way
 *      22  u16  reserved (0)
 *      24  n x (i16 I, i16 Q), full scale 1.0 = 32767 (before the shift)
 *   The frequency of the samples is the true one: a positive frequency is above the centre.
 */
#ifndef HRF_SPECTRUM_H
#define HRF_SPECTRUM_H

#include <complex.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define RFSPEC_BINS        1024U
#define RFSPEC_DEFAULT_PORT 18050U      /* the HackRF bridge */
#define RFSPEC_SX1255_PORT  18051U      /* the SX1255 bridge: both can run on one machine */

typedef struct hrf_spec rfspec_t;

/* port 0 or a socket that cannot be opened gives NULL.  bind_addr NULL = all interfaces. */
rfspec_t *rfspec_create(uint16_t port, const char *bind_addr, uint32_t span_hz);
void rfspec_destroy(rfspec_t *s);

void rfspec_set_freq(rfspec_t *s, uint32_t centre_hz);

/* Reads the requests that have arrived; true while at least one monitor wants frames.  Call it regularly (every RX block). */
bool rfspec_poll(rfspec_t *s);

/* The HackRF's raw 8 bit interleaved I/Q (n_bytes of them): tracks the largest magnitude and the number at full scale */
void rfspec_adc(rfspec_t *s, const int8_t *iq, size_t n_bytes);

/* The same for a converter that gives floating point I/Q with full scale 1.0 (the SX1255's): peak and share at full scale */
void rfspec_level(rfspec_t *s, const float complex *iq, size_t n);

/* The bridge's RX invert turns the spectrum round (it negates the demodulated signal): tell the module so the frequency axis of the
   frames is the true one. Call it before rfspec_feed() */
void rfspec_set_mirror(rfspec_t *s, bool mirror);

/* Baseband samples at span_hz; sends a spectrum frame and the IQ frames when they are due.  span_hz must be even (the IQ rate is half of it) */
void rfspec_feed(rfspec_t *s, const float complex *bb, size_t n);

#ifdef __cplusplus
}
#endif

#endif
