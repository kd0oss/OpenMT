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
 * OpenMT bit-stream modem logic (modem type "openmths").
 *
 * The protocol services on the host own everything mode specific: they send
 * fully built ADF7021 register sets plus a sync pattern for each mode
 * (ADD_MODE), do all framing/FEC, and exchange raw demodulated bits with
 * the modem. This file only:
 *
 *   - keeps the table of registered modes, keyed by connection id,
 *   - loads a mode into the ADF7021 and scans between modes when idle,
 *   - watches the received bits for the current mode's sync pattern and,
 *     once found, streams the bits to the host (BIT_DATA | conn),
 *   - queues host bits for transmission, keys the transmitter once enough
 *     is buffered, and tells the host when the TX buffer is full.
 *
 * Receive locking:
 *   sync match            -> start streaming, the bits up to and including
 *                            the sync are sent first (64 bits, 192 bits for
 *                            DMR) so the host decoder sees the whole sync
 *   host SET_MODE | conn  -> stay locked until SET_MODE | 0x0F (idle)
 *   no sync for the mode's lock timeout (learned, at most OPENMT_LOCK_TIMEOUT_MS)
 *   and no host lock -> stop, resume scan
 *
 * Scanning: a mode is skipped after a few ms when its channel is empty (RSSI).
 */

#include "Config.h"
#include "Globals.h"
#include "ADF7021.h"
#include "OpenMT.h"

#if OPENMT_RX_FRAME_BYTES > 252U
#error "OPENMT_RX_FRAME_BYTES must fit in one serial frame (max 252)"
#endif

// The DMR history must fit in the RX frame buffer (frame bytes + 8)
typedef char openmt_history_size_check[(OPENMT_HISTORY_BYTES_DMR <= (OPENMT_RX_FRAME_BYTES + 8U)) ? 1 : -1];

#if (OPENMT_TX_BUFFER_FRAMES * 1536U) > 65520U
#error "OPENMT_TX_BUFFER_FRAMES is too large for the bit ring buffer"
#endif

#if OPENMT_TX_START_BITS > (OPENMT_TX_BUFFER_FRAMES * 1536U)
#error "OPENMT_TX_START_BITS is larger than the TX buffer"
#endif

#if OPENMT_DEBUG_EVENTS
#define OPENMT_EVENT(text, a, b) serial.writeDebug(text, (int16_t)(a), (int16_t)(b))
#else
#define OPENMT_EVENT(text, a, b) do { } while (0)
#endif

static uint32_t clampMs(uint32_t ms)
{
    return (ms > 32767U) ? 32767U : ms;
}

static uint32_t readLE32(const uint8_t* p)
{
    return ((uint32_t)p[0U] << 0) |
           ((uint32_t)p[1U] << 8) |
           ((uint32_t)p[2U] << 16) |
           ((uint32_t)p[3U] << 24);
}

static uint64_t readLE64(const uint8_t* p)
{
    return ((uint64_t)readLE32(p + 4U) << 32) | (uint64_t)readLE32(p);
}

// True if the masked bits differ from the sync pattern in no more than
// the allowed number of places. Stops counting as soon as it is exceeded.
static bool syncCompare(uint64_t bits, uint64_t pattern, uint64_t mask, uint8_t allowed)
{
    uint64_t diff = (bits & mask) ^ pattern;
    uint8_t errs = 0U;

    while (diff != 0ULL)
    {
        diff &= diff - 1ULL;
        if (++errs > allowed)
            return false;
    }

    return true;
}

static uint8_t syncErrorCount(uint64_t bits, const OpenMTMode& mode)
{
    uint64_t diff = (bits & mode.syncMask) ^ mode.syncData;
    uint8_t best = 0U;

    while (diff != 0ULL)
    {
        diff &= diff - 1ULL;
        best++;
    }

    if (mode.syncAlt != 0ULL)
    {
        uint8_t alt = 0U;
        diff = (bits & mode.syncMask) ^ mode.syncAlt;
        while (diff != 0ULL)
        {
            diff &= diff - 1ULL;
            alt++;
        }

        if (alt < best)
            best = alt;
    }

    return best;
}

static bool syncMatch(uint64_t bits, const OpenMTMode& mode)
{
    if (syncCompare(bits, mode.syncData, mode.syncMask, mode.syncErrs))
        return true;

    return mode.syncAlt != 0ULL && syncCompare(bits, mode.syncAlt, mode.syncMask, mode.syncErrs);
}

