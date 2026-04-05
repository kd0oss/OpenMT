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

#include "string_builder.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define SB_MIN_CAPACITY 256

void sb_init(StringBuilder *sb, size_t initial_capacity)
{
    if (initial_capacity < SB_MIN_CAPACITY)
    {
        initial_capacity = SB_MIN_CAPACITY;
    }
    
    sb->data = (char *)malloc(initial_capacity);
    if (sb->data == NULL)
    {
        fprintf(stderr, "sb_init: malloc failed\n");
        exit(1);
    }
    
    sb->data[0] = '\0';
    sb->length = 0;
    sb->capacity = initial_capacity;
}

static void sb_ensure_capacity(StringBuilder *sb, size_t additional)
{
    size_t required = sb->length + additional + 1; /* +1 for null terminator */
    
    if (required > sb->capacity)
    {
        size_t new_capacity = sb->capacity * 2;
        while (new_capacity < required)
        {
            new_capacity *= 2;
        }
        
        char *new_data = (char *)realloc(sb->data, new_capacity);
        if (new_data == NULL)
        {
            fprintf(stderr, "sb_ensure_capacity: realloc failed\n");
            exit(1);
        }
        
        sb->data = new_data;
        sb->capacity = new_capacity;
    }
}

void sb_append(StringBuilder *sb, const char *str)
{
    if (str == NULL)
    {
        return;
    }
    
    size_t str_len = strlen(str);
    if (str_len == 0)
    {
        return;
    }
    
    sb_ensure_capacity(sb, str_len);
    
    memcpy(sb->data + sb->length, str, str_len);
    sb->length += str_len;
    sb->data[sb->length] = '\0';
}

void sb_append_char(StringBuilder *sb, char c)
{
    sb_ensure_capacity(sb, 1);
    
    sb->data[sb->length] = c;
    sb->length++;
    sb->data[sb->length] = '\0';
}

const char* sb_cstr(const StringBuilder *sb)
{
    return sb->data;
}

void sb_clear(StringBuilder *sb)
{
    sb->length = 0;
    if (sb->data != NULL)
    {
        sb->data[0] = '\0';
    }
}

void sb_free(StringBuilder *sb)
{
    if (sb->data != NULL)
    {
        free(sb->data);
        sb->data = NULL;
    }
    sb->length = 0;
    sb->capacity = 0;
}

size_t sb_length(const StringBuilder *sb)
{
    return sb->length;
}

void string_array_init(StringArray *arr)
{
    arr->tokens = NULL;
    arr->count = 0;
    arr->capacity = 0;
}

void string_array_add(StringArray *arr, const char *token)
{
    if (arr->count >= arr->capacity)
    {
        size_t new_capacity = arr->capacity == 0 ? 8 : arr->capacity * 2;
        char **new_tokens = (char **)realloc(arr->tokens, new_capacity * sizeof(char *));
        if (new_tokens == NULL)
        {
            fprintf(stderr, "string_array_add: realloc failed\n");
            exit(1);
        }
        arr->tokens = new_tokens;
        arr->capacity = new_capacity;
    }
    
    arr->tokens[arr->count] = (char *)malloc(strlen(token) + 1);
    if (arr->tokens[arr->count] == NULL)
    {
        fprintf(stderr, "string_array_add: malloc failed\n");
        exit(1);
    }
    strcpy(arr->tokens[arr->count], token);
    arr->count++;
}

void string_array_free(StringArray *arr)
{
    size_t i;
    for (i = 0; i < arr->count; i++)
    {
        if (arr->tokens[i] != NULL)
        {
            free(arr->tokens[i]);
        }
    }
    if (arr->tokens != NULL)
    {
        free(arr->tokens);
    }
    arr->tokens = NULL;
    arr->count = 0;
    arr->capacity = 0;
}

void split_string(const char *input, char delimiter, StringArray *result)
{
    char *input_copy;
    char *token_start;
    char *current;
    char token_buffer[1024];
    size_t token_len;
    
    if (input == NULL || result == NULL)
    {
        return;
    }
    
    string_array_init(result);
    
    input_copy = (char *)malloc(strlen(input) + 1);
    if (input_copy == NULL)
    {
        fprintf(stderr, "split_string: malloc failed\n");
        exit(1);
    }
    strcpy(input_copy, input);
    
    token_start = input_copy;
    current = input_copy;
    
    while (*current != '\0')
    {
        if (*current == delimiter)
        {
            token_len = current - token_start;
            if (token_len > 0 && token_len < sizeof(token_buffer))
            {
                strncpy(token_buffer, token_start, token_len);
                token_buffer[token_len] = '\0';
                string_array_add(result, token_buffer);
            }
            token_start = current + 1;
        }
        current++;
    }
    
    /* Add the last token */
    token_len = current - token_start;
    if (token_len > 0 && token_len < sizeof(token_buffer))
    {
        strncpy(token_buffer, token_start, token_len);
        token_buffer[token_len] = '\0';
        string_array_add(result, token_buffer);
    }
    
    free(input_copy);
}
