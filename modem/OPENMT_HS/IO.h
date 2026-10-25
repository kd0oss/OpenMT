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

#if !defined(CIO_H)
#define  CIO_H

#include "Config.h"
#include "Globals.h"
#include "BitRB.h"
#include "OpenMT.h"

#define LOW      0
#define HIGH     1

// HS frequency ranges
#define VHF1_MIN  144000000
#define VHF1_MAX  148000000
#define VHF2_MIN  219000000
#define VHF2_MAX  225000000
#define UHF1_MIN  420000000
#define UHF1_MAX  475000000
#define UHF2_MIN  842000000
#define UHF2_MAX  950000000

// Banned amateur frequency ranges (satellite only, ISS, etc)
#define BAN1_MIN  145800000
#define BAN1_MAX  146000000
#define BAN2_MIN  435000000
#define BAN2_MAX  438000000

extern uint32_t  m_frequency_rx;
extern uint32_t  m_frequency_tx;
extern uint32_t  m_pocsag_freq_tx;
extern uint8_t   m_power;

class CIO {

public:
  CIO();

  // Platform API
  void      Init(void);
  void      SCLK_pin(bool on);
  void      SDATA_pin(bool on);
  bool      SREAD_pin(void);
  void      SLE_pin(bool on);
#if defined(DUPLEX)
  void      SLE2_pin(bool on);
  bool      RXD2_pin(void);
#endif
  void      CE_pin(bool on);
  bool      RXD_pin(void);
  bool      CLK_pin(void);

#if defined(BIDIR_DATA_PIN)
  void      RXD_pin_write(bool on);
#endif

  void      TXD_pin(bool on);
  void      PTT_pin(bool on);
  void      LED_pin(bool on);
  void      DEB_pin(bool on);
  void      DSTAR_pin(bool on);
  void      DMR_pin(bool on);
  void      YSF_pin(bool on);
  void      P25_pin(bool on);
  void      NXDN_pin(bool on);
  void      M17_pin(bool on);
  void      POCSAG_pin(bool on);
  void      COS_pin(bool on);
  void      interrupt(void);
#if defined(DUPLEX)
  void      interrupt2(void);
#endif

#if defined(BIDIR_DATA_PIN)
  void      Data_dir_out(bool dir);
#endif

  // IO API
  uint16_t  getSpace(void) const;
  void      process(void);
  uint8_t   setFreq(uint32_t frequency_rx, uint32_t frequency_tx, uint8_t rf_power, uint32_t pocsag_freq_tx);
  void      setPower(uint8_t power);
  void      setMode(MMDVM_STATE modemState);
  void      setDecode(bool dcd);
  void      selfTest(void);
#if defined(ZUMSPOT_ADF7021) || defined(LONESTAR_USB) || defined(SKYBRIDGE_HS)
  void      checkBand(uint32_t frequency_rx, uint32_t frequency_tx);
  uint8_t   checkZUMspot(uint32_t frequency_rx, uint32_t frequency_tx);
  void      setBandVHF(bool vhf_on);
  bool      hasSingleADF7021(void);
  bool      isDualBand(void);
#endif

  // RF interface API
  void      setTX(void);
  void      setRX(bool doSle = true);
  void      start(void);
  void      startInt(void);

  // OpenMT bit stream API
  bool      getRXBit(uint8_t& bit);
  void      clearRX(void);
  void      writeTXBits(const uint8_t* data, uint16_t length, bool msbFirst, bool invert);
  uint16_t  getTXData(void) const;
  uint16_t  getTXPhase(uint16_t period);   // bits sent since the TX started, modulo period; 0xFFFF when not transmitting
  void      keyTX(void);
  uint32_t  getTicks(void) const;
  uint32_t  getMillis(void) const;
  void      ifConfRegs(const OpenMTMode& mode, bool reset);
  uint16_t  readRSSI(void);
  int16_t   readAFC(void);
  uint16_t  readback(uint32_t reg7);
  void      loopbackTune(uint32_t reg0);
  uint8_t   takeHangFlags(void);
#if defined(DUPLEX)
  void      ifConfRegs2(const OpenMTMode& mode);
#endif

  // Misc functions
  void      dlybit(void);
  void      delay_IFcal(void);
  void      delay_reset(void);
  void      delay_us(uint32_t us);

private:
  uint8_t            m_RX_N_divider;
  uint16_t           m_RX_F_divider;
  uint8_t            m_TX_N_divider;
  uint16_t           m_TX_F_divider;

  bool               m_started;
  CBitRB             m_rxBuffer;
  CBitRB             m_txBuffer;
  uint32_t           m_ledCount;
  bool               m_ledValue;
  volatile uint32_t  m_ticks;
  volatile uint8_t   m_hangFlags;
  uint32_t           m_txBitsPut;

};

#endif
