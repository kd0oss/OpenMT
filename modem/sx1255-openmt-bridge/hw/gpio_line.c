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
/*
 * gpio_line.c - Linux GPIO v2 character-device output line control.
 */
#include "gpio_line.h"
#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/gpio.h>

bool gpio_line_request_output(gpio_line_t *line, const char *chip_path, unsigned int offset, int initial)
{
    struct gpio_v2_line_request req;

    memset(line, 0, sizeof(*line));
    line->chip_fd = -1;
    line->line_fd = -1;

    line->chip_fd = open(chip_path, O_RDWR | O_CLOEXEC);
    if (line->chip_fd < 0)
    {
        return false;
    }

    memset(&req, 0, sizeof(req));
    req.num_lines = 1;
    req.offsets[0] = offset;
    req.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
    req.config.num_attrs = 1;
    req.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
    req.config.attrs[0].attr.values = (uint64_t)(initial ? 1 : 0);
    req.config.attrs[0].mask = 1; /* applies to line index 0 */
    strncpy(req.consumer, "sx1255-bridge-reset", sizeof(req.consumer) - 1);

    if (ioctl(line->chip_fd, GPIO_V2_GET_LINE_IOCTL, &req) < 0)
    {
        int saved_errno = errno; /* close() below can otherwise clobber it,
                                   * giving the caller a misleading
                                   * strerror() message for what was
                                   * actually, say, EBUSY from the ioctl. */

        close(line->chip_fd);
        line->chip_fd = -1;
        errno = saved_errno;
        return false;
    }

    line->line_fd = req.fd;
    line->requested = true;
    return true;
}

bool gpio_line_set_value(gpio_line_t *line, int value)
{
    struct gpio_v2_line_values vals;

    if (!line->requested || line->line_fd < 0)
    {
        return false;
    }

    memset(&vals, 0, sizeof(vals));
    vals.mask = 1;
    vals.bits = (uint64_t)(value ? 1 : 0);

    return ioctl(line->line_fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &vals) == 0;
}

void gpio_line_close(gpio_line_t *line)
{
    if (line->line_fd >= 0)
    {
        close(line->line_fd);
        line->line_fd = -1;
    }
    if (line->chip_fd >= 0)
    {
        close(line->chip_fd);
        line->chip_fd = -1;
    }
    line->requested = false;
}
