/*
 *   Copyright (C) 2013,2015,2016,2018,2020,2021 by Jonathan Naylor G4KLX
 *   Copyright (C) 2016 by Colin Durbridge G4EML
 *   Copyright (C) 2016,2017,2018,2019,2020 by Andy Uribe CA6JAU
 *   Copyright (C) 2019 by Florian Wolters DF2ET
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

#include "Config.h"
#include "Globals.h"
#include "version.h"
#include "SerialPort.h"

// Serial speed towards the host, must match modemBaud in the OpenMT config
#if !defined(OPENMT_SERIAL_SPEED)
#define OPENMT_SERIAL_SPEED 460800
#endif

CSerialPort::CSerialPort() :
m_buffer(),
m_ptr(0U),
m_len(0U),
m_lastByte(0U)
{
}

void CSerialPort::sendACK(uint8_t type)
{
    uint8_t reply[4U];

    reply[0U] = OPENMT_FRAME_START;
    reply[1U] = 4U;
    reply[2U] = OPENMT_ACK;
    reply[3U] = type;

    writeInt(1U, reply, 4U);
}

void CSerialPort::sendNAK(uint8_t type, uint8_t err)
{
    uint8_t reply[5U];

    reply[0U] = OPENMT_FRAME_START;
    reply[1U] = 5U;
    reply[2U] = OPENMT_NAK;
    reply[3U] = type;
    reply[4U] = err;

    writeInt(1U, reply, 5U);
}

void CSerialPort::getVersion()
{
    uint8_t reply[255U];

    reply[0U] = OPENMT_FRAME_START;
    reply[1U] = 0U;
    reply[2U] = OPENMT_GET_VERSION;
    reply[3U] = OPENMT_PROTOCOL_VERSION;

    uint16_t count = 4U;
    for (uint16_t i = 0U; HARDWARE[i] != 0x00U && count < 255U; i++, count++)
        reply[count] = HARDWARE[i];

    reply[1U] = (uint8_t)count;

    writeInt(1U, reply, count);
}

// Status frame: the free space (in host BIT frames) is carried in the low
// nibble of the type byte, which is where Modem_Host.c reads it.
void CSerialPort::writeStatus(uint8_t space)
{
    uint8_t reply[3U];

    if (space > 0x0FU)
        space = 0x0FU;

    reply[0U] = OPENMT_FRAME_START;
    reply[1U] = 3U;
    reply[2U] = OPENMT_GET_STATUS | space;

    writeInt(1U, reply, 3U);
}

void CSerialPort::writeMeasurement(uint8_t connId, const OpenMTMeas& m)
{
    uint8_t reply[14U];

    reply[0U]  = OPENMT_FRAME_START;
    reply[1U]  = 14U;
    reply[2U]  = OPENMT_MEAS | (connId & OPENMT_ID_MASK);
    reply[3U]  = (uint8_t)(m.afc0 & 0xFF);
    reply[4U]  = (uint8_t)((m.afc0 >> 8) & 0xFF);
    reply[5U]  = (uint8_t)(m.afc1 & 0xFF);
    reply[6U]  = (uint8_t)((m.afc1 >> 8) & 0xFF);
    reply[7U]  = (uint8_t)(m.step & 0xFF);
    reply[8U]  = (uint8_t)((m.step >> 8) & 0xFF);
    reply[9U]  = (uint8_t)(m.rssi & 0xFF);
    reply[10U] = (uint8_t)(m.rssi >> 8);
    reply[11U] = (uint8_t)(m.syncs & 0xFF);
    reply[12U] = (uint8_t)(m.syncs >> 8);
    reply[13U] = m.flags;

    writeInt(1U, reply, 14U, true);
}

void CSerialPort::writeProbe(uint8_t connId, const uint16_t* words)
{
    uint8_t reply[19U];

    reply[0U] = OPENMT_FRAME_START;
    reply[1U] = 19U;
    reply[2U] = OPENMT_MEAS | (connId & OPENMT_ID_MASK);

    for (uint8_t i = 0U; i < 8U; i++)
    {
        reply[3U + i * 2U] = (uint8_t)(words[i] & 0xFFU);
        reply[4U + i * 2U] = (uint8_t)(words[i] >> 8);
    }

    writeInt(1U, reply, 19U, true);
}

void CSerialPort::writeBitData(uint8_t connId, const uint8_t* data, uint8_t length)
{
    uint8_t head[3U];

    if (length == 0U || length > 252U)
        return;

    head[0U] = OPENMT_FRAME_START;
    head[1U] = length + 3U;
    head[2U] = OPENMT_BIT_DATA | (connId & OPENMT_ID_MASK);

    writeInt(1U, head, 3U);
    writeInt(1U, data, length);
}

void CSerialPort::start()
{
    beginInt(1U, OPENMT_SERIAL_SPEED);
}

void CSerialPort::process()
{
    uint32_t now = io.getMillis();

    // Drop a frame that stopped arriving part way through
    if (m_ptr > 0U && (now - m_lastByte) >= OPENMT_SERIAL_TIMEOUT_MS)
    {
        m_ptr = 0U;
        m_len = 0U;
    }

    while (availableInt(1U))
    {
        uint8_t c = readInt(1U);

        m_lastByte = now;

        if (m_ptr == 0U)
        {
            // Wait for the start of a frame
            if (c == OPENMT_FRAME_START)
            {
                m_buffer[0U] = c;
                m_ptr = 1U;
            }
        }
        else if (m_ptr == 1U)
        {
            // Total frame length, the header alone is three bytes
            if (c < 3U)
            {
                m_ptr = 0U;
            }
            else
            {
                m_buffer[1U] = c;
                m_len = c;
                m_ptr = 2U;
            }
        }
        else
        {
            m_buffer[m_ptr++] = c;

            if (m_ptr >= m_len)
            {
                processFrame();
                m_ptr = 0U;
                m_len = 0U;
            }
        }
    }
}

void CSerialPort::processFrame()
{
    uint8_t typeByte = m_buffer[2U];
    uint8_t type     = typeByte & OPENMT_TYPE_MASK;
    uint8_t id       = typeByte & OPENMT_ID_MASK;
    const uint8_t* payload = m_buffer + 3U;
    uint8_t length   = (uint8_t)(m_len - 3U);
    uint8_t err      = 0U;

    switch (type)
    {
        case OPENMT_GET_VERSION:
            getVersion();
            break;

        case OPENMT_GET_STATUS:
            writeStatus(openmt.getSpace());
            break;

        case OPENMT_SET_CONFIG:
            err = openmt.setConfig(payload, length);
            if (err == 0U)
                sendACK(typeByte);
            else
                sendNAK(typeByte, err);
            break;

        case OPENMT_SET_MODE:
            err = openmt.setMode(id);
            if (err == 0U)
                sendACK(typeByte);
            else
                sendNAK(typeByte, err);
            break;

        case OPENMT_ADD_MODE:
            err = openmt.addMode(payload, length);
            if (err == 0U)
                sendACK(typeByte);
            else
                sendNAK(typeByte, err);
            break;

        case OPENMT_DEL_MODE:
            err = openmt.delMode(id);
            if (err == 0U)
                sendACK(typeByte);
            else
                sendNAK(typeByte, err);
            break;

        case OPENMT_BIT_DATA:
            // Streaming data is not acknowledged, only rejected
            err = openmt.writeBits(id, payload, length);
            if (err != 0U)
                sendNAK(typeByte, err);
            break;

        case OPENMT_MEAS:
        {
            // A payload of pass and half asks for the register 7 diagnostic
            if (m_len >= 5U)
            {
                uint16_t words[8U];

                err = openmt.probe(id, m_buffer[3U], m_buffer[4U], words);
                if (err == 0U)
                    writeProbe(id, words);
                else
                    sendNAK(typeByte, err);
            }
            else
            {
                OpenMTMeas meas;

                err = openmt.measure(id, meas);
                if (err == 0U)
                    writeMeasurement(id, meas);
                else
                    sendNAK(typeByte, err);
            }
        }
        break;

        case OPENMT_BIT_EOT:
            // The transmitter drops by itself once the buffer drains
            break;

        default:
            // Sample streaming (SAMP_DATA etc.) is not supported by this
            // bit-stream firmware
            sendNAK(typeByte, OPENMT_ERR_UNSUPPORTED);
            break;
    }
}

void CSerialPort::writeDebugFrame(const char* text, const int32_t* values, uint8_t count, bool wide)
{
    uint8_t reply[130U];

    reply[0U] = OPENMT_FRAME_START;
    reply[1U] = 0U;
    reply[2U] = OPENMT_DEBUG;

    uint8_t pos = 3U;
    for (uint8_t i = 0U; text[i] != '\0' && pos < 100U; i++)
        reply[pos++] = text[i];

    for (uint8_t i = 0U; i < count; i++)
    {
        if (wide)
        {
            reply[pos++] = (uint8_t)(values[i] >> 24);
            reply[pos++] = (uint8_t)(values[i] >> 16);
        }
        reply[pos++] = (uint8_t)(values[i] >> 8);
        reply[pos++] = (uint8_t)(values[i] >> 0);
    }

    reply[1U] = pos;

    writeInt(1U, reply, pos, true);
}

#if defined(ENABLE_DEBUG)
void CSerialPort::writeDebug(const char* text)
{
    writeDebugFrame(text, NULL, 0U, false);
}

void CSerialPort::writeDebug(const char* text, int16_t n1)
{
    int32_t v[1U] = {n1};

    writeDebugFrame(text, v, 1U, false);
}

void CSerialPort::writeDebugI(const char* text, int32_t n1)
{
    int32_t v[1U] = {n1};

    writeDebugFrame(text, v, 1U, true);
}

void CSerialPort::writeDebug(const char* text, int16_t n1, int16_t n2, int16_t n3)
{
    int32_t v[3U] = {n1, n2, n3};

    writeDebugFrame(text, v, 3U, false);
}

void CSerialPort::writeDebug(const char* text, int16_t n1, int16_t n2, int16_t n3, int16_t n4)
{
    int32_t v[4U] = {n1, n2, n3, n4};

    writeDebugFrame(text, v, 4U, false);
}
#endif

void CSerialPort::writeDebug(const char* text, int16_t n1, int16_t n2)
{
    int32_t v[2U] = {n1, n2};

    writeDebugFrame(text, v, 2U, false);
}