#if OPENMT_LOOPBACK_TEST && defined(DUPLEX)
// RX REG0 that receives on the TX frequency: the TX N/F dividers lowered by
// the 100 kHz receive IF (the host does the same for the RX frequency)
static uint32_t loopbackReg0(const OpenMTMode& mode)
{
    uint32_t n = (mode.txReg0 >> 19) & 0xFFU;
    uint32_t f = (mode.txReg0 >> 4) & 0x7FFFU;
    uint32_t units = (uint32_t)(((uint64_t)100000U * 65536ULL) / (uint32_t)ADF7021_PFD);

    if (mode.reg1 == ADF7021_REG1_UHF2)
        units /= 2U;

    uint32_t v = ((n << 15) | f) - units;
    return (mode.rxReg0 & 0xF8000000U) | (((v >> 15) & 0xFFU) << 19) | ((v & 0x7FFFU) << 4);
}

static bool loopbackSync(uint64_t bits)
{
    // BS data sync and its voice pair, up to 2 bit errors
    return syncCompare(bits, 0x0000DFF57D75DF5DULL, 0x0000FFFFFFFFFFFFULL, 2U) ||
           syncCompare(bits, 0x0000755FD7DF75F7ULL, 0x0000FFFFFFFFFFFFULL, 2U);
}
#endif

// The four 48 bit DMR data sync words (ETSI TS 102 361-1 table 9.2): MS, BS
// and the two single slot (direct mode) ones. The voice sync of each is the
// same word with every symbol inverted, which for the dibit mapping is an
// XOR with 0xAAAAAAAAAAAA.
static const uint64_t DMR_SYNC_MASK       = 0x0000FFFFFFFFFFFFULL;
static const uint64_t DMR_VOICE_SYNC_FLIP = 0x0000AAAAAAAAAAAAULL;

static bool isDmrSync(uint64_t data, uint64_t mask)
{
    static const uint64_t dataSyncs[4U] =
    {
        0x0000D5D7F77FD757ULL,    // MS data
        0x0000DFF57D75DF5DULL,    // BS data
        0x0000F7FDD5DDFD55ULL,    // direct slot 1 data
        0x0000D7557F5FF7F5ULL     // direct slot 2 data
    };

    if (mask != DMR_SYNC_MASK)
        return false;

    for (uint8_t i = 0U; i < 4U; i++)
    {
        if (data == dataSyncs[i])
            return true;
    }

    return false;
}

// Recover the frequency programmed into an ADF7021 REG0 value, using the
// same PFD/divider rules as ifConf(). Used for the band/ban checks only.
static uint32_t reg0ToFrequency(uint32_t reg0, uint32_t reg1, bool rx)
{
    uint32_t n = (reg0 >> 19) & 0xFFU;
    uint32_t f = (reg0 >> 4) & 0x7FFFU;
    float pfd;

    if (reg1 == ADF7021_REG1_UHF2)
        pfd = ADF7021_PFD;
    else
        pfd = ADF7021_PFD / 2.0F;

    float freq = ((float)n + ((float)f / 32768.0F)) * pfd;

    // The RX synthesizer runs 100 kHz below the channel (IF offset)
    if (rx)
        freq += 100000.0F;

    return (uint32_t)(freq + 0.5F);
}

COpenMT::COpenMT() :
m_modes(),
m_current(-1),
m_modeCount(0U),
m_rxInvert(false),
m_txInvert(false),
m_rxShift(0ULL),
m_rxHist0(0ULL),
m_rxHist1(0ULL),
m_streaming(false),
m_hostLocked(false),
m_lockTime(0U),
m_syncSeen(false),
m_rxFrame(),
m_rxBits(0U),
m_scanStart(0U),
m_scanStartMs(0U),
m_rssiChecked(false),
m_rssiLast(0U),
m_scanLedMs(0U),
m_scanLedPhase(false),
m_reload(false),
m_txWaiting(false),
m_txWaitStart(0U),
m_txWasOn(false),
m_txSyncs(0U),
m_nearMiss(0U),
m_nearLast(0U),
m_lbBits(0U),
m_lbLast(0U),
m_lbSyncs(0U),
m_lbGood(0U),
m_lbBadDelta(0U),
m_lbActive(false),
m_txOnStart(0U),
m_lastSpaceZero(false)
{
    for (uint8_t i = 0U; i < OPENMT_MAX_MODES; i++)
        m_modes[i].used = false;
}

// SET_CONFIG starts a new host session: forget all registered modes, the
// protocol services register again with ADD_MODE as they connect.
uint8_t COpenMT::setConfig(const uint8_t* data, uint8_t length)
{
    if (length < 1U)
        return OPENMT_ERR_BAD_DATA;

    uint8_t flags = data[0U];

    stopStreaming();
    m_hostLocked = false;

    for (uint8_t i = 0U; i < OPENMT_MAX_MODES; i++)
        m_modes[i].used = false;

    m_modeCount = 0U;
    m_current   = -1;
    m_reload    = false;

    m_rxInvert = (flags & OPENMT_CFG_RX_INVERT) != 0U;
    m_txInvert = (flags & OPENMT_CFG_TX_INVERT) != 0U;

#if defined(DUPLEX)
    m_duplex = (flags & OPENMT_CFG_DUPLEX) != 0U;
#else
    m_duplex = false;
#endif

    io.setMode(STATE_IDLE);
    io.start();

    // Tell the host where the TX buffer stands
    m_lastSpaceZero = (getSpace() == 0U);
    serial.writeStatus(getSpace());

    return 0U;
}

