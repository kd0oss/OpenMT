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
 *   along with this program; if not, see <http:/* www.gnu.org/licenses/>  *
 *                                                                         *
 *   Some functions based on or inspired by MMDVM Jonathan Naylor G4KLX    *
 ***************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <strings.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <tools.h>
#include <mmdvm.h>
#include <RingBuffer.h>

/* MMDVM specific default parameters */
bool             modem_dmrEnabled          = false;
unsigned int     modem_dmrColorCode        = 1U;
unsigned int     modem_dmrDelay            = 0U;
uint8_t          modem_dmrTXLevel          = 50U;
uint8_t          dmr_space[2]              = {0U, 0U};

bool             modem_pocsagEnabled       = false;
uint8_t            modem_pocsagTXLevel       = 50U;
char             modem_pocsagFrequency[11] = "0";

bool             modem_nxdnEnabled         = false;
uint8_t            modem_nxdnTXLevel         = 50U;
unsigned int     modem_nxdnTXHang          = 5U;

bool             modem_fmEnabled           = false;
uint8_t            modem_fmTXLevel           = 50U;
uint8_t            modem_cwIdTXLevel         = 50U;

bool             modem_p25Enabled          = false;
unsigned int     modem_p25TXHang           = 5U;
uint8_t            modem_p25TXLevel          = 50U;
uint8_t          p25_space                 = 0U;

bool             modem_dstarEnabled        = false;
uint8_t            modem_dstarTXLevel        = 50U;
uint8_t          dstar_space               = 0U;

bool             modem_m17Enabled          = false;
uint8_t            modem_m17TXLevel          = 50U;
unsigned int     modem_m17TXHang           = 5U;
uint8_t          m17_space                 = 0U;

bool             modem_ysfEnabled          = false;
bool             modem_ysfLoDev            = false;
uint8_t            modem_ysfTXLevel          = 50U;
unsigned int     modem_ysfTXHang           = 4U;

bool             modem_ax25Enabled         = false;
int              modem_ax25RXTwist         = 6U;
unsigned int     modem_ax25TXDelay         = 300U;
unsigned int     modem_ax25SlotTime        = 50U;
unsigned int     modem_ax25PPersist        = 128U;
uint8_t            modem_ax25TXLevel         = 50U;
/* End of MMDVM parameters */

/* External references from Modem_Host */
extern RingBuffer modemCommandBuffer;  /* Modem command buffer */
extern char modem[20];                 /* modem type */

/* function to return M17 queue free space in modem */
uint8_t getM17Space()
{
    return m17_space;
}

/* function to record M17 queue free space. */
void setM17Space(uint8_t space)
{
    m17_space = space;
}

/* function to return DSTAR queue free space in modem */
uint8_t getDSTARSpace()
{
    return dstar_space;
}

/* function to record DSTAR queue free space */
void setDSTARSpace(uint8_t space)
{
    dstar_space = space;
}

/* function to return P25 queue free space in modem */
uint8_t getP25Space()
{
    return p25_space;
}

/* function to record P25 queue free space */
void setP25Space(uint8_t space)
{
    p25_space = space;
}

/* function to return DMR queue free space in modem */
uint8_t getDMRSpace(uint8_t slot)
{
    return dmr_space[slot - 1];
}

/* function to record DMR queue free space */
void setDMRSpace(uint8_t space1, uint8_t space2)
{
    dmr_space[0] = space1;
    dmr_space[1] = space2;
}

/* function to enable M17 mode in modem */
void enableM17(const char* modem_name)
{
    uint8_t buffer[4];

    if (!modem_m17Enabled)
    {
        modem_m17Enabled = true;
        if (strcasecmp(modem, "mmdvmhs") == 0)
            set_ConfigHS(modem_name);
        else
            set_Config(modem_name);
        sleep(1);
    }
    buffer[0] = 0xE0;
    buffer[1] = 0x04;
    buffer[2] = MODEM_MODE;
    buffer[3] = 0x07; /* M17_MODE */
    RingBuffer_addData(&modemCommandBuffer, buffer, 4);
}

