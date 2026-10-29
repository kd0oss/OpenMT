/*
 *   Copyright (C) 2026 by Rick KD0OSS
 *
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
 * OpenMT "OPENMTHS" bit-stream modem protocol.
 *
 * Serial framing (host <-> modem), as used by OpenMT Modem_Host.c:
 *
 *   [0x61] [total length incl. these 3 bytes] [type | id] [payload...]
 *
 * The high nibble of the type byte is the message type, the low nibble is a
 * connection id (protocol service slot on the host) or a parameter.
 *
 * Host -> modem
 *   SET_CONFIG  0x20         payload[0] = flags (see OPENMT_CFG_*)
 *   GET_STATUS  0x10         no payload, modem answers with a status frame
 *   GET_VERSION 0x00         no payload, modem answers with its version
 *   SET_MODE    0x30 | conn  lock modem to conn, 0x3F = idle (resume scan)
 *   ADD_MODE    0x40         payload[0] = conn, payload[1..58] = mode data
 *   DEL_MODE    0x50 | conn  remove a registered mode
 *   BIT_DATA    0x60 | conn  payload = TX bits, packed 8 per byte
 *
 * Modem -> host
 *   BIT_DATA    0x60 | conn  payload = RX bits, first received bit is the
 *                            MSB of the first byte
 *   GET_STATUS  0x10 | space free TX buffer space in BIT frames (0..15),
 *                            sent unsolicited when it changes to/from zero
 *   GET_VERSION 0x00         payload = protocol version + description
 *   ACK         0xE0         payload[0] = type byte being acknowledged
 *   NAK         0xF0         payload[0] = type byte, payload[1] = error
 *   DEBUG       0xC0         payload = debug text (+ values)
 *
 * ADD_MODE mode data (58 bytes, built by the protocol services, multi-byte
 * values little endian):
 *    0..39  ADF7021 registers: RX REG0, TX REG0, REG1, REG2, REG3, REG4,
 *           REG5, REG6, REG10, REG13 (uint32 each)
 *   40..47  sync pattern (uint64)
 *   48..55  sync mask    (uint64)
 *   56      reserved (stored, not used by this firmware)
 *   57      bit 4: 1 = transmit bits MSB first, 0 = LSB first
 *           bit 5: 1 = append the TX stream position (2 bytes, big endian,
 *                  bit count modulo 576) to every BIT_DATA frame sent to
 *                  the host; 0xFFFF while the modem is not transmitting
 *           low nibble: scan dwell multiplier (0 is treated as 1)
 *
 * DMR: if the registered sync is one of the four 48 bit DMR data syncs, the
 * modem also matches the paired voice sync (every symbol inverted, XOR
 * 0xAAAAAAAAAAAA), so a voice call keeps the stream alive, and starts the
 * stream with 192 bits of history instead of 64: the sync sits in the middle
 * of the 264 bit burst, so the host needs the 108 bits before it. DMR bytes
 * go out in symbol order, MSB first, so the host must send 0x1n in byte 57.
 */

#if !defined(OPENMT_H)
#define  OPENMT_H

#include <stdint.h>

// ---------------------------------------------------------------------------
// Wire protocol
// ---------------------------------------------------------------------------
const uint8_t OPENMT_FRAME_START   = 0x61U;

const uint8_t OPENMT_GET_VERSION   = 0x00U;
const uint8_t OPENMT_GET_STATUS    = 0x10U;
const uint8_t OPENMT_SET_CONFIG    = 0x20U;
const uint8_t OPENMT_SET_MODE      = 0x30U;
const uint8_t OPENMT_ADD_MODE      = 0x40U;
const uint8_t OPENMT_DEL_MODE      = 0x50U;
const uint8_t OPENMT_BIT_DATA      = 0x60U;
const uint8_t OPENMT_SAMP_DATA     = 0x70U;
const uint8_t OPENMT_TO_MODEM      = 0x80U;
const uint8_t OPENMT_SAMP_EOT      = 0x90U;
const uint8_t OPENMT_BIT_EOT       = 0xA0U;
const uint8_t OPENMT_DEBUG         = 0xC0U;
const uint8_t OPENMT_MEAS          = 0xD0U;   // MEAS | conn: receive frequency measurement (see COpenMT::measure)
const uint8_t OPENMT_ACK           = 0xE0U;
const uint8_t OPENMT_NAK           = 0xF0U;

const uint8_t OPENMT_TYPE_MASK     = 0xF0U;
const uint8_t OPENMT_ID_MASK       = 0x0FU;
const uint8_t OPENMT_MODE_IDLE     = 0x0FU;   // SET_MODE | 0x0F = go idle

const uint8_t OPENMT_PROTOCOL_VERSION = 1U;