uint8_t COpenMT::addMode(const uint8_t* data, uint8_t length)
{
    if (length < (1U + OPENMT_MODE_DATA_LEN))
        return OPENMT_ERR_BAD_DATA;

    uint8_t connId = data[0U];
    if (connId > OPENMT_MAX_CONN_ID)
        return OPENMT_ERR_BAD_CONN;

    const uint8_t* d = data + 1U;
    OpenMTMode mode;

    mode.used   = true;
    mode.connId = connId;

    mode.rxReg0 = readLE32(d + 0U);
    mode.txReg0 = readLE32(d + 4U);
    mode.reg1   = readLE32(d + 8U);
    mode.reg2   = readLE32(d + 12U);
    mode.reg3   = readLE32(d + 16U);
    mode.reg4   = readLE32(d + 20U);
    mode.reg5   = readLE32(d + 24U);
    mode.reg6   = readLE32(d + 28U);
    mode.reg10  = readLE32(d + 32U);
    mode.reg13  = readLE32(d + 36U);

    // Register numbers live in the low nibble, reject anything else
    if ((mode.rxReg0 & 0x0FU) != 0x00U || (mode.txReg0 & 0x0FU) != 0x00U ||
        (mode.reg1 & 0x0FU) != 0x01U || (mode.reg2 & 0x0FU) != 0x02U ||
        (mode.reg3 & 0x0FU) != 0x03U || (mode.reg4 & 0x0FU) != 0x04U ||
        (mode.reg5 & 0x0FU) != 0x05U || (mode.reg6 & 0x0FU) != 0x06U ||
        (mode.reg10 & 0x0FU) != 0x0AU || (mode.reg13 & 0x0FU) != 0x0DU)
        return OPENMT_ERR_BAD_DATA;

    mode.rxTrailer = (d[57U] & 0x20U) != 0U;
    mode.syncData = readLE64(d + 40U);
    mode.syncMask = readLE64(d + 48U);

    uint8_t syncBits = countBits64(mode.syncMask);
    if (syncBits > 16U)
        mode.syncErrs = (syncBits - 16U) / OPENMT_SYNC_ERR_DIVISOR;
    else
        mode.syncErrs = 0U;

    mode.syncAlt      = 0ULL;
    mode.historyBytes = OPENMT_HISTORY_BYTES;

    if (isDmrSync(mode.syncData, mode.syncMask))
    {
        mode.syncAlt      = mode.syncData ^ DMR_VOICE_SYNC_FLIP;
        mode.historyBytes = OPENMT_HISTORY_BYTES_DMR;
        mode.syncErrs     = OPENMT_DMR_SYNC_ERRS;
    }

    mode.lockMs     = OPENMT_LOCK_TIMEOUT_MS;
    mode.gapMaxMs   = 0U;
    mode.reserved   = d[56U];
    mode.txMsbFirst = (d[57U] & 0x10U) != 0U;
    mode.scanMult   = d[57U] & 0x0FU;
    if (mode.scanMult == 0U)
        mode.scanMult = 1U;

    mode.rxFreq = reg0ToFrequency(mode.rxReg0, mode.reg1, true);
    mode.txFreq = reg0ToFrequency(mode.txReg0, mode.reg1, false);

    // Same band and banned frequency checks as SET_FREQ in MMDVM_HS
    uint8_t power = (uint8_t)(((mode.reg2 >> 13) & 0x3FU) << 2);
    if (io.setFreq(mode.rxFreq, mode.txFreq, power, mode.txFreq) != 0U)
        return OPENMT_ERR_BAD_DATA;

    int8_t index = findMode(connId);
    if (index < 0)
    {
        for (uint8_t i = 0U; i < OPENMT_MAX_MODES; i++)
        {
            if (!m_modes[i].used)
            {
                index = (int8_t)i;
                break;
            }
        }

        if (index < 0)
            return OPENMT_ERR_NO_SPACE;

        m_modeCount++;
    }

    m_modes[index] = mode;

#if OPENMT_DEBUG_EVENTS
    // Build stamp, so it is obvious which firmware is running
    serial.writeDebug("FW build " __DATE__ " " __TIME__
#if OPENMT_LOOPBACK_TEST
                      " LB=on"
#else
                      " LB=off"
#endif
                      , 0, 0);
#endif

    // The PA setting is the ADF7021's REG2 bits 13-18; 0 means no RF output
    OPENMT_EVENT("ADD conn,PA", connId, (mode.reg2 >> 13) & 0x3FU);
    if (((mode.reg2 >> 13) & 0x3FU) == 0U)
        OPENMT_EVENT("PA=0: no TX power, host rfPower", connId, 0);

    io.start();

    if (m_current < 0)
    {
        if (!txBusy())
            selectMode(index);
    }
    else if (index == m_current)
    {
        // The current mode was updated, reload it once TX allows
        if (txBusy())
        {
            m_reload = true;
        }
        else
        {
            stopStreaming();
            selectMode(index);
        }
    }

    return 0U;
}