/* function to disable M17 mode in modem. */
void disableM17(const char* modem_name)
{
    uint8_t buffer[4];

    modem_m17Enabled = false;
    if (strcasecmp(modem, "mmdvmhs") == 0)
        set_ConfigHS(modem_name);
    else
        set_Config(modem_name);
    sleep(1);

    buffer[0] = 0xE0;
    buffer[1] = 0x04;
    buffer[2] = MODEM_MODE;
    buffer[3] = 0x00; /* IDLE_MODE */
    RingBuffer_addData(&modemCommandBuffer, buffer, 4);
}

/* function to enable DSTAR mode in modem. */
void enableDSTAR(const char* modem_name)
{
    uint8_t buffer[4];

    if (!modem_dstarEnabled)
    {
        modem_dstarEnabled = true;
        if (strcasecmp(modem, "mmdvmhs") == 0)
            set_ConfigHS(modem_name);
        else
            set_Config(modem_name);
        sleep(1);
    }
    buffer[0] = 0xE0;
    buffer[1] = 0x04;
    buffer[2] = MODEM_MODE;
    buffer[3] = 0x01; /* DSTAR_MODE */
    RingBuffer_addData(&modemCommandBuffer, buffer, 4);
}

/* function to disable DSTAR mode in modem. */
void disableDSTAR(const char* modem_name)
{
    uint8_t buffer[4];

    modem_dstarEnabled = false;
    if (strcasecmp(modem, "mmdvmhs") == 0)
        set_ConfigHS(modem_name);
    else
        set_Config(modem_name);
    sleep(1);

    buffer[0] = 0xE0;
    buffer[1] = 0x04;
    buffer[2] = MODEM_MODE;
    buffer[3] = 0x00; /* IDLE_MODE */
    RingBuffer_addData(&modemCommandBuffer, buffer, 4);
}

/* function to enable P25 mode in modem. */
void enableP25(const char* modem_name)
{
    uint8_t buffer[4];

    if (!modem_p25Enabled)
    {
        modem_p25Enabled = true;
        if (strcasecmp(modem, "mmdvmhs") == 0)
            set_ConfigHS(modem_name);
        else
            set_Config(modem_name);
        sleep(1);
    }
    buffer[0] = 0xE0;
    buffer[1] = 0x04;
    buffer[2] = MODEM_MODE;
    buffer[3] = 0x04; /* P25_MODE */
    RingBuffer_addData(&modemCommandBuffer, buffer, 4);
}

/* function to disable P25 mode in modem. */
void disableP25(const char* modem_name)
{
    uint8_t buffer[4];

    modem_p25Enabled = false;
    if (strcasecmp(modem, "mmdvmhs") == 0)
        set_ConfigHS(modem_name);
    else
        set_Config(modem_name);
    sleep(1);

    buffer[0] = 0xE0;
    buffer[1] = 0x04;
    buffer[2] = MODEM_MODE;
    buffer[3] = 0x00; /* IDLE_MODE */
    RingBuffer_addData(&modemCommandBuffer, buffer, 4);
}

/* function to enable DMR mode in modem. */
void enableDMR(const char* modem_name)
{
    uint8_t buffer[4];

    if (!modem_dmrEnabled)
    {
        modem_dmrEnabled = true;
        if (strcasecmp(modem, "mmdvmhs") == 0)
            set_ConfigHS(modem_name);
        else
            set_Config(modem_name);
        sleep(1);
    }
    buffer[0] = 0xE0;
    buffer[1] = 0x04;
    buffer[2] = MODEM_MODE;
    buffer[3] = 0x02; /* DMR_MODE */
    RingBuffer_addData(&modemCommandBuffer, buffer, 4);
}

/* function to disable DMR mode in modem. */
void disableDMR(const char* modem_name)
{
    uint8_t buffer[4];

    modem_dmrEnabled = false;
    if (strcasecmp(modem, "mmdvmhs") == 0)
        set_ConfigHS(modem_name);
    else
        set_Config(modem_name);
    sleep(1);

    buffer[0] = 0xE0;
    buffer[1] = 0x04;
    buffer[2] = MODEM_MODE;
    buffer[3] = 0x00; /* IDLE_MODE */
    RingBuffer_addData(&modemCommandBuffer, buffer, 4);
}

/* function to comvert OpemMT type to MMDVM type. */
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

/* function to comvert MMDVM type to OpenMT type. */
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

/* function to setup frequencies in MMDVM Hotspot. */
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

    RingBuffer_addData(&modemCommandBuffer, buffer, 17);
}

