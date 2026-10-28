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
 *   Based on MMDVM, Jonathan Naylor G4KLX                                 *
 ***************************************************************************/

/*
 * The free space the modem reported for each mode, as set from its status
 * reply and read by the pacing code of Modem_Host.
 */

#include <stdint.h>
#include <stdbool.h>

#include "mmdvm.h"

static uint8_t dmr_space[2] = {0U, 0U};
static uint8_t p25_space    = 0U;
static uint8_t nxdn_space   = 0U;
static uint8_t ysf_space    = 0U;
static uint8_t dstar_space  = 0U;
static uint8_t m17_space    = 0U;

/*
 * getM17Space()
 *
 * Returns the number of free M17 frame slots the modem last reported.
 *
 * Parameters: none.
 *
 * Returns: Free frame slots in the modem M17 queue.
 */
uint8_t getM17Space()
{
    return m17_space;
}

/*
 * setM17Space()
 *
 * Records the number of free M17 frame slots the modem reported in its
 * status.
 *
 * Parameters:
 *   space - Free frame slots in the modem M17 queue.
 *
 * Returns: nothing.
 */
void setM17Space(uint8_t space)
{
    m17_space = space;
}

/*
 * getDSTARSpace()
 *
 * Returns the number of free D-Star frame slots the modem last reported.
 *
 * Parameters: none.
 *
 * Returns: Free frame slots in the modem D-Star queue.
 */
uint8_t getDSTARSpace()
{
    return dstar_space;
}

/*
 * setDSTARSpace()
 *
 * Records the number of free D-Star frame slots the modem reported in its
 * status.
 *
 * Parameters:
 *   space - Free frame slots in the modem D-Star queue.
 *
 * Returns: nothing.
 */
void setDSTARSpace(uint8_t space)
{
    dstar_space = space;
}

/*
 * getP25Space()
 *
 * Returns the number of free P25 frame slots the modem last reported.
 *
 * Parameters: none.
 *
 * Returns: Free frame slots in the modem P25 queue.
 */
uint8_t getP25Space()
{
    return p25_space;
}

/*
 * getYSFSpace()
 *
 * Returns the number of free YSF frame slots the modem last reported.
 *
 * Parameters: none.
 *
 * Returns: Free frame slots in the modem YSF queue.
 */
uint8_t getYSFSpace()
{
    return ysf_space;
}

/*
 * setYSFSpace()
 *
 * Records the number of free YSF frame slots the modem reported in its
 * status.
 *
 * Parameters:
 *   space - Free frame slots in the modem YSF queue.
 *
 * Returns: nothing.
 */
void setYSFSpace(uint8_t space)
{
    ysf_space = space;
}

/*
 * getNXDNSpace()
 *
 * Returns the number of free NXDN frame slots the modem last reported.
 *
 * Parameters: none.
 *
 * Returns: Free frame slots in the modem NXDN queue.
 */
uint8_t getNXDNSpace()
{
    return nxdn_space;
}

/*
 * setNXDNSpace()
 *
 * Records the number of free NXDN frame slots the modem reported in its
 * status.
 *
 * Parameters:
 *   space - Free frame slots in the modem NXDN queue.
 *
 * Returns: nothing.
 */
void setNXDNSpace(uint8_t space)
{
    nxdn_space = space;
}

/*
 * setP25Space()
 *
 * Records the number of free P25 frame slots the modem reported in its
 * status.
 *
 * Parameters:
 *   space - Free frame slots in the modem P25 queue.
 *
 * Returns: nothing.
 */
void setP25Space(uint8_t space)
{
    p25_space = space;
}

/*
 * getDMRSpace()
 *
 * Returns the number of free DMR frame slots the modem last reported for a
 * DMR slot.
 *
 * Parameters:
 *   slot - DMR slot number, 1 or 2.
 *
 * Returns: Free frame slots in the modem DMR slot queue.
 */
uint8_t getDMRSpace(uint8_t slot)
{
    return dmr_space[slot - 1];
}

/*
 * setDMRSpace()
 *
 * Records the number of free DMR frame slots the modem reported for each of
 * its two slots.
 *
 * Parameters:
 *   space1 - Free frame slots for DMR slot 1.
 *   space2 - Free frame slots for DMR slot 2.
 *
 * Returns: nothing.
 */
void setDMRSpace(uint8_t space1, uint8_t space2)
{
    dmr_space[0] = space1;
    dmr_space[1] = space2;
}