uint8_t COpenMT::delMode(uint8_t connId)
{
    int8_t index = findMode(connId);
    if (index < 0)
        return OPENMT_ERR_BAD_CONN;

    if (index == m_current)
    {
        stopStreaming();
        m_hostLocked = false;
        m_reload     = false;
        m_current    = -1;

        // The ADF7021 keeps its settings, so a transmission in progress
        // still drains. processScan() loads another mode afterwards.
        io.setMode(STATE_IDLE);
    }

    m_modes[index].used = false;
    m_modeCount--;

    return 0U;
}

uint8_t COpenMT::setMode(uint8_t connId)
{
    if (connId == OPENMT_MODE_IDLE)
    {
        m_hostLocked = false;
        stopStreaming();
        restartDwell();
        setModeLEDs();
        return 0U;
    }

    int8_t index = findMode(connId);
    if (index < 0)
        return OPENMT_ERR_BAD_CONN;

    if (index != m_current)
    {
        if (txBusy())
            return OPENMT_ERR_BUSY;

        stopStreaming();
        selectMode(index);
    }

    m_hostLocked = true;

    if (!m_streaming)
        startStreaming(false);

    return 0U;
}

uint8_t COpenMT::writeBits(uint8_t connId, const uint8_t* data, uint8_t length)
{
    if (length == 0U)
        return OPENMT_ERR_BAD_DATA;

    int8_t index = findMode(connId);
    if (index < 0)
        return OPENMT_ERR_BAD_CONN;

    if (index != m_current)
    {
        // Never retune while bits for another mode are still going out
        if (txBusy())
            return OPENMT_ERR_BUSY;

        stopStreaming();
        selectMode(index);
    }

    // Strictly greater: the buffer must never become exactly full
    if (io.getSpace() <= (uint16_t)(length * 8U))
        return OPENMT_ERR_NO_SPACE;

    io.writeTXBits(data, length, m_modes[index].txMsbFirst, m_txInvert);

    if (!m_txWaiting && !m_tx)
    {
        m_txWaiting   = true;
        m_txWaitStart = io.getMillis();
    }

    return 0U;
}

uint8_t COpenMT::getSpace() const
{
    uint16_t frames = io.getSpace() / OPENMT_BIT_FRAME_BITS;

    if (frames > OPENMT_TX_SPACE_MARGIN)
        frames -= OPENMT_TX_SPACE_MARGIN;
    else
        frames = 0U;

    if (frames > 0x0FU)
        frames = 0x0FU;

    return (uint8_t)frames;
}

void COpenMT::process()
{
    processRX();
    processTX();
    processScan();
    updateScanLEDs();
    processStatus();
}

int8_t COpenMT::findMode(uint8_t connId) const
{
    for (uint8_t i = 0U; i < OPENMT_MAX_MODES; i++)
    {
        if (m_modes[i].used && m_modes[i].connId == connId)
            return (int8_t)i;
    }

    return -1;
}

bool COpenMT::txBusy() const
{
    return m_tx || io.getTXData() > 0U;
}

// Receive for ms milliseconds without streaming and count the sync patterns of
// the mode that are seen.
uint16_t COpenMT::listen(const OpenMTMode& mode, uint32_t ms)
{
    uint32_t start = io.getMillis();
    uint64_t shift = 0ULL;
    uint16_t syncs = 0U;
    uint8_t  bit;

    while ((io.getMillis() - start) < ms)
    {
        while (io.getRXBit(bit))
        {
            if (m_rxInvert)
                bit ^= 0x01U;

            shift = (shift << 1) | bit;
            if (mode.syncMask != 0ULL && syncMatch(shift, mode) && syncs < 0xFFFFU)
                syncs++;
        }
    }

    return syncs;
}