// SET_CONFIG flags (payload[0]). Same bit layout as the "openmt" modem type
// in Modem_Host.c. Bit 7 is set by the "openmths" handshake and is ignored.
const uint8_t OPENMT_CFG_DUPLEX    = 0x01U;
const uint8_t OPENMT_CFG_RX_INVERT = 0x02U;
const uint8_t OPENMT_CFG_TX_INVERT = 0x04U;
const uint8_t OPENMT_CFG_PTT_INVERT = 0x08U;

// NAK error codes
const uint8_t OPENMT_ERR_UNSUPPORTED = 1U;    // unknown or unsupported type
const uint8_t OPENMT_ERR_NO_MODE     = 2U;    // no mode registered/selected
const uint8_t OPENMT_ERR_BAD_CONN    = 3U;    // connection id not usable
const uint8_t OPENMT_ERR_BAD_DATA    = 4U;    // bad length or bad values
const uint8_t OPENMT_ERR_BUSY        = 5U;    // transmitting another mode
const uint8_t OPENMT_ERR_NO_SPACE    = 6U;    // TX buffer full / table full
const uint8_t OPENMT_ERR_TX_DATA     = 8U;    // MEAS: transmit bits are waiting in the modem (not keyed yet or still draining)

// Mode data layout
const uint8_t OPENMT_MODE_DATA_LEN = 58U;
const uint8_t OPENMT_MAX_CONN_ID   = 14U;     // 0x0F is reserved for idle

// Bits sent to the host ahead of the sync match when streaming starts
const uint8_t OPENMT_HISTORY_BYTES     = 8U;      // 64 bits, ends with the sync
const uint8_t OPENMT_HISTORY_BYTES_DMR = 24U;     // 192 bits: a DMR burst has 108
                                                  // bits before and 108 after its
                                                  // 48 bit sync (264 bits in all)

// Host side frames carry 192 bytes of bits (dstar_host writeBits())
const uint16_t OPENMT_BIT_FRAME_BYTES = 192U;
const uint16_t OPENMT_BIT_FRAME_BITS  = OPENMT_BIT_FRAME_BYTES * 8U;

// The TX bit buffer gets 8 bits of slack and is never filled completely:
// CBitRB's "full" flag is shared between the main loop (put) and the
// ADF7021 interrupt (get) and can be left set wrongly if both race at the
// moment the buffer becomes exactly full.
const uint16_t OPENMT_TX_BUFFER_SLACK = 8U;

// ---------------------------------------------------------------------------
// Tunables (may be overridden from Config.h)
// ---------------------------------------------------------------------------

// Maximum number of protocol services registered at the same time
#if !defined(OPENMT_MAX_MODES)
#define OPENMT_MAX_MODES            8U
#endif

// TX bit buffer, in host BIT frames (192 bytes = 1536 bits each)
#if !defined(OPENMT_TX_BUFFER_FRAMES)
#define OPENMT_TX_BUFFER_FRAMES     8U
#endif

// Frames held back from the space reported to the host, to cover frames
// already in flight on the serial line when the host sees "no space"
#if !defined(OPENMT_TX_SPACE_MARGIN)
#define OPENMT_TX_SPACE_MARGIN      2U
#endif

// Key the transmitter once this many bits are buffered...
#if !defined(OPENMT_TX_START_BITS)
#define OPENMT_TX_START_BITS        (2U * OPENMT_BIT_FRAME_BITS)
#endif

// ...or once the first bits have waited this long (ms)
#if !defined(OPENMT_TX_START_TIMEOUT_MS)
#define OPENMT_TX_START_TIMEOUT_MS  350U
#endif

// RX bytes collected before a BIT frame is sent to the host
#if !defined(OPENMT_RX_FRAME_BYTES)
#define OPENMT_RX_FRAME_BYTES       24U
#endif

// Stop streaming RX bits this long (ms) after the last sync match, unless
// the host has locked the mode with SET_MODE
#if !defined(OPENMT_LOCK_TIMEOUT_MS)
#define OPENMT_LOCK_TIMEOUT_MS      1500U
#endif

// Lock timeout learning. While the host has the mode locked, the longest gap
// between two syncs is measured; afterwards, when the host has not locked,
// streaming ends LOCK_FACTOR_PERCENT percent of that gap after the last sync
// (never less than OPENMT_LOCK_MIN_MS, never more than OPENMT_LOCK_TIMEOUT_MS,
// which is also used until a mode has been learned). A false sync then only
// stalls the scan for a fraction of a second. Set OPENMT_LOCK_LEARN to 0 to
// always use OPENMT_LOCK_TIMEOUT_MS.
#if !defined(OPENMT_LOCK_LEARN)
#define OPENMT_LOCK_LEARN           1
#endif

#if !defined(OPENMT_LOCK_MIN_MS)
#define OPENMT_LOCK_MIN_MS          250U
#endif

