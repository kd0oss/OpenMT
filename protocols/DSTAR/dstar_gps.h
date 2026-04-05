/***************************************************************************
 *   Copyright (C) 2025 by Rick KD0OSS                                     *
 *                                                                         *
 *   D-STAR GPS Parsing Module Header                                      *
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

#ifndef DSTAR_GPS_H
#define DSTAR_GPS_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Parse D-STAR GPS data from slow-speed data stream
 * 
 * Supports three APRS GPS formats:
 * - DSTAR*:/ - Position with altitude
 * - DSTAR*:! - Position without altitude
 * - DSTAR*:; - Object position
 * 
 * @param c Current byte from slow-speed data
 * @param gps_buffer Buffer to accumulate GPS data (200 bytes)
 * @param gps_idx Pointer to current index in buffer
 * @param gps_call_out Output: Extracted callsign (256 bytes)
 * @param lat_out Output: Latitude in decimal degrees
 * @param lon_out Output: Longitude in decimal degrees
 * @param alt_out Output: Altitude in feet
 * @return true if valid GPS packet was parsed, false otherwise
 */
bool dstar_parse_gps(unsigned char c,
                    char* gps_buffer,
                    int* gps_idx,
                    char* gps_call_out,
                    float* lat_out,
                    float* lon_out,
                    uint16_t* alt_out);

#ifdef __cplusplus
}
#endif

#endif /* DSTAR_GPS_H */