// Measure the receive frequency error of a registered mode against whatever
// is being received on its frequency (a reference signal). Blocks for about
// 2 * OPENMT_MEAS_SETTLE_MS plus the two mode loads, so it is refused while a
// mode is locked by the host or the modem transmits. The mode is loaded again
// as it was afterwards.
uint8_t COpenMT::measure(uint8_t connId, OpenMTMeas& result)
{
    int8_t index = findMode(connId);
    if (index < 0)
        return OPENMT_ERR_BAD_CONN;

    // Any mode can be measured, also while the host has a mode locked (the reference may be a radio
    // sending that mode's frames): the stream stops for the measurement, and afterwards the modem goes
    // back to the mode it was on, locked as before.
    bool   wasLocked = m_hostLocked;
    int8_t previous  = m_current;

    if (m_tx)
        return OPENMT_ERR_BUSY;

    if (io.getTXData() > 0U)
        return OPENMT_ERR_TX_DATA;

    stopStreaming();

    // The internal AFC must be off: it pulls the signal back to the IF and hides the error. With it off
    // the AFC readback is the intermediate frequency itself, 100 kHz when the receiver is on frequency,
    // frequency [Hz] = word * DEMOD_CLK / 2^18 with DEMOD_CLK = XTAL / DEMOD_CLK_DIVIDE (register 3, bits 6-9).
    OpenMTMode mode = m_modes[index];
    mode.reg10 &= ~0x10U;

    m_current = index;
    io.ifConfRegs(mode, true);
    io.clearRX();
    result.syncs = listen(mode, OPENMT_MEAS_SETTLE_MS);
    result.afc0  = io.readAFC();
    result.rssi  = io.readRSSI();

    // A second reading later, the host averages the two
    result.syncs += listen(mode, OPENMT_MEAS_SETTLE_MS);
    result.afc1   = io.readAFC();

    uint32_t div = (mode.reg3 >> 6) & 0x0FU;
    if (div == 0U)
        div = 1U;

    // Hz per readback count in 1/64 Hz
    result.step  = (int16_t)((((uint32_t)ADF7021_XTAL * 64UL) / div) >> 18);
    result.flags = 0U;

    selectMode((wasLocked && previous >= 0) ? previous : index);
    if (wasLocked && !m_streaming)
        startStreaming(false);

    return 0U;
}

// Diagnostic for bringing up the AFC readback: reads eight of the sixteen 16 bit
// words that register 7 can select (bits 7:4 stepped, readback enabled), so the
// host can see which word follows the signal. pass 0: AFC on, 1: AFC off,
// 2: AFC on with the frequency moved by OPENMT_MEAS_STEP_COUNTS.
uint8_t COpenMT::probe(uint8_t connId, uint8_t pass, uint8_t half, uint16_t* words)
{
    int8_t index = findMode(connId);
    if (index < 0)
        return OPENMT_ERR_BAD_CONN;

    if ((pass > 2U && (pass < 16U || pass > 20U)) || half > 1U)
        return OPENMT_ERR_BAD_DATA;

    // Any mode can be measured, also while the host has a mode locked (the reference may be a radio
    // sending that mode's frames): the stream stops for the measurement, and afterwards the modem goes
    // back to the mode it was on, locked as before.
    bool   wasLocked = m_hostLocked;
    int8_t previous  = m_current;

    if (m_tx)
        return OPENMT_ERR_BUSY;

    if (io.getTXData() > 0U)
        return OPENMT_ERR_TX_DATA;

    stopStreaming();

    OpenMTMode mode = m_modes[index];
    mode.reg10 = (mode.reg10 & 0x00FFFFFFU) | ((uint32_t)OPENMT_MEAS_AFC_RANGE << 24);
    if (pass >= 16U)
    {
        // Sweep: the frequency moved by (pass - 18) * 32 synthesizer counts, AFC on when half is 1. Words: the
        // AFC readback after 60, 120, 180 and 240 ms (does it settle?), then RSSI, filter calibration, revision.
        int32_t shift = ((int32_t)pass - 18) * 32;

        if (half != 0U)
            mode.reg10 |= 0x10U;
        else
            mode.reg10 &= ~0x10U;
        mode.rxReg0 = (uint32_t)((int32_t)mode.rxReg0 + (shift << 4));

        m_current = index;
        io.ifConfRegs(mode, true);
        io.clearRX();

        for (uint8_t i = 0U; i < 4U; i++)
        {
            listen(mode, OPENMT_MEAS_SETTLE_MS);
            words[i] = io.readback(0x0107UL);
        }
        words[4U] = io.readback(0x0147UL);
        words[5U] = io.readback(0x0187UL);
        words[6U] = io.readback(0x01C7UL);
        words[7U] = 0U;

        selectMode((wasLocked && previous >= 0) ? previous : index);
        if (wasLocked && !m_streaming)
            startStreaming(false);
        return 0U;
    }

    if (pass != 1U)
        mode.reg10 |= 0x10U;
    else
        mode.reg10 &= ~0x10U;

    if (pass == 2U)
        mode.rxReg0 += ((uint32_t)OPENMT_MEAS_STEP_COUNTS << 4);

    m_current = index;
    io.ifConfRegs(mode, true);
    io.clearRX();
    listen(mode, OPENMT_MEAS_SETTLE_MS);

    for (uint8_t i = 0U; i < 8U; i++)
        words[i] = io.readback(0x0107UL | ((uint32_t)(half * 8U + i) << 4));

    selectMode((wasLocked && previous >= 0) ? previous : index);
    if (wasLocked && !m_streaming)
        startStreaming(false);

    return 0U;
}

