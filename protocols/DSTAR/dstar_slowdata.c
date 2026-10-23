/***************************************************************************
 *   Copyright (C) 2025 by Rick KD0OSS                                     *
 *                                                                         *
 *   D-STAR slow data encoder, shared by the host and the gateway          *
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
 ***************************************************************************/

/* Created by Rick KD0OSS */

#include "dstar_slowdata.h"

#include <string.h>

/*
 * slowSpeedDataEncode()
 *
 * Builds the three slow data bytes for the next voice frame from a text
 * message.
 *
 * Parameters:
 *   cMessage - Text message to send.
 *   ucBytes  - Receives the three slow data bytes.
 *   ucMode   - Frame mode; 10 restarts the frame counter.
 *
 * Returns: nothing.
 */
void slowSpeedDataEncode(char* cMessage, unsigned char* ucBytes, unsigned char ucMode)
{
    static int iIndex;
    static unsigned char ucFrameCount;

    if (ucMode == 10)
    {
        ucFrameCount = 0;
        return;
    }

    int iSyncfrm = ucFrameCount % 21;

    if (ucFrameCount > 251)
        ucFrameCount = 0;
    else
        ucFrameCount++;

    if (iSyncfrm == 0)
    {
        ucBytes[0] = 0x25 ^ 0x70;
        ucBytes[1] = 0x62 ^ 0x4f;
        ucBytes[2] = 0x85 ^ 0x93;
        iIndex     = 0;
        return;
    }

    if (ucMode == 0)
    {
        char slowMessage[40];
        memset(slowMessage, 0x20, 39);

        if (iIndex <= 36)
        {
            int iSection = iIndex % 5;
            if (iSection == 0)
            {
                if (iIndex == 36)
                    ucBytes[0] = 0x51;
                else
                    ucBytes[0] = 0x55;
                ucBytes[1] = (unsigned char)slowMessage[iIndex++];
                ucBytes[2] = (unsigned char)slowMessage[iIndex++];
            }
            else
            {
                ucBytes[0] = (unsigned char)slowMessage[iIndex++];
                ucBytes[1] = (unsigned char)slowMessage[iIndex++];
                ucBytes[2] = (unsigned char)slowMessage[iIndex++];
            }
        }
        else
        {
            ucBytes[0] = 0x66;
            ucBytes[1] = 0x66;
            ucBytes[2] = 0x66;
        }
    }

    if (ucMode == 1)
    {
        if (iIndex <= 17)
        {
            int iSection = iIndex % 5;
            if (iSection == 0)
            {
                ucBytes[0] = (unsigned char)((0x40 + iIndex / 5) ^ 0x70);
                ucBytes[1] = (unsigned char)cMessage[iIndex++] ^ 0x4f;
                ucBytes[2] = (unsigned char)cMessage[iIndex++] ^ 0x93;
            }
            else
            {
                ucBytes[0] = (unsigned char)cMessage[iIndex++] ^ 0x70;
                ucBytes[1] = (unsigned char)cMessage[iIndex++] ^ 0x4f;
                ucBytes[2] = (unsigned char)cMessage[iIndex++] ^ 0x93;
            }
        }
        else
        {
            ucBytes[0] = 0x66 ^ 0x70;
            ucBytes[1] = 0x66 ^ 0x4f;
            ucBytes[2] = 0x66 ^ 0x93;
        }
    }
}
