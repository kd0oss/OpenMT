/***************************************************************************
 *   Copyright (C) 2026 by Rick KD0OSS                                     *
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
 ***************************************************************************/

#ifndef GPIO_LINE_H
#define GPIO_LINE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct
{
    int chip_fd;
    int line_fd;
    bool requested;
} gpio_line_t;

/* Request "offset" on gpio device "chip_path" (e.g. "/dev/gpiochip0") as an
 * output line, initial value "initial" (0 or 1). Returns true on success. */
bool gpio_line_request_output(gpio_line_t *line, const char *chip_path, unsigned int offset, int initial);

/* Set the line's output value (0 or 1). Returns true on success. */
bool gpio_line_set_value(gpio_line_t *line, int value);

/* Release the line and close file descriptors. */
void gpio_line_close(gpio_line_t *line);

#ifdef __cplusplus
}
#endif

#endif /* GPIO_LINE_H */