void COpenMT::restartDwell()
{
    m_scanStart   = io.getTicks();
    m_scanStartMs = io.getMillis();
    m_rssiChecked = false;
}

void COpenMT::selectMode(int8_t index)
{
    m_current = index;
    m_reload  = false;

    io.ifConfRegs(m_modes[index], true);

    // Bits demodulated with the previous settings are of no use now
    io.clearRX();
    m_rxShift = 0ULL;
    m_rxHist0 = 0ULL;
    m_rxHist1 = 0ULL;

    restartDwell();
    setModeLEDs();
}

void COpenMT::processRX()
{
    uint32_t now = io.getMillis();
    uint8_t bit;

    // Bounded so a backlog cannot starve the serial port
    for (uint16_t n = 0U; n < 512U && io.getRXBit(bit); n++)
    {
        if (m_current < 0)
            continue;

        if (m_rxInvert)
            bit ^= 0x01U;

        m_rxHist1 = (m_rxHist1 << 1) | (m_rxHist0 >> 63);
        m_rxHist0 = (m_rxHist0 << 1) | (m_rxShift >> 63);
        m_rxShift = (m_rxShift << 1) | bit;

        const OpenMTMode& mode = m_modes[m_current];
        bool sync = (mode.syncMask != 0ULL) && syncMatch(m_rxShift, mode);

        if (sync && m_tx)
            m_txSyncs++;

#if OPENMT_DEBUG_EVENTS
        if (!m_streaming && !sync && !m_tx && mode.syncMask != 0ULL)
        {
            uint8_t e = syncErrorCount(m_rxShift, mode);
            if (e <= (uint8_t)(mode.syncErrs + 3U))
            {
                m_nearMiss++;
                if ((now - m_nearLast) >= 1000U)
                {
                    m_nearLast = now;
                    OPENMT_EVENT("RX near-miss sync, errs,count", e, m_nearMiss);
                }
            }
        }
#endif

#if OPENMT_LOOPBACK_TEST && defined(DUPLEX)
        if (m_lbActive)
        {
            m_lbBits++;
            if (loopbackSync(m_rxShift))
            {
                uint32_t delta = m_lbBits - m_lbLast;
                if (m_lbLast != 0U)
                {
                    if (delta == 288U)
                        m_lbGood++;
                    else if (delta > 8U)
                        m_lbBadDelta = (uint16_t)delta;
                }
                m_lbLast = m_lbBits;
                m_lbSyncs++;
            }
        }
#endif

        if (m_streaming)
        {
            putRXBit(bit);
            if (sync)
            {
#if OPENMT_LOCK_LEARN
                if (m_hostLocked && m_syncSeen)
                {
                    uint32_t gap = now - m_lockTime;
                    OpenMTMode& cur = m_modes[m_current];

                    if (gap < OPENMT_LOCK_TIMEOUT_MS && gap > cur.gapMaxMs)
                    {
                        uint32_t t = (gap * OPENMT_LOCK_FACTOR_PERCENT) / 100U;

                        cur.gapMaxMs = (uint16_t)gap;
                        if (t < OPENMT_LOCK_MIN_MS)
                            t = OPENMT_LOCK_MIN_MS;
                        if (t > OPENMT_LOCK_TIMEOUT_MS)
                            t = OPENMT_LOCK_TIMEOUT_MS;
                        cur.lockMs = (uint16_t)t;
                        OPENMT_EVENT("lock timeout learned, conn,ms", cur.connId, cur.lockMs);
                    }
                }
#endif
                m_lockTime = now;
                m_syncSeen = true;
            }
        }
        else if (sync)
        {
            // The history already holds this bit
            startStreaming(true);
        }
    }

    if (m_streaming && !m_hostLocked && m_current >= 0 && (now - m_lockTime) >= m_modes[m_current].lockMs)
        stopStreaming();
}

