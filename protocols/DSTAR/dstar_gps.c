/***************************************************************************
 *   Copyright (C) 2025 by Rick KD0OSS                                     *
 *                                                                         *
 *   D-STAR GPS Parsing Module (C)                                         *
 *                                                                         *
 *   This module handles GPS string parsing for D-STAR slow-speed data    *
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

#include <CCITTChecksumReverse.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tools.h>

/* External variables from main dstar_host.c */
extern bool debugM;

/* GPS parsing implementation in pure C */
bool dstar_parse_gps(unsigned char c, char *tgps, int *gpsidx_ptr,
                     char *sGPSCall_out, float *fLat_out, float *fLong_out,
                     uint16_t *altitude_out) {
  int gpsidx = *gpsidx_ptr;

  if (c != 0x00) {
    tgps[gpsidx++] = c;
    *gpsidx_ptr = gpsidx;

    if ((c == '\n' || c == '\r' || gpsidx >= 200) && (gpsidx > 14)) {
      if (tgps[0] == '$' && tgps[1] == '$' && tgps[2] == 'C') {
        CCITTChecksumReverse cc;
        ccitt_checksum_init(&cc);
        ccitt_checksum_update_bytes(&cc, (const uint8_t *)tgps + 10,
                                    gpsidx - 10);
        unsigned char calccrc[2];
        ccitt_checksum_result_bytes(&cc, calccrc);
        char sum[5];
        sprintf(sum, "%02X%02X", calccrc[1], calccrc[0]);

        if (memcmp(sum, tgps + 5, 4) == 0) {
          /* Build clean GPS string without newlines */
          char sGps[256];
          int j = 0;
          for (int i = 0; i < gpsidx && j < 255; i++) {
            if (tgps[i] != '\n' && tgps[i] != '\r') {
              sGps[j++] = tgps[i];
            }
          }
          sGps[j] = '\0';

          if (debugM)
            fprintf(stderr, "GPS:%s\n", sGps);

          /* Parse callsign from fields[1] */
          StringArray fields;
          split_string(sGps, ',', &fields);
          if (fields.count > 1) {
            StringArray call_parts;
            split_string(fields.tokens[1], '>', &call_parts);
            if (call_parts.count > 0) {
              strncpy(sGPSCall_out, call_parts.tokens[0], 255);
              sGPSCall_out[255] = '\0';
            }
            string_array_free(&call_parts);
          }

          /* Check for different GPS format types */
          if (strstr(sGps, "DSTAR*:/") != NULL) {
            /* Position format with altitude */
            char delim = 'h'; /* default */
            StringArray slash_parts;
            split_string(sGps, '/', &slash_parts);
            if (slash_parts.count > 1 &&
                strstr(slash_parts.tokens[1], "z") != NULL) {
              delim = 'z';
            }
            string_array_free(&slash_parts);

            /* Parse latitude */
            float deg = 0.0;
            StringArray delim_fields;
            split_string(sGps, delim, &delim_fields);
            if (delim_fields.count > 1) {
              StringArray lat;
              split_string(delim_fields.tokens[1], '.', &lat);
              if (lat.count >= 2) {
                float fract = modff(atoi(lat.tokens[0]) / 100.0, &deg) * 100.0;
                char sTmp[3];
                sTmp[0] = lat.tokens[1][0];
                sTmp[1] = lat.tokens[1][1];
                sTmp[2] = '\0';
                float min = fract + atoi(sTmp) / 100.0;
                if (lat.tokens[1][2] == 'N')
                  *fLat_out = deg + (min / 60.0);
                else
                  *fLat_out = (deg + (min / 60.0)) * -1.0;
              }
              string_array_free(&lat);
            }
            string_array_free(&delim_fields);

            /* Parse longitude */
            split_string(sGps, '/', &slash_parts);
            if (slash_parts.count > 2) {
              StringArray lon;
              split_string(slash_parts.tokens[2], '.', &lon);
              if (lon.count >= 2) {
                float fract = modff(atoi(lon.tokens[0]) / 100.0, &deg) * 100.0;
                char sTmp[3];
                sTmp[0] = lon.tokens[1][0];
                sTmp[1] = lon.tokens[1][1];
                sTmp[2] = '\0';
                float min = fract + atoi(sTmp) / 100.0;
                if (lon.tokens[1][2] == 'W')
                  *fLong_out = (deg + (min / 60.0)) * -1.0;
                else
                  *fLong_out = deg + (min / 60.0);

                /* Check for altitude */
                if (strlen(lon.tokens[1]) > 3 && lon.tokens[1][3] == 'A') {
                  StringArray alt_parts;
                  split_string(lon.tokens[1], 'A', &alt_parts);
                  if (alt_parts.count > 1) {
                    *altitude_out = atoi(alt_parts.tokens[1]);
                  }
                  string_array_free(&alt_parts);
                } else {
                  *altitude_out = 0;
                }
              }
              string_array_free(&lon);
            }
            string_array_free(&slash_parts);
          } else if (strstr(sGps, "DSTAR*:!") != NULL) {
            /* Position format without altitude */
            float deg = 0.0;
            StringArray exc_fields;
            split_string(sGps, '!', &exc_fields);
            if (exc_fields.count > 1) {
              StringArray lat;
              split_string(exc_fields.tokens[1], '.', &lat);
              if (lat.count >= 2) {
                float fract = modff(atoi(lat.tokens[0]) / 100.0, &deg) * 100.0;
                char sTmp[3];
                sTmp[0] = lat.tokens[1][0];
                sTmp[1] = lat.tokens[1][1];
                sTmp[2] = '\0';
                float min = fract + atoi(sTmp) / 100.0;
                if (lat.tokens[1][2] == 'N')
                  *fLat_out = deg + (min / 60.0);
                else
                  *fLat_out = (deg + (min / 60.0)) * -1.0;
              }
              string_array_free(&lat);
            }
            string_array_free(&exc_fields);

            /* Parse longitude */
            StringArray slash_parts;
            split_string(sGps, '/', &slash_parts);
            if (slash_parts.count > 1) {
              StringArray lon;
              split_string(slash_parts.tokens[1], '.', &lon);
              if (lon.count >= 2) {
                float fract = modff(atoi(lon.tokens[0]) / 100.0, &deg) * 100.0;
                char sTmp[3];
                sTmp[0] = lon.tokens[1][0];
                sTmp[1] = lon.tokens[1][1];
                sTmp[2] = '\0';
                float min = fract + atoi(sTmp) / 100.0;
                if (lon.tokens[1][2] == 'W')
                  *fLong_out = (deg + (min / 60.0)) * -1.0;
                else
                  *fLong_out = deg + (min / 60.0);
              }
              string_array_free(&lon);
            }
            string_array_free(&slash_parts);
            *altitude_out = 0;
          } else if (strstr(sGps, "DSTAR*:;") != NULL) {
            /* Check for 'z' and 'I' markers */
            StringArray z_parts;
            split_string(sGps, 'z', &z_parts);
            if (z_parts.count > 1 && strstr(z_parts.tokens[1], "I") != NULL) {
              /* Object position format */
              float deg = 0.0;
              StringArray lat;
              split_string(z_parts.tokens[1], '.', &lat);
              if (lat.count >= 2) {
                float fract = modff(atoi(lat.tokens[0]) / 100.0, &deg) * 100.0;
                char sTmp[3];
                sTmp[0] = lat.tokens[1][0];
                sTmp[1] = lat.tokens[1][1];
                sTmp[2] = '\0';
                float min = fract + atoi(sTmp) / 100.0;
                if (lat.tokens[1][2] == 'N')
                  *fLat_out = deg + (min / 60.0);
                else
                  *fLat_out = (deg + (min / 60.0)) * -1.0;
              }
              string_array_free(&lat);

              /* Parse longitude */
              StringArray i_fields;
              split_string(sGps, 'I', &i_fields);
              if (i_fields.count > 2) {
                StringArray lon;
                split_string(i_fields.tokens[2], '.', &lon);
                if (lon.count >= 2) {
                  float fract =
                      modff(atoi(lon.tokens[0]) / 100.0, &deg) * 100.0;
                  char sTmp[3];
                  sTmp[0] = lon.tokens[1][0];
                  sTmp[1] = lon.tokens[1][1];
                  sTmp[2] = '\0';
                  float min = fract + atoi(sTmp) / 100.0;
                  if (lon.tokens[1][2] == 'W')
                    *fLong_out = (deg + (min / 60.0)) * -1.0;
                  else
                    *fLong_out = deg + (min / 60.0);

                  /* Check for altitude */
                  if (strlen(lon.tokens[1]) > 3 && lon.tokens[1][3] == 'A') {
                    StringArray alt_parts;
                    split_string(lon.tokens[1], 'A', &alt_parts);
                    if (alt_parts.count > 1) {
                      *altitude_out = atoi(alt_parts.tokens[1]);
                    }
                    string_array_free(&alt_parts);
                  } else {
                    *altitude_out = 0;
                  }
                }
                string_array_free(&lon);
              }
              string_array_free(&i_fields);
            }
            string_array_free(&z_parts);
          }

          string_array_free(&fields);
          *gpsidx_ptr = 0;
          return true;
        }
        *gpsidx_ptr = 0;
        return false;
      }
    }
    if (gpsidx >= 200) {
      *gpsidx_ptr = 0;
    }
    return false;
  }
  return false;
}
