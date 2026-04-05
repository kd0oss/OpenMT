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
 ***************************************************************************/

#ifndef STRING_BUILDER_H
#define STRING_BUILDER_H

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 
 * Simple dynamic string builder for C
 * Replaces std::string functionality for building SQL queries
 */

typedef struct
{
    char *data;
    size_t length;
    size_t capacity;
} StringBuilder;

/* Initialize a new string builder */
void sb_init(StringBuilder *sb, size_t initial_capacity);

/* Append a C string to the builder */
void sb_append(StringBuilder *sb, const char *str);

/* Append a single character to the builder */
void sb_append_char(StringBuilder *sb, char c);

/* Get the current C string (null-terminated) */
const char* sb_cstr(const StringBuilder *sb);

/* Clear the string builder (keeps allocated memory) */
void sb_clear(StringBuilder *sb);

/* Free all memory associated with the string builder */
void sb_free(StringBuilder *sb);

/* Get the current length of the string */
size_t sb_length(const StringBuilder *sb);

/* String array for tokenization */
typedef struct
{
    char **tokens;
    size_t count;
    size_t capacity;
} StringArray;

/* Initialize a string array */
void string_array_init(StringArray *arr);

/* Add a token to the array */
void string_array_add(StringArray *arr, const char *token);

/* Free all memory in the array */
void string_array_free(StringArray *arr);

/* Split a string by delimiter into a StringArray */
void split_string(const char *input, char delimiter, StringArray *result);

#ifdef __cplusplus
}
#endif

#endif /* STRING_BUILDER_H */
