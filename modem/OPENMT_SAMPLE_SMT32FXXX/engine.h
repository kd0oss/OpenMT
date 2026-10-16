/*
 *  engine.h - OpenMT SAMPLE-mode protocol and sample engine.
 */

#ifndef ENGINE_H
#define ENGINE_H

#include <stdint.h>
#include <stdbool.h>

/* OpenMT frame types (high nibble of the type byte) */
#define OMT_START           0x61U
#define OMT_SAMP_EOT        0x00U   /* [0x61][0x03][0x00]            */
#define OMT_GET_STATUS      0x10U   /* reply low nibble = free space */
#define OMT_SET_CONFIG      0x20U
#define OMT_SET_MODE        0x30U
#define OMT_ADD_MODE        0x40U
#define OMT_DEL_MODE        0x50U
#define OMT_BIT_DATA        0x60U
#define OMT_SAMP_DATA       0x70U
#define OMT_TO_MODEM        0x80U
#define OMT_TX_TIMING       0xB0U   /* device -> host: TX/RX alignment */
#define OMT_DEBUG           0xC0U
#define OMT_GET_FW_VERSION  0xD0U   /* host -> device; D0 = version (reply is a DEBUG
                                       text frame), D1 + "BOOT" = reboot into the
                                       ROM boot loader */
#define OMT_ACK             0xE0U
#define OMT_NAK             0xF0U

#define OMT_ERR_NO_SPACE    6U

void engine_init(void);

/* Call as often as possible from the main loop */
void engine_poll(void);

/* Statistics (also used by the unit tests) */
typedef struct
{
    uint32_t rxFramesToHost;
    uint32_t txFramesFromHost;
    uint32_t txOverflow;
    uint32_t txUnderrun;
    uint32_t rxDropped;
    uint32_t badBytes;
    uint32_t lostFrames;                /* sample frames replaced by silence */
    uint32_t adcClip;
    uint32_t keyUps;
} engine_stats_t;

const engine_stats_t *engine_stats(void);

/* Test / diagnostic helpers */
bool     engine_is_keyed(void);
bool     engine_configured(void);

#endif /* ENGINE_H */
