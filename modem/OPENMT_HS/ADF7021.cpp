/*
 *   Copyright (C) 2020,2021 by Jonathan Naylor G4KLX
 *   Copyright (C) 2016 by Jim McLaughlin KI6ZUM
 *   Copyright (C) 2016,2017,2018,2019,2020 by Andy Uribe CA6JAU
 *   Copyright (C) 2017 by Danilo DB4PLE
 *
 *   Some of the code is based on work of Guus Van Dooren PE1PLM:
 *   https://github.com/ki6zum/gmsk-dstar/blob/master/firmware/dvmega/dvmega.ino
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

#if defined(ENABLE_ADF7021)

#include "Globals.h"
#include "IO.h"
#include "ADF7021.h"
#include <math.h>

volatile bool totx_request = false;
volatile bool torx_request = false;
volatile bool even = true;
static uint32_t last_clk = 2U;

volatile uint32_t  AD7021_control_word;

uint32_t           ADF7021_RX_REG0;
uint32_t           ADF7021_TX_REG0;
uint32_t           ADF7021_REG1;


static void Send_AD7021_control_shift()
{
  for (int AD7021_counter = 31; AD7021_counter >= 0; AD7021_counter--) {
    if (bitRead(AD7021_control_word, AD7021_counter) == HIGH)
      io.SDATA_pin(HIGH);
    else
      io.SDATA_pin(LOW);

    io.dlybit();
    io.SCLK_pin(HIGH);
    io.dlybit();
    io.SCLK_pin(LOW);
  }

  // to keep SDATA signal at defined level when idle (not required)
  io.SDATA_pin(LOW);
}

static void Send_AD7021_control_slePulse()
{
  io.SLE_pin(HIGH);
  io.dlybit();
  io.SLE_pin(LOW);
}

void Send_AD7021_control(bool doSle)
{
  Send_AD7021_control_shift();

  if (doSle)
    Send_AD7021_control_slePulse();
}

#if defined(DUPLEX)
static  void Send_AD7021_control_sle2Pulse()
{
  io.SLE2_pin(HIGH);
  io.dlybit();
  io.SLE2_pin(LOW);
}

void Send_AD7021_control2(bool doSle)
{
  Send_AD7021_control_shift();

  if (doSle)
    Send_AD7021_control_sle2Pulse();
}
#endif

// AGC register (9), selected at compile time
static uint32_t agcControlWord()
{
#if defined(AD7021_GAIN_AUTO)
    return 0x000231E9U;   // AGC ON, normal operation
#elif defined(AD7021_GAIN_AUTO_LIN)
    return 0x100231E9U;   // AGC ON, LNA high linearity
#elif defined(AD7021_GAIN_LOW)
    return 0x120631E9U;   // AGC OFF, low gain, LNA high linearity
#elif defined(AD7021_GAIN_HIGH)
    return 0x00A631E9U;   // AGC OFF, high gain
#else
    return 0x000231E9U;
#endif
}

// Read the 16 bit readback word selected by register 7 (reg7 holds the whole
// control word: readback enable, readback mode and ADC mode). Reads from the
// chip that receives (the second one when duplex).
uint16_t CIO::readback(uint32_t reg7)
{
  uint16_t RB_word = 0U;

  // Send control register
  for (int AD7021_counter = 8; AD7021_counter >= 0; AD7021_counter--) {
    if (bitRead(reg7, AD7021_counter) == HIGH)
      SDATA_pin(HIGH);
    else
      SDATA_pin(LOW);

    dlybit();
    SCLK_pin(HIGH);
    dlybit();
    SCLK_pin(LOW);
  }

  SDATA_pin(LOW);

#if defined(DUPLEX)
  if (m_duplex)
    SLE2_pin(HIGH);
  else
    SLE_pin(HIGH);
#else
  SLE_pin(HIGH);
#endif

  dlybit();

  // Read SREAD pin
  for (int AD7021_counter = 17; AD7021_counter >= 0; AD7021_counter--) {
    SCLK_pin(HIGH);
    dlybit();

    if ((AD7021_counter != 17) && (AD7021_counter != 0))
      RB_word |= ((SREAD_pin() & 0x01) << (AD7021_counter - 1));

    SCLK_pin(LOW);
    dlybit();
  }

#if defined(DUPLEX)
  if (m_duplex)
    SLE2_pin(LOW);
  else
    SLE_pin(LOW);
#else
  SLE_pin(LOW);
#endif

  return RB_word;
}

// Received signal strength of the loaded mode, as -dBm (a smaller number is
// a stronger signal). Register 7: readback enabled, ADC readback, RSSI mode.
// A failed readback gives a value that looks like a strong signal, so the
// scan then simply behaves as without it.
uint16_t CIO::readRSSI()
{
  uint16_t RB_word = readback(0x0147U);
  uint8_t RB_code, gain_code, gain_corr;

  // Process RSSI code
  RB_code = RB_word & 0x7f;
  gain_code = (RB_word >> 7) & 0x0f;

  switch(gain_code) {
    case 0b1010:
      gain_corr = 0U;
      break;
    case 0b0110:
      gain_corr = 24U;
      break;
    case 0b0101:
      gain_corr = 38U;
      break;
    case 0b0100:
      gain_corr = 58U;
      break;
    case 0b0000:
      gain_corr = 86U;
      break;
    default:
      gain_corr = 0U;
      break;
  }

  return ( 130 - (RB_code + gain_corr)/2 );
}

// AFC readback: the frequency error the AFC loop measured, a signed 16 bit
// value (register 7: readback enabled, readback mode AFC word). The scale is
// not needed, COpenMT::measure() takes two readings and the host works out
// the error from them.
int16_t CIO::readAFC()
{
  return (int16_t)readback(0x0107U);
}

// Load a mode supplied by an OpenMT protocol service (ADD_MODE). The
// registers arrive fully built (frequency, deviation, power, modulation),
// so they are written as-is, in the same order the ADF7021 datasheet requires.
void CIO::ifConfRegs(const OpenMTMode& mode, bool reset)
{
#if defined(ZUMSPOT_ADF7021) || defined(SKYBRIDGE_HS)
    io.checkBand(mode.rxFreq, mode.txFreq);
#endif

    // Toggle CE pin for ADF7021 reset
    if (reset)
    {
        CE_pin(LOW);
        delay_reset();
        CE_pin(HIGH);
        delay_reset();
    }

    ADF7021_RX_REG0 = mode.rxReg0;
    ADF7021_TX_REG0 = mode.txReg0;
    ADF7021_REG1    = mode.reg1;

    // VCO/OSCILLATOR (1)
    AD7021_control_word = mode.reg1;
    Send_AD7021_control();

    // TX/RX CLOCK (3)
    AD7021_control_word = mode.reg3;
    Send_AD7021_control();

    // DEMOD (4)
    AD7021_control_word = mode.reg4;
    Send_AD7021_control();

    // IF fine cal (6)
    AD7021_control_word = mode.reg6;
    Send_AD7021_control();

    // IF coarse cal (5)
    AD7021_control_word = mode.reg5;
    Send_AD7021_control();

    // Delay for filter calibration
    delay_IFcal();

    // Frequency RX (0)
    setRX();

    // MODULATION (2), deviation, power and PA already set by the host
    AD7021_control_word = mode.reg2;
    Send_AD7021_control();

    // TEST DAC (14)
    AD7021_control_word = 0x0000000EU;
    Send_AD7021_control();

    // AGC (9)
    AD7021_control_word = agcControlWord();
    Send_AD7021_control();

    // AFC (10)
    AD7021_control_word = mode.reg10;
    Send_AD7021_control();

    // SYNC WORD DET (11)
    AD7021_control_word = 0x0000003BU;
    Send_AD7021_control();

    // SWD/THRESHOLD (12)
    AD7021_control_word = 0x0000010CU;
    Send_AD7021_control();

    // 3FSK/4FSK DEMOD (13)
    AD7021_control_word = mode.reg13;
    Send_AD7021_control();

    // TEST MODE (disabled) (15)
    AD7021_control_word = 0x000E000FU;
    Send_AD7021_control();

#if defined(DUPLEX)
    if (m_duplex)
        ifConfRegs2(mode);
#endif
}

uint8_t CIO::takeHangFlags()
{
  uint8_t f = m_hangFlags;
  m_hangFlags = 0U;
  return f;
}

// Diagnostic: write REG0 of the second chip (OPENMT_LOOPBACK_TEST)
void CIO::loopbackTune(uint32_t reg0)
{
#if defined(DUPLEX)
  AD7021_control_word = reg0;
  Send_AD7021_control2();
#else
  (void)reg0;
#endif
}

#if defined(DUPLEX)
// Second ADF7021 (receive only in duplex mode)
void CIO::ifConfRegs2(const OpenMTMode& mode)
{
    AD7021_control_word = mode.reg1;
    Send_AD7021_control2();

    AD7021_control_word = mode.reg3;
    Send_AD7021_control2();

    AD7021_control_word = mode.reg4;
    Send_AD7021_control2();

    AD7021_control_word = mode.reg6;
    Send_AD7021_control2();

    AD7021_control_word = mode.reg5;
    Send_AD7021_control2();

    delay_IFcal();

    // Frequency RX (0) and set to RX only
    AD7021_control_word = mode.rxReg0;
    Send_AD7021_control2();

    AD7021_control_word = mode.reg2;
    Send_AD7021_control2();

    AD7021_control_word = 0x0000000EU;
    Send_AD7021_control2();

    AD7021_control_word = 0x000231E9U;
    Send_AD7021_control2();

    AD7021_control_word = mode.reg10;
    Send_AD7021_control2();

    AD7021_control_word = 0x0000003BU;
    Send_AD7021_control2();

    AD7021_control_word = 0x0000010CU;
    Send_AD7021_control2();

    AD7021_control_word = mode.reg13;
    Send_AD7021_control2();

    AD7021_control_word = 0x000E000FU;
    Send_AD7021_control2();
}
#endif

void CIO::interrupt()
{
  uint8_t bit = 0U;

  if (!m_started)
    return;

  uint8_t clk = CLK_pin();

  // this is to prevent activation by spurious interrupts
  // which seem to happen if you send out an control word
  // needs investigation
  // this workaround will fail if only rising or falling edge
  // is used to trigger the interrupt !!!!
  // TODO: figure out why sending the control word seems to issue interrupts
  // possibly this is a design problem of the RF7021 board or too long wires
  // on the breadboard build
  // but normally this will not hurt too much
  if (clk == last_clk)
    return;
  else
    last_clk = clk;

  // we set the TX bit at TXD low, sampling of ADF7021 happens at rising clock
  if (m_tx && clk == 0U) {
    m_txBuffer.get(bit, m_control);
    even = !even;

#if defined(BIDIR_DATA_PIN)
    if (bit)
      RXD_pin_write(HIGH);
    else
      RXD_pin_write(LOW);
#else
    if (bit)
      TXD_pin(HIGH);
    else
      TXD_pin(LOW);
#endif

    // wait a brief period before raising SLE
    if (totx_request == true) {
      asm volatile("nop          \n\t"
                   "nop          \n\t"
                   "nop          \n\t"
                   );

      // SLE Pulse, should be moved out of here into class method
      // according to datasheet in 4FSK we have to deliver this before 1/4 tbit == 26uS
      SLE_pin(HIGH);
      asm volatile("nop          \n\t"
                   "nop          \n\t"
                   "nop          \n\t"
                   );
      SLE_pin(LOW);
      SDATA_pin(LOW);

      // now do housekeeping
      totx_request = false;
      // first tranmittted bit is always the odd bit
      even = ADF7021_EVEN_BIT;
    }
  }

  // we sample the RX bit at rising TXD clock edge, so TXD must be 1 and we are not in tx mode
  if (!m_tx && clk == 1U && !m_duplex) {
    if (RXD_pin())
      bit = 1U;
    else
      bit = 0U;

    m_rxBuffer.put(bit, m_control);
  }

  if (torx_request && even == ADF7021_EVEN_BIT && m_tx && clk == 0U) {
      // that is absolutely crucial in 4FSK, see datasheet:
      // enable sle after 1/4 tBit == 26uS when sending MSB (even == false) and clock is low
      delay_us(26U);

      // SLE Pulse, should be moved out of here into class method
      SLE_pin(HIGH);
      asm volatile("nop          \n\t"
                   "nop          \n\t"
                   "nop          \n\t"
                   );
      SLE_pin(LOW);
      SDATA_pin(LOW);

      // now do housekeeping
      m_tx = false;
      torx_request = false;
      // last tranmittted bit is always the even bit
      // since the current bit is a transitional "don't care" bit, never transmitted
      even = !ADF7021_EVEN_BIT;
  }

  m_ticks++;
  m_modeTimerCnt++;
}

#if defined(DUPLEX)
void CIO::interrupt2()
{
  uint8_t bit = 0U;

  if (m_duplex) {
    if (RXD2_pin())
      bit = 1U;
    else
      bit = 0U;

    m_rxBuffer.put(bit, m_control);
  }

}
#endif

void CIO::setTX()
{
  // PTT pin on (doing it earlier helps to measure timing impact)
  PTT_pin(HIGH);

  // Send register 0 for TX operation, but do not activate yet.
  // This is done in the interrupt at the correct time
  AD7021_control_word = ADF7021_TX_REG0;
  Send_AD7021_control(false);

#if defined(BIDIR_DATA_PIN)
  Data_dir_out(true);  // Data pin output mode
#endif

  totx_request = true;

  // Bounded: the original waits forever if the ADF7021 clock never goes low
  uint32_t guard = 0U;
  while (CLK_pin() && ++guard < 4000000U);
  if (guard >= 4000000U)
    m_hangFlags |= 0x01U;
}

void CIO::setRX(bool doSle)
{
  // PTT pin off (doing it earlier helps to measure timing impact)
  PTT_pin(LOW);

  // Send register 0 for RX operation, but do not activate yet.
  // This is done in the interrupt at the correct time
  AD7021_control_word = ADF7021_RX_REG0;
  Send_AD7021_control(doSle);

#if defined(BIDIR_DATA_PIN)
  Data_dir_out(false);  // Data pin input mode
#endif

  if (!doSle) {
    torx_request = true;

    // Bounded: normally the interrupt completes this on the next TX clock
    // edge. If the clock has stopped, finish the switch here instead of
    // locking the modem up.
    uint32_t guard = 0U;
    while (torx_request && ++guard < 4000000U) { asm volatile ("nop"); }

    if (torx_request) {
      SLE_pin(HIGH);
      asm volatile("nop          \n\t"
                   "nop          \n\t"
                   "nop          \n\t"
                   );
      SLE_pin(LOW);
      SDATA_pin(LOW);

      m_tx = false;
      torx_request = false;
      even = !ADF7021_EVEN_BIT;
      m_hangFlags |= 0x02U;
    }
  }
}

void CIO::setPower(uint8_t power)
{
  m_power = power >> 2;
}

#endif
