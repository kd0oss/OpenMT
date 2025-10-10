/***************************************************************************
 *   Copyright (C) 2025 by Rick KD0OSS                                     *
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
 *   Some functions based on or inspired by MMDVM Jonathan Naylor G4KLX    *
 ***************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <cstdint>
#include <unistd.h>
#include <string.h>
#include <string>
#include <ctype.h>
#include "../tools/tools.h"
#include "mmdvm.h"
#include "../tools/RingBuffer.h"
#include "../tools/CRingBuffer.h"

// ******* MMDVM specific default parameters  ****
bool             modem_dmrEnabled          = false;
unsigned int     modem_dmrColorCode        = 2U;
unsigned int     modem_dmrDelay            = 0U;
float            modem_dmrTXLevel          = 50U;

bool             modem_pocsagEnabled       = false;
float            modem_pocsagTXLevel       = 50U;
char             modem_pocsagFrequency[11] = "0";

bool             modem_nxdnEnabled         = false;
float            modem_nxdnTXLevel         = 50U;
unsigned int     modem_nxdnTXHang          = 5U;

bool             modem_fmEnabled           = false;
float            modem_fmTXLevel           = 50U;
float            modem_cwIdTXLevel         = 50U;

bool             modem_p25Enabled          = false;
unsigned int     modem_p25TXHang           = 5U;
float            modem_p25TXLevel          = 50U;

bool             modem_dstarEnabled        = false;
float            modem_dstarTXLevel        = 50U;
uint8_t          dstar_space               = 0U;

bool             modem_m17Enabled          = false;
float            modem_m17TXLevel          = 50U;
unsigned int     modem_m17TXHang           = 5U;
uint8_t          m17_space                 = 0U;

bool             modem_ysfEnabled          = false;
bool             modem_ysfLoDev            = false;
float            modem_ysfTXLevel          = 50U;
unsigned int     modem_ysfTXHang           = 4U;

bool             modem_ax25Enabled         = false;
int              modem_ax25RXTwist         = 6U;
unsigned int     modem_ax25TXDelay         = 300U;
unsigned int     modem_ax25SlotTime        = 50U;
unsigned int     modem_ax25PPersist        = 128U;
float            modem_ax25TXLevel         = 50U;
// ******************************************************

extern RingBuffer<uint8_t> modemCommandBuffer; //< Host out going command queue.
extern std::string modem;                       //< modem type

// function to return M17 queue free space in modem.
uint8_t getM17Space()
{
    return m17_space;
}

// function to record M17 queue free space.
void setM17Space(uint8_t space)
{
    m17_space = space;
}

// function to return DSTAR queue free space in modem.
uint8_t getDSTARSpace()
{
    return dstar_space;
}

// function to record DSTAR queue free space.
void setDSTARSpace(uint8_t space)
{
    dstar_space = space;
}

// function to enable M17 mode in modem.
void enableM17()
{
    uint8_t buffer[4];

    if (!modem_m17Enabled)
    {
        modem_m17Enabled = true;
        if (modem == "mmdvmhs")
            set_ConfigHS();
        else
            set_Config();
        sleep(1);
    }
    buffer[0] = 0xE0;
    buffer[1] = 0x04;
    buffer[2] = MODEM_MODE;
    buffer[3] = 0x07; // M17_MODE
    modemCommandBuffer.addData(buffer, 4);
}

// function to disable M17 mode in modem.
void disableM17()
{
    uint8_t buffer[4];

    modem_m17Enabled = false;
    if (modem == "mmdvmhs")
        set_ConfigHS();
    else
        set_Config();
    sleep(1);

    buffer[0] = 0xE0;
    buffer[1] = 0x04;
    buffer[2] = MODEM_MODE;
    buffer[3] = 0x00; // IDLE_MODE
    modemCommandBuffer.addData(buffer, 4);
}

// function to enable DSTAR mode in modem.
void enableDSTAR()
{
    uint8_t buffer[4];

    if (!modem_dstarEnabled)
    {
        modem_dstarEnabled = true;
        if (modem == "mmdvmhs")
            set_ConfigHS();
        else
            set_Config();
        sleep(1);
    }
    buffer[0] = 0xE0;
    buffer[1] = 0x04;
    buffer[2] = MODEM_MODE;
    buffer[3] = 0x01; // DSTAR_MODE
    modemCommandBuffer.addData(buffer, 4);
}

// function to disable DSTAR mode in modem.
void disableDSTAR()
{
    uint8_t buffer[4];

    modem_dstarEnabled = false;
    if (modem == "mmdvmhs")
        set_ConfigHS();
    else
        set_Config();
    sleep(1);

    buffer[0] = 0xE0;
    buffer[1] = 0x04;
    buffer[2] = MODEM_MODE;
    buffer[3] = 0x00; // IDLE_MODE
    modemCommandBuffer.addData(buffer, 4);
}

// function to comvert OpemMT type to MMDVM type.
bool openMTtoMMDVM(uint8_t mmdvm_in, char* openmt_out)
{
    bool ret = false;

    switch (mmdvm_in)
    {
        case TYPE_M17_LSF:
            strcpy(openmt_out, "M17L");
            ret = true;
        break;

        case TYPE_M17_STREAM:
            strcpy(openmt_out, "M17S");
            ret = true;
        break;

        case TYPE_M17_PACKET:
            strcpy(openmt_out, "M17P");
            ret = true;
        break;

        case TYPE_M17_EOT:
            strcpy(openmt_out, "M17E");
            ret = true;
        break;
    }
    return ret;
}

// function to comvert MMDVM type to OpenMT type.
bool mmdvmToOpenMT(const char* openmt_in, uint8_t mmdvm_out)
{
    bool ret = false;

    if (strcasecmp(openmt_in, "M17L") == 0)
    {
        mmdvm_out = TYPE_M17_LSF;
        ret = true;
    }
    else if (strcasecmp(openmt_in, "M17S") == 0)
    {
        mmdvm_out = TYPE_M17_STREAM;
        ret = true;
    }
    else if (strcasecmp(openmt_in, "M17P") == 0)
    {
        mmdvm_out = TYPE_M17_PACKET;
        ret = true;
    }
    else if (strcasecmp(openmt_in, "M17E") == 0)
    {
        mmdvm_out = TYPE_M17_EOT;
        ret = true;
    }
    return ret;
}

// function to setup frequencies in MMDVM Hotspot.
void setFrequency(const char* rxFreq, const char* txFreq, const char* pocsagFreq, uint8_t rfPower)
{
    uint8_t buffer[17];
    buffer[0] = MODEM_FRAME_START;
    buffer[1] = 17;
    buffer[2] = MODEM_SET_FREQ;
    buffer[3] = 0;

    uint32_t freq = atol(rxFreq);
    buffer[4] = freq & 0x000000ff;
    buffer[5] = (freq & 0x0000ff00) >> 8;
    buffer[6] = (freq & 0x00ff0000) >> 16;
    buffer[7] = (freq & 0xff000000) >> 24;

    freq = atol(txFreq);
    buffer[8] = freq & 0x000000ff;
    buffer[9] = (freq & 0x0000ff00) >> 8;
    buffer[10] = (freq & 0x00ff0000) >> 16;
    buffer[11] = (freq & 0xff000000) >> 24;

    buffer[12] = rfPower;

    freq = atol(pocsagFreq);
    buffer[13] = freq & 0x000000ff;
    buffer[14] = (freq & 0x0000ff00) >> 8;
    buffer[15] = (freq & 0x00ff0000) >> 16;
    buffer[16] = (freq & 0xff000000) >> 24;

    modemCommandBuffer.addData(buffer, 17);
}

// function to set config in MMDVM modem.
bool set_Config()
{
    uint8_t buffer[50U];

    buffer[0U] = MODEM_FRAME_START;
    buffer[1U] = 40U;
    buffer[2U] = MODEM_CONFIG;

    buffer[3U] = 0x00U;
    modem_rxInvert = readModemConfig("modem1", "rxInvert") == "true" ? true : false;
    modem_txInvert = readModemConfig("modem1", "txInvert") == "true" ? true : false;
    modem_pttInvert = readModemConfig("modem1", "pttInvert") == "true" ? true : false;
//    modem_ysfLoDev = readModemConfig("modem1", "ysfLoDev") == "true" ? true : false;
    modem_debug = readModemConfig("modem1", "debug") == "true" ? true : false;
    modem_trace = readModemConfig("modem1", "trace") == "true" ? true : false;
    modem_useCOSAsLockout = readModemConfig("modem1", "useCOSAsLockout") == "true" ? true : false;
    modem_duplex = readModemConfig("modem1", "mode") == "duplex" ? true : false;

    if (modem_rxInvert)
        buffer[3U] |= 0x01U;
    if (modem_txInvert)
        buffer[3U] |= 0x02U;
    if (modem_pttInvert)
        buffer[3U] |= 0x04U;
    if (modem_ysfLoDev)
        buffer[3U] |= 0x08U;
    if (modem_debug)
        buffer[3U] |= 0x10U;
    if (modem_useCOSAsLockout)
        buffer[3U] |= 0x20U;
    if (!modem_duplex)
        buffer[3U] |= 0x80U;

//    modem_dstarEnabled = readModemConfig("modem1", "dstarEnabled") == "true" ? true : false;
//    modem_dmrEnabled = readModemConfig("modem1", "dmrEnabled") == "true" ? true : false;
//    modem_ysfEnabled = readModemConfig("modem1", "ysfEnabled") == "true" ? true : false;
//    modem_p25Enabled = readModemConfig("modem1", "p25Enabled") == "true" ? true : false;
//    modem_nxdnEnabled = readModemConfig("modem1", "nxdnEnabled") == "true" ? true : false;
    modem_fmEnabled = readModemConfig("modem1", "fmEnabled") == "true" ? true : false;
//    modem_m17Enabled = readModemConfig("modem1", "m17Enabled") == "true" ? true : false;

    buffer[4U] = 0x00U;
    if (modem_dstarEnabled)
        buffer[4U] |= 0x01U;
    if (modem_dmrEnabled)
        buffer[4U] |= 0x02U;
    if (modem_ysfEnabled)
        buffer[4U] |= 0x04U;
    if (modem_p25Enabled)
        buffer[4U] |= 0x08U;
    if (modem_nxdnEnabled)
        buffer[4U] |= 0x10U;
    if (modem_fmEnabled)
        buffer[4U] |= 0x20U;
    if (modem_m17Enabled)
        buffer[4U] |= 0x40U;

//    modem_pocsagEnabled = readModemConfig("modem1", "pocsagEnabled") == "true" ? true : false;
//    modem_ax25Enabled = readModemConfig("modem1", "ax25Enabled") == "true" ? true : false;

    buffer[5U] = 0x00U;
    if (modem_pocsagEnabled)
        buffer[5U] |= 0x01U;
    if (modem_ax25Enabled)
        buffer[5U] |= 0x02U;

    modem_txDelay = atoi(readModemConfig("modem1", "txdelay").c_str());

    buffer[6U] = modem_txDelay / 10U;        // In 10ms units

    buffer[7U] = MODE_IDLE;

    modem_txDCOffset = atoi(readModemConfig("modem1", "txDCOffset").c_str());
    modem_txDCOffset = atoi(readModemConfig("modem1", "txDCOffset").c_str());

    buffer[8U] = (unsigned char)(modem_txDCOffset + 128);
    buffer[9U] = (unsigned char)(modem_rxDCOffset + 128);

    modem_rxLevel = atoi(readModemConfig("modem1", "rxLevel").c_str());

    buffer[10U] = (unsigned char)(modem_rxLevel * 2.55F + 0.5F);

    modem_cwIdTXLevel = atoi(readModemConfig("modem1", "cwIdTxLevel").c_str());
//    modem_dstarTXLevel = atoi(readModemConfig("modem1", "dstarTxLevel").c_str());
//    modem_dmrTXLevel = atoi(readModemConfig("modem1", "dmrTxLevel").c_str());
//    modem_ysfTXLevel = atoi(readModemConfig("modem1", "ysfTxLevel").c_str());
//    modem_p25TXLevel = atoi(readModemConfig("modem1", "p25TxLevel").c_str());
//    modem_nxdnTXLevel = atoi(readModemConfig("modem1", "nxdnTxLevel").c_str());
//    modem_m17TXLevel = atoi(readModemConfig("modem1", "m17TxLevel").c_str());
//    modem_pocsagTXLevel = atoi(readModemConfig("modem1", "pocsagTxLevel").c_str());
    modem_fmTXLevel = atoi(readModemConfig("modem1", "fmTxLevel").c_str());
//    modem_ax25TXLevel = atoi(readModemConfig("modem1", "ax25TxLevel").c_str());

    buffer[11U] = (unsigned char)(modem_cwIdTXLevel * 2.55F + 0.5F);
    buffer[12U] = (unsigned char)(modem_dstarTXLevel * 2.55F + 0.5F);
    buffer[13U] = (unsigned char)(modem_dmrTXLevel * 2.55F + 0.5F);
    buffer[14U] = (unsigned char)(modem_ysfTXLevel * 2.55F + 0.5F);
    buffer[15U] = (unsigned char)(modem_p25TXLevel * 2.55F + 0.5F);
    buffer[16U] = (unsigned char)(modem_nxdnTXLevel * 2.55F + 0.5F);
    buffer[17U] = (unsigned char)(modem_m17TXLevel * 2.55F + 0.5F);
    buffer[18U] = (unsigned char)(modem_pocsagTXLevel * 2.55F + 0.5F);
    buffer[19U] = (unsigned char)(modem_fmTXLevel * 2.55F + 0.5F);
    buffer[20U] = (unsigned char)(modem_ax25TXLevel * 2.55F + 0.5F);
    buffer[21U] = 0x00U;
    buffer[22U] = 0x00U;

//    modem_ysfTXHang = atoi(readModemConfig("modem1", "ysfTXHang").c_str());
//    modem_p25TXHang = atoi(readModemConfig("modem1", "p25TXHang").c_str());
//    modem_nxdnTXHang = atoi(readModemConfig("modem1", "nxdnTXHang").c_str());
//    modem_m17TXHang = atoi(readModemConfig("modem1", "m17TXHang").c_str());

    buffer[23U] = (unsigned char)modem_ysfTXHang;
    buffer[24U] = (unsigned char)modem_p25TXHang;
    buffer[25U] = (unsigned char)modem_nxdnTXHang;
    buffer[26U] = (unsigned char)modem_m17TXHang;
    buffer[27U] = 0x00U;
    buffer[28U] = 0x00U;

//    modem_dmrColorCode = atoi(readModemConfig("modem1", "dmrColorCode").c_str());
//    modem_dmrDelay = atoi(readModemConfig("modem1", "dmrdelay").c_str());

    buffer[29U] = modem_dmrColorCode;
    buffer[30U] = modem_dmrDelay;

//    modem_ax25RXTwist = atoi(readModemConfig("modem1", "ax25RXTwist").c_str());
//    modem_ax25TXDelay = atoi(readModemConfig("modem1", "ax25TXDelay").c_str());
//    modem_ax25SlotTime = atoi(readModemConfig("modem1", "ax25SlotTime").c_str());
//    modem_ax25PPersist = atoi(readModemConfig("modem1", "ax25PPersist").c_str());

    buffer[31U] = (unsigned char)(modem_ax25RXTwist + 128);
    buffer[32U] = modem_ax25TXDelay / 10U;        // In 10ms units
    buffer[33U] = modem_ax25SlotTime / 10U;        // In 10ms units
    buffer[34U] = modem_ax25PPersist;

    buffer[35U] = 0x00U;
    buffer[36U] = 0x00U;
    buffer[37U] = 0x00U;
    buffer[38U] = 0x00U;
    buffer[39U] = 0x00U;

    modemCommandBuffer.addData(buffer, 40);

    return true;
}

// function to set config in MMDVM Hotspot.
bool set_ConfigHS()
{
    uint8_t buffer[30U];

    buffer[0U] = MODEM_FRAME_START;
    buffer[1U] = 26U;
    buffer[2U] = MODEM_CONFIG;

    buffer[3U] = 0x00U;
    modem_rxInvert = readModemConfig("modem1", "rxInvert") == "true" ? true : false;
    modem_txInvert = readModemConfig("modem1", "txInvert") == "true" ? true : false;
    modem_pttInvert = readModemConfig("modem1", "pttInvert") == "true" ? true : false;
//    modem_ysfLoDev = readModemConfig("modem1", "ysfLoDev") == "true" ? true : false;
    modem_debug = readModemConfig("modem1", "debug") == "true" ? true : false;
    modem_trace = readModemConfig("modem1", "trace") == "true" ? true : false;
    modem_useCOSAsLockout = readModemConfig("modem1", "useCOSAsLockout") == "true" ? true : false;
    modem_duplex = readModemConfig("modem1", "mode") == "duplex" ? true : false;

	if (modem_rxInvert)
		buffer[3U] |= 0x01U;
	if (modem_txInvert)
		buffer[3U] |= 0x02U;
	if (modem_pttInvert)
		buffer[3U] |= 0x04U;
    if (modem_ysfLoDev)
        buffer[3U] |= 0x08U;
	if (modem_debug)
		buffer[3U] |= 0x10U;
	if (modem_useCOSAsLockout)
		buffer[3U] |= 0x20U;
    if (!modem_duplex)
        buffer[3U] |= 0x80U;

//    modem_dstarEnabled = readModemConfig("modem1", "dstarEnabled") == "true" ? true : false;
//    modem_dmrEnabled = readModemConfig("modem1", "dmrEnabled") == "true" ? true : false;
//    modem_ysfEnabled = readModemConfig("modem1", "ysfEnabled") == "true" ? true : false;
//    modem_p25Enabled = readModemConfig("modem1", "p25Enabled") == "true" ? true : false;
//    modem_nxdnEnabled = readModemConfig("modem1", "nxdnEnabled") == "true" ? true : false;
//    modem_fmEnabled = readModemConfig("modem1", "fmEnabled") == "true" ? true : false;
//    modem_m17Enabled = readModemConfig("modem1", "m17Enabled") == "true" ? true : false;
//    modem_pocsagEnabled = readModemConfig("modem1", "pocsagEnabled") == "true" ? true : false;

    buffer[4U] = 0x00U;
    if (modem_dstarEnabled)
        buffer[4U] |= 0x01U;
    if (modem_dmrEnabled)
        buffer[4U] |= 0x02U;
    if (modem_ysfEnabled)
        buffer[4U] |= 0x04U;
    if (modem_p25Enabled)
        buffer[4U] |= 0x08U;
    if (modem_nxdnEnabled)
        buffer[4U] |= 0x10U;
    if (modem_pocsagEnabled)
        buffer[4U] |= 0x20U;
    if (modem_m17Enabled)
        buffer[4U] |= 0x40U;

    modem_txDelay = atoi(readModemConfig("modem1", "txdelay").c_str());

    buffer[5U] = modem_txDelay / 10U;        // In 10ms units

    buffer[6U] = MODE_IDLE;
	buffer[7U] = (unsigned char)(modem_rxLevel * 2.55F + 0.5F);

//    modem_cwIdTXLevel = atoi(readModemConfig("modem1", "cwIdTxLevel").c_str());
//    modem_dstarTXLevel = atoi(readModemConfig("modem1", "dstarTxLevel").c_str());
//    modem_dmrTXLevel = atoi(readModemConfig("modem1", "dmrTxLevel").c_str());
//    modem_ysfTXLevel = atoi(readModemConfig("modem1", "ysfTxLevel").c_str());
//    modem_p25TXLevel = atoi(readModemConfig("modem1", "p25TxLevel").c_str());
//    modem_nxdnTXLevel = atoi(readModemConfig("modem1", "nxdnTxLevel").c_str());
//    modem_m17TXLevel = atoi(readModemConfig("modem1", "m17TxLevel").c_str());
//    modem_pocsagTXLevel = atoi(readModemConfig("modem1", "pocsagTxLevel").c_str());
//    modem_ysfTXHang = atoi(readModemConfig("modem1", "ysfTXHang").c_str());
//    modem_p25TXHang = atoi(readModemConfig("modem1", "p25TXHang").c_str());
//    modem_nxdnTXHang = atoi(readModemConfig("modem1", "nxdnTXHang").c_str());
//    modem_m17TXHang = atoi(readModemConfig("modem1", "m17TXHang").c_str());

    buffer[8] = (unsigned char)(modem_cwIdTXLevel * 2.55F + 0.5F) << 2;
	buffer[9U] = modem_dmrColorCode;

	buffer[10U] = modem_dmrDelay;

	buffer[11U] = 128U;           // Was OscOffset

    buffer[12U] = (unsigned char)(modem_dstarTXLevel * 2.55F + 0.5F);
    buffer[13U] = (unsigned char)(modem_dmrTXLevel * 2.55F + 0.5F);
    buffer[14U] = (unsigned char)(modem_ysfTXLevel * 2.55F + 0.5F);
    buffer[15U] = (unsigned char)(modem_p25TXLevel * 2.55F + 0.5F);
	buffer[16U] = (unsigned char)(modem_txDCOffset + 128);
	buffer[17U] = (unsigned char)(modem_rxDCOffset + 128);

	buffer[18U] = (unsigned char)(modem_nxdnTXLevel * 2.55F + 0.5F);

	buffer[19U] = (unsigned char)modem_ysfTXHang;

	buffer[20U] = (unsigned char)(modem_pocsagTXLevel * 2.55F + 0.5F);

	buffer[21U] = (unsigned char)(modem_fmTXLevel * 2.55F + 0.5F);

	buffer[22U] = (unsigned char)modem_p25TXHang;

	buffer[23U] = (unsigned char)modem_nxdnTXHang;

	buffer[24U] = (unsigned char)(modem_m17TXLevel * 2.55F + 0.5F);

	buffer[25U] = (unsigned char)modem_m17TXHang;

    modemCommandBuffer.addData(buffer, 26);

    return true;
}