#if !defined(OPENMT_LOCK_FACTOR_PERCENT)
#define OPENMT_LOCK_FACTOR_PERCENT  250U
#endif

// Send the first BIT frame as soon as a sync is found, even when it is shorter
// than OPENMT_RX_FRAME_BYTES (saves 13 to 27 ms on the way to the host).
#if !defined(OPENMT_SYNC_FLUSH)
#define OPENMT_SYNC_FLUSH           1
#endif

// Skip empty channels while scanning. A short time after a mode is loaded the
// received signal strength is read; when it is weaker than the threshold the
// scanner moves on at once instead of listening for the whole dwell. The
// value is the readRSSI() result, -dBm: 115 means "carrier present when
// stronger than -115 dBm". A noise floor above that threshold only makes the
// scan behave as before. Set OPENMT_RSSI_SCAN to 0 to disable.
#if !defined(OPENMT_RSSI_SCAN)
#define OPENMT_RSSI_SCAN            1
#endif

#if !defined(OPENMT_RSSI_THRESHOLD)
#define OPENMT_RSSI_THRESHOLD       115U
#endif

#if !defined(OPENMT_RSSI_SETTLE_MS)
#define OPENMT_RSSI_SETTLE_MS       2U
#endif

// While scanning (two or more modes registered, nothing streaming or locked) the
// D-Star and DMR LEDs blink alternately, this many ms each, whichever mode is
// being listened to. A mode LED shows the modulation of the mode in use only
// while a mode is streaming or locked, or when a single mode is registered.
#if !defined(OPENMT_SCAN_LED_MS)
#define OPENMT_SCAN_LED_MS          250U
#endif

// RX frequency measurement (MEAS command, used by the calibration program).
// The mode is loaded with the ADF7021 internal AFC switched off; the AFC readback
// is then the intermediate frequency (100 kHz without error), read twice,
// OPENMT_MEAS_SETTLE_MS apart. The step and AFC range options below are used
// by the register 7 diagnostic (probe) only.
#if !defined(OPENMT_MEAS_SETTLE_MS)
#define OPENMT_MEAS_SETTLE_MS       60U
#endif

#if !defined(OPENMT_MEAS_STEP_COUNTS)
#define OPENMT_MEAS_STEP_COUNTS     16U
#endif

// Value for the maximum AFC range field (register 10 bits 24-31)
#if !defined(OPENMT_MEAS_AFC_RANGE)
#define OPENMT_MEAS_AFC_RANGE       12U
#endif

// Scan dwell per mode, in ADF7021 clock edges, multiplied by the mode's
// scan multiplier (same unit as SCAN_TIME in the MMDVM_HS firmware)
#if !defined(OPENMT_SCAN_TIME)
#define OPENMT_SCAN_TIME            1920U
#endif

// Allowed bit errors in a sync match: (mask bits - 16) / divisor,
// e.g. 16 bit sync -> 0, 32 bit -> 1, 48 bit -> 2
// Allowed bit errors in the 48-bit DMR sync (the host checks again, with the same limit)
#if !defined(OPENMT_DMR_SYNC_ERRS)
#define OPENMT_DMR_SYNC_ERRS        4U
#endif

#if !defined(OPENMT_SYNC_ERR_DIVISOR)
#define OPENMT_SYNC_ERR_DIVISOR     16U
#endif

// Send a short debug frame (0xC0) for the events that matter when bringing up
// a protocol: mode added (with its PA setting), TX keyed/ended, RX stream on/off.
// Modem_Host shows them as "Modem rec data". Set to 0 in Config.h to disable.
#if !defined(OPENMT_DEBUG_EVENTS)
#define OPENMT_DEBUG_EVENTS         1
#endif

// Diagnostic: while the modem transmits, retune the second ADF7021 (duplex
// boards only) to the TX frequency so it listens to the modem's own signal,
// count the DMR BS syncs it hears and check they arrive every 288 bits (one
// slot: 24 bit CACH + 264 bit burst). Reported as "LB ..." debug events after
// "TX end". Proves the transmitted bit stream is a valid DMR downlink as an
// ADF7021 demodulator sees it. The radio's reply cannot be heard while this
// is active. Set to 0 to disable.
#if !defined(OPENMT_LOOPBACK_TEST)
#define OPENMT_LOOPBACK_TEST        0
#endif

// Discard a partially received serial frame after this many ms of silence
#if !defined(OPENMT_SERIAL_TIMEOUT_MS)
#define OPENMT_SERIAL_TIMEOUT_MS    100U
#endif

// ---------------------------------------------------------------------------
// Registered mode
// ---------------------------------------------------------------------------
struct OpenMTMode {
    bool     used;
    uint8_t  connId;