void COpenMT::processTX()
{
    uint32_t now = io.getMillis();

    uint8_t hang = io.takeHangFlags();
    if (hang != 0U)
        OPENMT_EVENT("HANG GUARD tripped, 1=TX start 2=TX end", hang, 0);

    if (m_tx)
    {
        if (!m_txWasOn)
        {
            m_txWasOn   = true;
            m_txOnStart = now;
            m_txSyncs   = 0U;

        }

        m_txWaiting = false;
        return;
    }

    if (m_txWasOn)
    {
        m_txWasOn = false;
        OPENMT_EVENT("TX end, ms on air", clampMs(now - m_txOnStart), 0);
        OPENMT_EVENT("RX syncs seen while TX, streaming", m_txSyncs, m_streaming ? 1 : 0);
#if OPENMT_LOOPBACK_TEST && defined(DUPLEX)
        if (m_lbActive)
        {
            m_lbActive = false;
            if (m_current >= 0)
                io.loopbackTune(m_modes[m_current].rxReg0);
            OPENMT_EVENT("LB BS syncs,at 288 bits", m_lbSyncs, m_lbGood);
            OPENMT_EVENT("LB last odd gap,kbits", m_lbBadDelta, clampMs(m_lbBits / 1000U));
        }
#endif
    }

    uint16_t data = io.getTXData();
    if (data == 0U)
    {
        m_txWaiting = false;
#if OPENMT_LOOPBACK_TEST && defined(DUPLEX)
        if (m_lbActive && !m_txWasOn)
        {
            m_lbActive = false;     // armed but never keyed: undo the retune
            if (m_current >= 0)
                io.loopbackTune(m_modes[m_current].rxReg0);
        }
#endif
        return;
    }

    if (!m_txWaiting)
    {
        m_txWaiting   = true;
        m_txWaitStart = now;

#if OPENMT_LOOPBACK_TEST && defined(DUPLEX)
        // Retune the second chip now, well before the transmitter keys: the
        // two chips share SCLK/SDATA, so a write to chip 2 between the TX
        // REG0 load and the TX start pulse would corrupt chip 1's register
        if (m_duplex && m_current >= 0 && !m_lbActive)
        {
            m_lbBits = m_lbLast = 0U;
            m_lbSyncs = m_lbGood = m_lbBadDelta = 0U;
            io.loopbackTune(loopbackReg0(m_modes[m_current]));
            m_lbActive = true;
        }
#endif
    }

    // Hold off keying until enough is buffered to ride out the gap before
    // the host's next frame, or until the bits have waited long enough
    // (a short transmission that will never reach the threshold)
    if (data >= OPENMT_TX_START_BITS || (now - m_txWaitStart) >= OPENMT_TX_START_TIMEOUT_MS)
    {
        OPENMT_EVENT("TX key, bits,waited ms", data, clampMs(now - m_txWaitStart));
        io.keyTX();
        m_txWaiting = false;
    }
}

void COpenMT::processScan()
{
    if (txBusy())
        return;

    if (m_reload && m_current >= 0)
    {
        stopStreaming();
        selectMode(m_current);
        return;
    }

    if (m_current < 0)
    {
        // Nothing loaded (start up, or the current mode was deleted)
        for (uint8_t i = 0U; i < OPENMT_MAX_MODES; i++)
        {
            if (m_modes[i].used)
            {
                selectMode((int8_t)i);
                break;
            }
        }
        return;
    }

    if (m_modeCount < 2U || m_streaming || m_hostLocked)
        return;

#if OPENMT_RSSI_SCAN
    if (!m_rssiChecked)
    {
        if ((io.getMillis() - m_scanStartMs) < OPENMT_RSSI_SETTLE_MS)
            return;

        m_rssiChecked = true;

        uint16_t rssi = io.readRSSI();
        if (rssi > OPENMT_RSSI_THRESHOLD)
        {
            // Empty channel: next mode now
            scanNext();
            return;
        }

        uint32_t t = io.getMillis();
        if ((t - m_rssiLast) >= 1000U)
        {
            m_rssiLast = t;
            OPENMT_EVENT("RSSI carrier, conn,-dBm", m_modes[m_current].connId, rssi);
        }
    }
#endif

    uint32_t dwell = OPENMT_SCAN_TIME * m_modes[m_current].scanMult;
    if ((io.getTicks() - m_scanStart) < dwell)
        return;

    scanNext();
}

// Load the next registered mode
void COpenMT::scanNext()
{
    int8_t next = m_current;
    do
    {
        next = (int8_t)((next + 1) % (int8_t)OPENMT_MAX_MODES);
    } while (!m_modes[next].used);

    if (next != m_current)
        selectMode(next);
    else
        restartDwell();
}

// Only changes to/from "no space" are announced: that is all Modem_Host
// acts on, and it logs every status frame it receives.
void COpenMT::processStatus()
{
    uint8_t space = getSpace();
    bool zero = (space == 0U);

    if (zero != m_lastSpaceZero)
    {
        serial.writeStatus(space);
        m_lastSpaceZero = zero;
    }
}