/* function to set config in MMDVM modem. */
bool set_Config(const char* modem_name)
{
    uint8_t buffer[50U];
    char    tmp[20];

    bzero(buffer, 50);

    buffer[0U] = MODEM_FRAME_START;
    buffer[1U] = 40U;
    buffer[2U] = MODEM_CONFIG;

    readHostConfig(modem_name, "config", "rxInvert", tmp);
    modem_rxInvert = strcasecmp(tmp, "true") == 0 ? true : false;
    readHostConfig(modem_name, "config", "txInvert", tmp);
    modem_txInvert = strcasecmp(tmp, "true") == 0 ? true : false;
    readHostConfig(modem_name, "config", "pttInvert", tmp);
    modem_pttInvert = strcasecmp(tmp, "true") == 0 ? true : false;
/* modem_ysfLoDev = readHostConfig(modem_name, "config", "ysfLoDev") == "true" ? true : false; */
    readHostConfig(modem_name, "config", "debug", tmp);
    modem_debug = strcasecmp(tmp, "true") == 0 ? true : false;
    readHostConfig(modem_name, "config", "trace", tmp);
    modem_trace = strcasecmp(tmp, "true") == 0 ? true : false;
    readHostConfig(modem_name, "config", "useCOSAsLockout", tmp);
    modem_useCOSAsLockout = strcasecmp(tmp, "true") == 0 ? true : false;
    readHostConfig(modem_name, "config", "mode", tmp);
    modem_duplex = strcasecmp(tmp, "duplex") == 0 ? true : false;

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

/* modem_dstarEnabled = readHostConfig(modem_name, "config", "dstarEnabled") == "true" ? true : false; */
/* modem_dmrEnabled = readHostConfig(modem_name, "config", "dmrEnabled") == "true" ? true : false; */
/* modem_ysfEnabled = readHostConfig(modem_name, "config", "ysfEnabled") == "true" ? true : false; */
/* modem_p25Enabled = readHostConfig(modem_name, "config", "p25Enabled") == "true" ? true : false; */
/* modem_nxdnEnabled = readHostConfig(modem_name, "config", "nxdnEnabled") == "true" ? true : false; */
    readHostConfig(modem_name, "config", "fmEnabled", tmp);
    modem_fmEnabled = strcasecmp(tmp, "true") == 0 ? true : false;
/* modem_m17Enabled = readHostConfig(modem_name, "config", "m17Enabled") == "true" ? true : false; */

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

/* modem_pocsagEnabled = readHostConfig(modem_name, "config", "pocsagEnabled") == "true" ? true : false; */
/* modem_ax25Enabled = readHostConfig(modem_name, "config", "ax25Enabled") == "true" ? true : false; */

    buffer[5U] = 0x00U;
    if (modem_pocsagEnabled)
        buffer[5U] |= 0x01U;
    if (modem_ax25Enabled)
        buffer[5U] |= 0x02U;

    readHostConfig(modem_name, "config", "txdelay", tmp);
    modem_txDelay = atoi(tmp);

    buffer[6U] = modem_txDelay / 10U;        /* In 10ms units */

    buffer[7U] = MODE_IDLE;

    readHostConfig(modem_name, "config", "txDCOffset", tmp);
    modem_txDCOffset = atoi(tmp);
    readHostConfig(modem_name, "config", "rxDCOffset", tmp);
    modem_rxDCOffset = atoi(tmp);

    buffer[8U] = (unsigned char)(modem_txDCOffset + 128);
    buffer[9U] = (unsigned char)(modem_rxDCOffset + 128);

    readHostConfig(modem_name, "config", "rxLevel", tmp);
    modem_rxLevel = atoi(tmp);

    buffer[10U] = (unsigned char)(modem_rxLevel * 2.55F + 0.5F);

    readHostConfig(modem_name, "config", "cwIdTxLevel", tmp);
    modem_cwIdTXLevel = atoi(tmp);
    readHostConfig(modem_name, "DSTAR", "txLevel", tmp);
    modem_dstarTXLevel = atoi(tmp);
    readHostConfig(modem_name, "DMR", "txLevel", tmp);
    modem_dmrTXLevel = atoi(tmp);
/* modem_ysfTXLevel = atoi(readHostConfig(modem_name, "config", "ysfTxLevel").c_str()); */
    readHostConfig(modem_name, "P25", "txLevel", tmp);
    modem_p25TXLevel = atoi(tmp);
/* modem_nxdnTXLevel = atoi(readHostConfig(modem_name, "config", "nxdnTxLevel").c_str()); */
    readHostConfig(modem_name, "M17", "txLevel", tmp);
    modem_m17TXLevel = atoi(tmp);
/* modem_pocsagTXLevel = atoi(readHostConfig(modem_name, "config", "pocsagTxLevel").c_str()); */
    readHostConfig(modem_name, "config", "fmTxLevel", tmp);
    modem_fmTXLevel = atoi(tmp);
/* modem_ax25TXLevel = atoi(readHostConfig(modem_name, "config", "ax25TxLevel").c_str()); */

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

/* modem_ysfTXHang = atoi(readHostConfig(modem_name, "config", "ysfTXHang").c_str()); */
/* modem_p25TXHang = atoi(readHostConfig(modem_name, "config", "p25TXHang").c_str()); */
/* modem_nxdnTXHang = atoi(readHostConfig(modem_name, "config", "nxdnTXHang").c_str()); */
/* modem_m17TXHang = atoi(readHostConfig(modem_name, "config", "m17TXHang").c_str()); */

    buffer[23U] = (unsigned char)modem_ysfTXHang;
    buffer[24U] = (unsigned char)modem_p25TXHang;
    buffer[25U] = (unsigned char)modem_nxdnTXHang;
    buffer[26U] = (unsigned char)modem_m17TXHang;
    buffer[27U] = 0x00U;
    buffer[28U] = 0x00U;

/* modem_dmrColorCode = atoi(readHostConfig("config", "dmrColorCode").c_str()); */
/* modem_dmrDelay = atoi(readHostConfig("config", "dmrdelay").c_str()); */

    buffer[29U] = modem_dmrColorCode;
    buffer[30U] = modem_dmrDelay;

/* modem_ax25RXTwist = atoi(readHostConfig("config", "ax25RXTwist").c_str()); */
/* modem_ax25TXDelay = atoi(readHostConfig("config", "ax25TXDelay").c_str()); */
/* modem_ax25SlotTime = atoi(readHostConfig("config", "ax25SlotTime").c_str()); */
/* modem_ax25PPersist = atoi(readHostConfig("config", "ax25PPersist").c_str()); */

    buffer[31U] = (unsigned char)(modem_ax25RXTwist + 128);
    buffer[32U] = modem_ax25TXDelay / 10U;        /* In 10ms units */
    buffer[33U] = modem_ax25SlotTime / 10U;        /* In 10ms units */
    buffer[34U] = modem_ax25PPersist;

    buffer[35U] = 0x00U;
    buffer[36U] = 0x00U;
    buffer[37U] = 0x00U;
    buffer[38U] = 0x00U;
    buffer[39U] = 0x00U;

    uint8_t tmp2[] = {0xE0, 0x28, 0x02, 0x02, buffer[4], 0x00, 0x0A, 0x00, 0x80, 0x80, 0x80, 0x00, 0x80, 0xB3, 0x46, 0x46, 0x46,
        0x46, 0x46, 0x46, 0x46, 0x00, 0x00, 0x14, 0x14, 0x14, 0x14, 0x00, 0x00, 0x01, 0x00,0x80, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

    RingBuffer_addData(&modemCommandBuffer, buffer, 40);

    return true;
}

/* function to set config in MMDVM Hotspot. */
bool set_ConfigHS(const char* modem_name)
{
    uint8_t buffer[30U];

    buffer[0U] = MODEM_FRAME_START;
    buffer[1U] = 26U;
    buffer[2U] = MODEM_CONFIG;

    buffer[3U] = 0x00U;
    char tmp[20];
    readHostConfig(modem_name, "config", "rxInvert", tmp);
    modem_rxInvert = strcasecmp(tmp, "true") == 0 ? true : false;
    readHostConfig(modem_name, "config", "txInvert", tmp);
    modem_txInvert = strcasecmp(tmp, "true") == 0 ? true : false;
    readHostConfig(modem_name, "config", "pttInvert", tmp);
    modem_pttInvert = strcasecmp(tmp, "true") == 0 ? true : false;
/* modem_ysfLoDev = readHostConfig(modem_name, "config", "ysfLoDev") == "true" ? true : false; */
    readHostConfig(modem_name, "config", "debug", tmp);
    modem_debug = strcasecmp(tmp, "true") == 0 ? true : false;
    readHostConfig(modem_name, "config", "trace", tmp);
    modem_trace = strcasecmp(tmp, "true") == 0 ? true : false;
    readHostConfig(modem_name, "config", "useCOSAsLockout", tmp);
    modem_useCOSAsLockout = strcasecmp(tmp, "true") == 0 ? true : false;
    readHostConfig(modem_name, "config", "mode", tmp);
    modem_duplex = strcasecmp(tmp, "duplex") == 0 ? true : false;

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

/* modem_dstarEnabled = readHostConfig(modem_name, "config", "dstarEnabled") == "true" ? true : false; */
/* modem_dmrEnabled = readHostConfig(modem_name, "config", "dmrEnabled") == "true" ? true : false; */
/* modem_ysfEnabled = readHostConfig(modem_name, "config", "ysfEnabled") == "true" ? true : false; */
/* modem_p25Enabled = readHostConfig(modem_name, "config", "p25Enabled") == "true" ? true : false; */
/* modem_nxdnEnabled = readHostConfig(modem_name, "config", "nxdnEnabled") == "true" ? true : false; */
/* modem_fmEnabled = readHostConfig(modem_name, "config", "fmEnabled") == "true" ? true : false; */
/* modem_m17Enabled = readHostConfig(modem_name, "config", "m17Enabled") == "true" ? true : false; */
/* modem_pocsagEnabled = readHostConfig(modem_name, "config", "pocsagEnabled") == "true" ? true : false; */

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

    readHostConfig(modem_name, "config", "txdelay", tmp);
    modem_txDelay = atoi(tmp);

    buffer[5U] = modem_txDelay / 10U;        /* In 10ms units */

    buffer[6U] = MODE_IDLE;
	buffer[7U] = (unsigned char)(modem_rxLevel * 2.55F + 0.5F);

/* modem_cwIdTXLevel = atoi(readHostConfig(modem_name, "config", "cwIdTxLevel").c_str()); */
/* modem_dstarTXLevel = atoi(readHostConfig(modem_name, "config", "dstarTxLevel").c_str()); */
/* modem_dmrTXLevel = atoi(readHostConfig(modem_name, "config", "dmrTxLevel").c_str()); */
/* modem_ysfTXLevel = atoi(readHostConfig(modem_name, "config", "ysfTxLevel").c_str()); */
/* modem_p25TXLevel = atoi(readHostConfig(modem_name, "config", "p25TxLevel").c_str()); */
/* modem_nxdnTXLevel = atoi(readHostConfig(modem_name, "config", "nxdnTxLevel").c_str()); */
/* modem_m17TXLevel = atoi(readHostConfig(modem_name, "config", "m17TxLevel").c_str()); */
/* modem_pocsagTXLevel = atoi(readHostConfig(modem_name, "config", "pocsagTxLevel").c_str()); */
/* modem_ysfTXHang = atoi(readHostConfig(modem_name, "config", "ysfTXHang").c_str()); */
/* modem_p25TXHang = atoi(readHostConfig(modem_name, "config", "p25TXHang").c_str()); */
/* modem_nxdnTXHang = atoi(readHostConfig(modem_name, "config", "nxdnTXHang").c_str()); */
/* modem_m17TXHang = atoi(readHostConfig(modem_name, "config", "m17TXHang").c_str()); */

    readHostConfig(modem_name, "config", "cwIdTxLevel", tmp);
    modem_cwIdTXLevel = atoi(tmp);
    readHostConfig(modem_name, "DSTAR", "txLevel", tmp);
    modem_dstarTXLevel = atoi(tmp);
    readHostConfig(modem_name, "DMR", "txLevel", tmp);
    modem_dmrTXLevel = atoi(tmp);
    readHostConfig(modem_name, "P25", "txLevel", tmp);
    modem_p25TXLevel = atoi(tmp);
    readHostConfig(modem_name, "M17", "txLevel", tmp);
    modem_m17TXLevel = atoi(tmp);

    buffer[8] = (unsigned char)(modem_cwIdTXLevel * 2.55F + 0.5F) << 2;
	buffer[9U] = modem_dmrColorCode;

	buffer[10U] = modem_dmrDelay;

	buffer[11U] = 128U;           /* Was OscOffset */

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

    RingBuffer_addData(&modemCommandBuffer, buffer, 26);

    return true;
}
