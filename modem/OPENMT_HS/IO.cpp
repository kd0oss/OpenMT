/*
 *   Copyright (C) 2015,2016,2020 by Jonathan Naylor G4KLX
 *   Copyright (C) 2016,2017,2018,2019,2020 by Andy Uribe CA6JAU
 *   Copyright (C) 2017 by Danilo DB4PLE
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

#include "Config.h"
#include "Globals.h"
#include "IO.h"

// Modem state shared with ADF7021.cpp, OpenMT.cpp and BitRB.cpp
MMDVM_STATE m_modemState = STATE_IDLE;

uint32_t m_modeTimerCnt;

bool m_duplex = false;
bool m_tx = false;
bool m_dcd = false;

uint8_t m_control;

uint32_t m_frequency_rx;
uint32_t m_frequency_tx;
uint32_t m_pocsag_freq_tx;
uint8_t m_power;

CIO::CIO()
    : m_started(false), m_rxBuffer(1024U),
      m_txBuffer((OPENMT_TX_BUFFER_FRAMES * OPENMT_BIT_FRAME_BITS) + OPENMT_TX_BUFFER_SLACK),
      m_ledCount(0U), m_ledValue(true), m_ticks(0U), m_hangFlags(0U), m_txBitsPut(0U) {
  Init();

  CE_pin(HIGH);
  LED_pin(HIGH);
  PTT_pin(LOW);
  DSTAR_pin(LOW);
  DMR_pin(LOW);
  YSF_pin(LOW);
  P25_pin(LOW);
  NXDN_pin(LOW);
  M17_pin(LOW);
  POCSAG_pin(LOW);
  COS_pin(LOW);
  DEB_pin(LOW);

#if !defined(BIDIR_DATA_PIN)
  TXD_pin(LOW);
#endif

  SCLK_pin(LOW);
  SDATA_pin(LOW);
  SLE_pin(LOW);

  selfTest();

  m_modeTimerCnt = 0U;
}

void CIO::selfTest() {
  bool ledValue = false;
  uint32_t ledCount = 0U;
  uint32_t blinks = 0U;

  while (true) {
    ledCount++;
    delay_us(1000U);

    if (ledCount >= 125U) {
      ledCount = 0U;
      ledValue = !ledValue;

      LED_pin(!ledValue);
      PTT_pin(ledValue);
      DSTAR_pin(ledValue);
      DMR_pin(ledValue);
      YSF_pin(ledValue);
      P25_pin(ledValue);
      NXDN_pin(ledValue);
      M17_pin(ledValue);
      POCSAG_pin(ledValue);
      COS_pin(ledValue);

      blinks++;

      if (blinks > 5U)
        break;
    }
  }
}

void CIO::process()
{
    m_ledCount++;

    if (m_started)
    {
#if defined(CONSTANT_SRV_LED)
        LED_pin(HIGH);
#elif defined(CONSTANT_SRV_LED_INVERTED)
        LED_pin(LOW);
#elif defined(DISCREET_SRV_LED)
        if (m_ledCount == 10000U)
            LED_pin(LOW);
        if (m_ledCount >= 480000U)
        {
            m_ledCount = 0U;
            LED_pin(HIGH);
        }
#elif defined(DISCREET_SRV_LED_INVERTED)
        if (m_ledCount == 10000U)
            LED_pin(HIGH);
        if (m_ledCount >= 480000U)
        {
            m_ledCount = 0U;
            LED_pin(LOW);
        }
#else
        if (m_ledCount >= 24000U)
        {
            m_ledCount = 0U;
            m_ledValue = !m_ledValue;
            LED_pin(m_ledValue);
        }
#endif
    }
    else
    {
        if (m_ledCount >= 240000U)
        {
            m_ledCount = 0U;
            m_ledValue = !m_ledValue;
            LED_pin(m_ledValue);
        }
        return;
    }

    // Switch off the transmitter once every buffered bit has been sent
    if (m_txBuffer.getData() == 0U && m_tx)
        setRX(false);
}

// Mode handling and scanning live in COpenMT; this only enables the
// ADF7021 clock interrupt(s) once.
void CIO::start()
{
    if (m_started)
        return;

    startInt();

    m_started = true;
}

bool CIO::getRXBit(uint8_t& bit)
{
    uint8_t control;

    return m_rxBuffer.get(bit, control);
}

void CIO::clearRX()
{
    uint8_t bit, control;

    while (m_rxBuffer.get(bit, control))
        ;
}

// Queue bits for transmission without keying the transmitter; COpenMT
// decides when enough is buffered and calls keyTX().
void CIO::writeTXBits(const uint8_t* data, uint16_t length, bool msbFirst, bool invert)
{
    m_txBitsPut += (uint32_t)length * 8U;

    for (uint16_t i = 0U; i < length; i++)
    {
        uint8_t c = data[i];

        for (uint8_t j = 0U; j < 8U; j++)
        {
            uint8_t bit;

            if (msbFirst)
                bit = (c >> (7U - j)) & 0x01U;
            else
                bit = (c >> j) & 0x01U;

            if (invert)
                bit ^= 0x01U;

            m_txBuffer.put(bit, MARK_NONE);
        }
    }
}

uint16_t CIO::getTXData() const
{
    return m_txBuffer.getData();
}

// Position in the transmitted bit stream, modulo 'period'. The count is
// bits queued minus bits still waiting, so it is 0 at the first bit on air.
// While not transmitting the baseline is reset to whatever is queued.
uint16_t CIO::getTXPhase(uint16_t period)
{
    uint16_t waiting = m_txBuffer.getData();

    if (!m_tx)
    {
        m_txBitsPut = waiting;
        return 0xFFFFU;
    }

    uint32_t sent = m_txBitsPut - (uint32_t)waiting;
    return (uint16_t)(sent % period);
}

void CIO::keyTX()
{
    if (!m_started || m_tx)
        return;

    setTX();
    m_tx = true;
}

uint32_t CIO::getTicks() const
{
    return m_ticks;
}

uint16_t CIO::getSpace() const { return m_txBuffer.getSpace(); }

#if defined(ZUMSPOT_ADF7021) || defined(LONESTAR_USB) || defined(SKYBRIDGE_HS)
void CIO::checkBand(uint32_t frequency_rx, uint32_t frequency_tx) {
  if (!(io.hasSingleADF7021())) {
    // There are two ADF7021s on the board
    if (io.isDualBand()) {
      // Dual band
      if ((frequency_tx <= VHF2_MAX) && (frequency_rx <= VHF2_MAX)) {
        // Turn on VHF side
        io.setBandVHF(true);
      } else if ((frequency_tx >= UHF1_MIN) && (frequency_rx >= UHF1_MIN)) {
        // Turn on UHF side
        io.setBandVHF(false);
      }
    }
  }
}

uint8_t CIO::checkZUMspot(uint32_t frequency_rx, uint32_t frequency_tx) {
  if (!(io.hasSingleADF7021())) {
    // There are two ADF7021s on the board
    if (io.isDualBand()) {
      // Dual band
      if ((frequency_tx <= VHF2_MAX) && (frequency_rx <= VHF2_MAX)) {
        // Turn on VHF side
        io.setBandVHF(true);
      } else if ((frequency_tx >= UHF1_MIN) && (frequency_rx >= UHF1_MIN)) {
        // Turn on UHF side
        io.setBandVHF(false);
      }
    } else if (!io.isDualBand()) {
      // Duplex board
      if ((frequency_tx < UHF1_MIN) || (frequency_rx < UHF1_MIN)) {
        // Reject VHF frequencies
        return 4U;
      }
    }
  }
  return 0U;
}
#endif

uint8_t CIO::setFreq(uint32_t frequency_rx, uint32_t frequency_tx,
                     uint8_t rf_power, uint32_t pocsag_freq_tx) {
  // Configure power level
  setPower(rf_power);

#if !defined(DISABLE_FREQ_CHECK)
  // Check frequency ranges
  if (!(((frequency_rx >= VHF1_MIN) && (frequency_rx < VHF1_MAX)) ||
        ((frequency_tx >= VHF1_MIN) && (frequency_tx < VHF1_MAX)) ||
        ((frequency_rx >= UHF1_MIN) && (frequency_rx < UHF1_MAX)) ||
        ((frequency_tx >= UHF1_MIN) && (frequency_tx < UHF1_MAX)) ||
        ((frequency_rx >= VHF2_MIN) && (frequency_rx < VHF2_MAX)) ||
        ((frequency_tx >= VHF2_MIN) && (frequency_tx < VHF2_MAX)) ||
        ((frequency_rx >= UHF2_MIN) && (frequency_rx < UHF2_MAX)) ||
        ((frequency_tx >= UHF2_MIN) && (frequency_tx < UHF2_MAX))))
    return 4U;

  if (!(((pocsag_freq_tx >= VHF1_MIN) && (pocsag_freq_tx < VHF1_MAX)) ||
        ((pocsag_freq_tx >= UHF1_MIN) && (pocsag_freq_tx < UHF1_MAX)) ||
        ((pocsag_freq_tx >= VHF2_MIN) && (pocsag_freq_tx < VHF2_MAX)) ||
        ((pocsag_freq_tx >= UHF2_MIN) && (pocsag_freq_tx < UHF2_MAX))))
    return 4U;
#endif

#if !defined(DISABLE_FREQ_BAN)
  // Check banned frequency ranges
  if (((frequency_rx >= BAN1_MIN) && (frequency_rx <= BAN1_MAX)) ||
      ((frequency_tx >= BAN1_MIN) && (frequency_tx <= BAN1_MAX)) ||
      ((frequency_rx >= BAN2_MIN) && (frequency_rx <= BAN2_MAX)) ||
      ((frequency_tx >= BAN2_MIN) && (frequency_tx <= BAN2_MAX)))
    return 4U;

  if (((pocsag_freq_tx >= BAN1_MIN) && (pocsag_freq_tx <= BAN1_MAX)) ||
      ((pocsag_freq_tx >= BAN2_MIN) && (pocsag_freq_tx <= BAN2_MAX)))
    return 4U;
#endif

// Check if we have a single, dualband or duplex board
#if defined(ZUMSPOT_ADF7021) || defined(LONESTAR_USB) || defined(SKYBRIDGE_HS)
  if (checkZUMspot(frequency_rx, frequency_tx) > 0) {
    return 4U;
  }
#endif

  // Configure frequency
  m_frequency_rx = frequency_rx;
  m_frequency_tx = frequency_tx;
  m_pocsag_freq_tx = pocsag_freq_tx;

  return 0U;
}

void CIO::setMode(MMDVM_STATE modemState) {
#if defined(USE_ALTERNATE_POCSAG_LEDS)
  if (modemState != STATE_POCSAG) {
#endif
    DSTAR_pin(modemState == STATE_DSTAR);
    DMR_pin(modemState == STATE_DMR);
#if defined(USE_ALTERNATE_POCSAG_LEDS)
  }
#endif
#if defined(USE_ALTERNATE_NXDN_LEDS)
  if (modemState != STATE_NXDN) {
#endif
    YSF_pin(modemState == STATE_YSF);
    P25_pin(modemState == STATE_P25);
#if defined(USE_ALTERNATE_NXDN_LEDS)
  }
#endif
#if defined(USE_ALTERNATE_M17_LEDS)
  if (modemState != STATE_M17) {
#endif
    YSF_pin(modemState == STATE_YSF);
    P25_pin(modemState == STATE_P25);
#if defined(USE_ALTERNATE_M17_LEDS)
  }
#endif
#if defined(USE_ALTERNATE_NXDN_LEDS)
  if (modemState != STATE_YSF && modemState != STATE_P25) {
#endif
    NXDN_pin(modemState == STATE_NXDN);
#if defined(USE_ALTERNATE_NXDN_LEDS)
  }
#endif
#if defined(USE_ALTERNATE_POCSAG_LEDS)
  if (modemState != STATE_DSTAR && modemState != STATE_DMR) {
#endif
    POCSAG_pin(modemState == STATE_POCSAG);
#if defined(USE_ALTERNATE_POCSAG_LEDS)
  }
#endif
#if defined(USE_ALTERNATE_M17_LEDS)
  if (modemState != STATE_DSTAR && modemState != STATE_P25) {
#endif
    M17_pin(modemState == STATE_M17);
#if defined(USE_ALTERNATE_M17_LEDS)
  }
#endif
}

void CIO::setDecode(bool dcd)
{
    if (dcd != m_dcd)
        COS_pin(dcd ? true : false);

    m_dcd = dcd;
}

// Global objects (definition order is the construction order)
CSerialPort serial;
CIO io;
COpenMT openmt;

#if defined(STM32_I2C_HOST)
CI2CHost i2c;
#endif

#if defined(STM32F10X_MD) || defined(STM32F4XX) || defined(STM32F7XX)
void setup()
{
    serial.start();
}

void loop()
{
    io.process();
    serial.process();
    openmt.process();
}

int main()
{
    setup();

    for (;;)
        loop();
}
#endif