void COpenMT::startStreaming(bool withHistory)
{
    m_streaming = true;
    m_rxBits    = 0U;
    m_lockTime  = io.getMillis();
    m_syncSeen  = withHistory;      // a stream that starts at a sync begins with one

    if (withHistory)
    {
        // Oldest bit first, so the sync pattern ends the history
        uint8_t bytes = m_modes[m_current].historyBytes;
        uint8_t first = 0U;

        if (bytes > 16U)
        {
            for (uint8_t i = 0U; i < 8U; i++)
                m_rxFrame[first++] = (uint8_t)(m_rxHist1 >> (56U - (8U * i)));
        }

        if (bytes > 8U)
        {
            for (uint8_t i = 0U; i < 8U; i++)
                m_rxFrame[first++] = (uint8_t)(m_rxHist0 >> (56U - (8U * i)));
        }

        for (uint8_t i = 0U; i < 8U; i++)
            m_rxFrame[first++] = (uint8_t)(m_rxShift >> (56U - (8U * i)));

        m_rxBits = (uint16_t)first * 8U;

        // The history is whole bytes, so the stream stays bit-contiguous
#if OPENMT_SYNC_FLUSH
        flushRX();
#else
        if (m_rxBits >= (OPENMT_RX_FRAME_BYTES * 8U))
            flushRX();
#endif
    }

    io.setDecode(true);
    setModeLEDs();

    if (m_current >= 0)
        OPENMT_EVENT("RX stream on, conn,hostlock", m_modes[m_current].connId, withHistory ? 0 : 1);
}

void COpenMT::stopStreaming()
{
    if (!m_streaming)
        return;

    if (m_current >= 0)
        OPENMT_EVENT("RX stream off, conn", m_modes[m_current].connId, 0);

    flushRX();

    m_streaming = false;
    restartDwell();

    io.setDecode(false);
    setModeLEDs();
}

void COpenMT::putRXBit(uint8_t bit)
{
    uint8_t mask = 0x80U >> (m_rxBits & 0x07U);
    uint16_t pos = m_rxBits >> 3;

    if (bit)
        m_rxFrame[pos] |= mask;
    else
        m_rxFrame[pos] &= (uint8_t)~mask;

    m_rxBits++;

    if (m_rxBits >= (OPENMT_RX_FRAME_BYTES * 8U))
        flushRX();
}

// Send whatever has been collected, the last byte padded with zero bits
void COpenMT::flushRX()
{
    if (m_rxBits == 0U || m_current < 0)
    {
        m_rxBits = 0U;
        return;
    }

    uint8_t bytes = (uint8_t)((m_rxBits + 7U) / 8U);
    uint8_t rem = m_rxBits & 0x07U;

    if (rem != 0U)
        m_rxFrame[bytes - 1U] &= (uint8_t)(0xFFU << (8U - rem));

    if (m_modes[m_current].rxTrailer)
    {
        // Where the transmitter is in its 60 ms (576 bit) cycle, so the host
        // can tell which time slot a received burst belongs to
        uint16_t phase = io.getTXPhase(576U);
        m_rxFrame[bytes++] = (uint8_t)(phase >> 8);
        m_rxFrame[bytes++] = (uint8_t)(phase & 0xFFU);
    }

    serial.writeBitData(m_modes[m_current].connId, m_rxFrame, (uint8_t)bytes);

    m_rxBits = 0U;
}

// True while the modem is only scanning: two or more modes, none streaming or locked by the host.
bool COpenMT::isScanning() const
{
    return m_current >= 0 && m_modeCount >= 2U && !m_streaming && !m_hostLocked;
}

// Scan indicator: the D-Star and DMR LEDs blink alternately, whichever mode is being listened to.
void COpenMT::updateScanLEDs()
{
    if (!isScanning())
        return;

    uint32_t now = io.getMillis();
    if ((now - m_scanLedMs) < OPENMT_SCAN_LED_MS)
        return;

    m_scanLedMs    = now;
    m_scanLedPhase = !m_scanLedPhase;
    io.setMode(m_scanLedPhase ? STATE_DSTAR : STATE_DMR);
}

// The modem has no idea which protocol a connection carries. While scanning, two LEDs blink
// alternately (updateScanLEDs). Otherwise the mode LEDs follow the modulation of the mode in
// use: GMSK -> D-Star, 2FSK -> POCSAG, 3/4FSK -> DMR.
void COpenMT::setModeLEDs()
{
    MMDVM_STATE state = STATE_IDLE;

    if (isScanning())
    {
        io.setMode(m_scanLedPhase ? STATE_DSTAR : STATE_DMR);
        return;
    }

    if (m_current >= 0)
    {
#if defined(QUIET_MODE_LEDS)
        if (m_streaming || m_hostLocked || m_modeCount < 2U)
#endif
        {
            switch ((m_modes[m_current].reg4 >> 4) & 0x07U)
            {
                case 0x01U:
                    state = STATE_DSTAR;
                    break;
                case 0x00U:
                    state = STATE_POCSAG;
                    break;
                default:
                    state = STATE_DMR;
                    break;
            }
        }
    }

    io.setMode(state);
}