    // ADF7021 registers, exactly as built by the protocol service
    uint32_t rxReg0;
    uint32_t txReg0;
    uint32_t reg1;
    uint32_t reg2;
    uint32_t reg3;
    uint32_t reg4;
    uint32_t reg5;
    uint32_t reg6;
    uint32_t reg10;
    uint32_t reg13;

    uint64_t syncData;
    uint64_t syncMask;
    uint8_t  syncErrs;
    bool     rxTrailer;    // append the TX stream position (2 bytes) to every BIT_DATA frame

    // DMR style sync (see isDmrSync() in OpenMT.cpp): the burst sync sits in
    // the middle of the burst, so more history is sent when streaming starts,
    // and the voice sync that pairs with the registered data sync also counts
    // as a sync match. syncAlt is 0 when unused.
    uint64_t syncAlt;
    uint8_t  historyBytes;

    uint8_t  reserved;
    bool     txMsbFirst;
    uint8_t  scanMult;

    uint16_t lockMs;       // streaming timeout without host lock (learned, see OPENMT_LOCK_LEARN)
    uint16_t gapMaxMs;     // longest sync to sync gap seen while the host had the mode locked

    uint32_t rxFreq;
    uint32_t txFreq;
};

// Result of a MEAS command, sent to the host as MEAS | conn with these 11 bytes
// (16 bit values, low byte first): afc0, afc1, step, rssi, syncs, flags.
struct OpenMTMeas {
    int16_t  afc0;      // AFC readback (the IF frequency: Hz = afc * step / 64)
    int16_t  afc1;      // the same, one settle time later
    int16_t  step;      // Hz per count of the readback, in 1/64 Hz
    uint16_t rssi;      // -dBm, as readRSSI()
    uint16_t syncs;     // sync patterns of the mode seen while measuring
    uint8_t  flags;     // reserved, 0
};

class COpenMT {
public:
    COpenMT();

    // Host commands, return 0 or an OPENMT_ERR_* code
    uint8_t setConfig(const uint8_t* data, uint8_t length);
    uint8_t addMode(const uint8_t* data, uint8_t length);
    uint8_t delMode(uint8_t connId);
    uint8_t setMode(uint8_t connId);
    uint8_t writeBits(uint8_t connId, const uint8_t* data, uint8_t length);
    uint8_t measure(uint8_t connId, OpenMTMeas& result);
    uint8_t probe(uint8_t connId, uint8_t pass, uint8_t half, uint16_t* words);

    // Free TX space reported to the host, in BIT frames (0..15)
    uint8_t getSpace() const;

    void    process();

private:
    OpenMTMode m_modes[OPENMT_MAX_MODES];
    int8_t     m_current;          // index of the mode loaded in the ADF7021
    uint8_t    m_modeCount;

    bool       m_rxInvert;
    bool       m_txInvert;

    // RX
    uint64_t   m_rxShift;          // newest 64 bits, newest bit in bit 0
    uint64_t   m_rxHist0;          // the 64 bits before those
    uint64_t   m_rxHist1;          // and the 64 before those
    bool       m_streaming;
    bool       m_hostLocked;
    uint32_t   m_lockTime;
    bool       m_syncSeen;         // m_lockTime is the time of a real sync, not of the stream start
    uint8_t    m_rxFrame[OPENMT_RX_FRAME_BYTES + 8U];
    uint16_t   m_rxBits;

    // Scanning
    uint32_t   m_scanStart;
    uint32_t   m_scanStartMs;
    bool       m_rssiChecked;      // the carrier check of this dwell is done
    uint32_t   m_rssiLast;         // time of the last RSSI debug event
    uint32_t   m_scanLedMs;        // time of the last scan LED change
    bool       m_scanLedPhase;     // false: DMR LED, true: D-Star LED
    bool       m_reload;           // current mode changed during TX

    // TX
    bool       m_txWaiting;
    uint32_t   m_txWaitStart;
    bool       m_txWasOn;
    uint16_t   m_txSyncs;
    uint16_t   m_nearMiss;
    uint32_t   m_nearLast;
    uint32_t   m_lbBits;           // loopback test counters
    uint32_t   m_lbLast;
    uint16_t   m_lbSyncs;
    uint16_t   m_lbGood;
    uint16_t   m_lbBadDelta;
    bool       m_lbActive;
    uint32_t   m_txOnStart;
    bool       m_lastSpaceZero;

    int8_t  findMode(uint8_t connId) const;
    bool    txBusy() const;
    void    selectMode(int8_t index);
    void    restartDwell();
    void    scanNext();
    uint16_t listen(const OpenMTMode& mode, uint32_t ms);
    void    processRX();
    void    processTX();
    void    processScan();
    void    processStatus();
    void    startStreaming(bool withHistory);
    void    stopStreaming();
    void    putRXBit(uint8_t bit);
    void    flushRX();
    void    setModeLEDs();
    bool    isScanning() const;
    void    updateScanLEDs();
};

#endif
