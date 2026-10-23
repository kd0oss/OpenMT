/* Created by Rick KD0OSS */
/*
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

#include "ysf_reflist.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

/*
 * reflistInit()
 *
 * Clears a reflector list.  Use it before the first use; reflistFree()
 * releases what the list holds.
 *
 * Parameters:
 *   l - The list.
 *
 * Returns: nothing.
 */
void reflistInit(RefList* l)
{
    memset(l, 0x00, sizeof(*l));
}

/*
 * reflistFree()
 *
 * Frees the memory of the reflectors and FCS rooms of a list and empties it.
 *
 * Parameters:
 *   l - The list.
 *
 * Returns: nothing.
 */
void reflistFree(RefList* l)
{
    free(l->fcsRooms);
    l->fcsRooms    = NULL;
    l->fcsCount    = 0U;
    l->fcsCapacity = 0U;
    free(l->items);
    l->items    = NULL;
    l->count    = 0U;
    l->capacity = 0U;
}

/*
 * reflistAddFcsRoom()
 *
 * Adds an FCS room description to the list (the array grows when it is
 * full).  Nothing is added when memory runs out.
 *
 * Parameters:
 *   l    - The list.
 *   id   - The room name, such as FCS001-01.
 *   name - The description of the room.
 *
 * Returns: nothing.
 */
void reflistAddFcsRoom(RefList* l, const char* id, const char* name)
{
    if (l->fcsCount == l->fcsCapacity)
    {
        unsigned int cap = l->fcsCapacity == 0U ? 256U : l->fcsCapacity * 2U;
        struct FcsRoom* p = realloc(l->fcsRooms, cap * sizeof(*p));

        if (p == NULL)
            return;

        l->fcsRooms    = p;
        l->fcsCapacity = cap;
    }

    snprintf(l->fcsRooms[l->fcsCount].id, sizeof(l->fcsRooms[0].id), "%s", id);
    snprintf(l->fcsRooms[l->fcsCount].desc, sizeof(l->fcsRooms[0].desc), "%s", name);
    l->fcsCount++;
}

/*
 * reflistReadFcsRooms()
 *
 * Reads FCS room descriptions from a text file with the lines
 * "room;description".  Lines that start with # are skipped.  The rooms read
 * before are forgotten.
 *
 * Parameters:
 *   l    - The list.
 *   path - The file.
 *
 * Returns: The number of rooms read, 0 if the file cannot be opened.
 */
unsigned int reflistReadFcsRooms(RefList* l, const char* path)
{
    FILE* fp = fopen(path, "rt");
    char line[200];
    unsigned int count = 0U;

    if (fp == NULL)
        return 0U;

    l->fcsCount = 0U;

    while (fgets(line, sizeof(line), fp) != NULL)
    {
        char* p1;
        char* p2;
        char* save = NULL;

        if (line[0] == '#')
            continue;

        p1 = strtok_r(line, ";", &save);
        p2 = strtok_r(NULL, ";", &save);

        if (p1 != NULL && p2 != NULL)
        {
            reflistAddFcsRoom(l, p1, p2);
            count++;
        }
    }

    fclose(fp);

    return count;
}

/*
 * addItem()
 *
 * Makes room for one more reflector in the list (the array doubles when it
 * is full, starting at 256) and clears it.
 *
 * Parameters:
 *   l - The list.
 *
 * Returns: The new entry, or NULL when memory runs out.
 */
static Reflector* addItem(RefList* l)
{
    if (l->count == l->capacity)
    {
        unsigned int cap = l->capacity == 0U ? 256U : l->capacity * 2U;
        Reflector* p = realloc(l->items, cap * sizeof(Reflector));

        if (p == NULL)
            return NULL;

        l->items    = p;
        l->capacity = cap;
    }

    memset(&l->items[l->count], 0x00, sizeof(Reflector));

    return &l->items[l->count++];
}

/*
 * putField()
 *
 * Copies 'text' into a field of 'width' characters, padded with spaces, 0
 * terminated.
 *
 * Parameters:
 *   field - The field; width + 1 bytes.
 *   width - Size of the field, without the terminator.
 *   text  - The text.
 *   upper - true to convert the text to upper case.
 *
 * Returns: nothing.
 */
static void putField(char* field, unsigned int width, const char* text, bool upper)
{
    size_t len = strlen(text);

    if (len > width)
        len = width;

    memset(field, ' ', width);
    field[width] = '\0';

    for (size_t i = 0U; i < len; i++)
        field[i] = upper ? (char)toupper((unsigned char)text[i]) : text[i];
}

/*
 * putId()
 *
 * Writes the five digit reflector id: the number in the text, modulo 100000,
 * with leading zeroes.
 *
 * Parameters:
 *   field - Receives the id and a terminator; 6 bytes.
 *   text  - The number as text.
 *
 * Returns: nothing.
 */
static void putId(char* field, const char* text)
{
    unsigned long v = strtoul(text, NULL, 10);
    char tmp[16];

    snprintf(tmp, sizeof(tmp), "%05lu", v % 100000UL);
    memcpy(field, tmp, 6U);
}

/*
 * putCount()
 *
 * Writes the user count of a reflector as three digits, limited to 999.
 *
 * Parameters:
 *   field - Receives the count and a terminator; 4 bytes.
 *   text  - The number as text.
 *
 * Returns: nothing.
 */
static void putCount(char* field, const char* text)
{
    unsigned long v = strtoul(text, NULL, 10);

    if (v > 999UL)
        v = 999UL;

    snprintf(field, 4U, "%03lu", v);
}

/*
 * jsonMember()
 *
 * The text of a string or number member of a flat JSON object; false for
 * null or when it is not there.
 *
 * Parameters:
 *   obj     - The object as text.
 *   key     - The name of the member.
 *   out     - Receives the value as a string (escaped characters are copied
 *             without the backslash).
 *   outSize - Size of out, in bytes.
 *
 * Returns: true if the member was found and has a string, number or boolean
 *   value.
 */
static bool jsonMember(const char* obj, const char* key, char* out, size_t outSize)
{
    char pattern[48];
    const char* p;
    size_t n = 0U;

    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    p = strstr(obj, pattern);

    if (p == NULL)
        return false;

    p += strlen(pattern);

    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;

    if (*p != ':')
        return false;

    p++;

    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;

    if (*p == '"')
    {
        p++;

        while (*p != '\0' && *p != '"' && n + 1U < outSize)
        {
            if (*p == '\\' && p[1] != '\0')
                p++;
            out[n++] = *p++;
        }

        out[n] = '\0';
        return true;
    }

    if (isdigit((unsigned char)*p) || *p == '-' || strncmp(p, "true", 4) == 0 || strncmp(p, "false", 5) == 0)
    {
        while (*p != '\0' && *p != ',' && *p != '}' && !isspace((unsigned char)*p) && n + 1U < outSize)
            out[n++] = *p++;

        out[n] = '\0';
        return true;
    }

    return false;
}

/*
 * loadJson()
 *
 * Adds the reflectors of a JSON hosts file.  Every object with a designator,
 * name, ipv4 and port becomes a YSF reflector; names that begin with XLX are
 * marked as doing Wires-X.
 *
 * Parameters:
 *   l    - The list.
 *   text - The contents of the file.
 *
 * Returns: nothing.
 */
static void loadJson(RefList* l, const char* text)
{
    const char* p = text;

    while ((p = strstr(p, "\"designator\"")) != NULL)
    {
        const char* start = p;
        const char* end;
        char obj[2048];
        char designator[16], country[16], name[64], ipv4[64], portText[16], xx[8], desc[96], count[16];
        size_t len;
        Reflector* r;
        char full[96];

        while (start > text && *start != '{')
            start--;

        end = strchr(p, '}');

        if (end == NULL)
            break;

        len = (size_t)(end - start);

        if (len >= sizeof(obj))
        {
            p = end;
            continue;
        }

        memcpy(obj, start, len);
        obj[len] = '\0';
        p = end;

        designator[0] = country[0] = name[0] = ipv4[0] = portText[0] = xx[0] = desc[0] = count[0] = '\0';

        if (!jsonMember(obj, "designator", designator, sizeof(designator)) ||
            !jsonMember(obj, "name", name, sizeof(name)) ||
            !jsonMember(obj, "ipv4", ipv4, sizeof(ipv4)) ||
            !jsonMember(obj, "port", portText, sizeof(portText)))
            continue;

        if (atoi(portText) <= 0 || atoi(portText) > 65535 || !isdigit((unsigned char)designator[0]))
            continue;

        jsonMember(obj, "country", country, sizeof(country));
        jsonMember(obj, "use_xx_prefix", xx, sizeof(xx));
        jsonMember(obj, "user_count", count, sizeof(count));

        if (!jsonMember(obj, "description", desc, sizeof(desc)))
            snprintf(desc, sizeof(desc), "%s", name);

        r = addItem(l);

        if (r == NULL)
            return;

        r->useXX = strcmp(xx, "true") == 0;
        snprintf(full, sizeof(full), "%s-%s", r->useXX ? "XX" : country, name);

        putId(r->id, designator);
        putField(r->name, 16U, full, true);
        putField(r->desc, 14U, desc, true);
        putCount(r->count, count);
        r->type   = REFL_YSF;
        r->wiresX = strncmp(name, "XLX", 3) == 0;
        snprintf(r->address, sizeof(r->address), "%s", ipv4);
        r->port = (uint16_t)atoi(portText);
        snprintf(r->designator, sizeof(r->designator), "%s", designator);
        snprintf(r->country, sizeof(r->country), "%s", country);
        snprintf(r->rawName, sizeof(r->rawName), "%s", name);
    }
}

/*
 * loadText()
 *
 * Adds the reflectors of a text hosts file.  Each line is
 * "id;name;description;address;port;count"; lines that start with # or do
 * not fit are skipped.
 *
 * Parameters:
 *   l    - The list.
 *   text - The contents of the file.
 *
 * Returns: nothing.
 */
static void loadText(RefList* l, const char* text)
{
    const char* p = text;

    while (*p != '\0')
    {
        char line[256];
        char* f[6];
        size_t n = 0U;
        int count = 0;
        char* q;
        Reflector* r;

        while (*p != '\0' && *p != '\n' && n + 1U < sizeof(line))
            line[n++] = *p++;
        line[n] = '\0';

        while (*p != '\0' && *p != '\n')
            p++;
        if (*p == '\n')
            p++;

        while (n > 0U && (line[n - 1U] == '\r' || line[n - 1U] == ' '))
            line[--n] = '\0';

        if (line[0] == '#' || strchr(line, ';') == NULL)
            continue;

        q = line;
        f[count++] = q;

        while (count < 6 && (q = strchr(q, ';')) != NULL)
        {
            *q++ = '\0';
            f[count++] = q;
        }

        if (count < 5 || atoi(f[4]) <= 0 || atoi(f[4]) > 65535 || !isdigit((unsigned char)f[0][0]))
            continue;

        r = addItem(l);

        if (r == NULL)
            return;

        putId(r->id, f[0]);
        putField(r->name, 16U, f[1], true);
        putField(r->desc, 14U, f[2], true);
        putCount(r->count, count > 5 ? f[5] : "0");
        r->type = REFL_YSF;
        snprintf(r->address, sizeof(r->address), "%s", f[3]);
        r->port = (uint16_t)atoi(f[4]);
        snprintf(r->designator, sizeof(r->designator), "%s", f[0]);
        snprintf(r->rawName, sizeof(r->rawName), "%s", f[1]);
        r->country[0] = '\0';
    }
}

/*
 * compareName()
 *
 * Comparison for qsort(): sorts reflectors by name.
 *
 * Parameters:
 *   a - First reflector.
 *   b - Second reflector.
 *
 * Returns: A negative value, 0 or a positive value, as the name of a is
 *   before, equal to or after the name of b.
 */
static int compareName(const void* a, const void* b)
{
    const Reflector* r1 = (const Reflector*)a;
    const Reflector* r2 = (const Reflector*)b;

    return memcmp(r1->name, r2->name, 16U);
}

/*
 * comparePtr()
 *
 * Comparison for qsort(): sorts an array of reflector pointers by name.
 *
 * Parameters:
 *   a - Pointer to the first pointer.
 *   b - Pointer to the second pointer.
 *
 * Returns: A negative value, 0 or a positive value, as the name of the first
 *   is before, equal to or after the name of the second.
 */
static int comparePtr(const void* a, const void* b)
{
    const Reflector* r1 = *(const Reflector* const*)a;
    const Reflector* r2 = *(const Reflector* const*)b;

    return memcmp(r1->name, r2->name, 16U);
}

/*
 * idUsed()
 *
 * Tells if a reflector id is in use.
 *
 * Parameters:
 *   l  - The list.
 *   id - The id as a number.
 *
 * Returns: true if a reflector of the list has that id.
 */
static bool idUsed(const RefList* l, unsigned int id)
{
    char text[16];

    snprintf(text, sizeof(text), "%05u", id);

    for (unsigned int i = 0U; i < l->count; i++)
    {
        if (strcmp(l->items[i].id, text) == 0)
            return true;
    }

    return false;
}

/*
 * reflistLoad()
 *
 * Loads a hosts file (JSON or text, found by the first character), adds the
 * FCS rooms (they take the free ids from 00010 up, in the order of the file)
 * and sorts the list by name.
 *
 * Parameters:
 *   l    - The list.
 *   path - The file; larger than 8 MB is refused.
 *
 * Returns: true if the list has any entries, false if the file cannot be
 *   read or is empty.
 */
bool reflistLoad(RefList* l, const char* path)
{
    FILE* fp = fopen(path, "r");
    struct stat st;
    long size;
    char* text;
    const char* s;
    unsigned int id = 9U;

    if (fp == NULL)
        return false;

    if (fseek(fp, 0, SEEK_END) != 0 || (size = ftell(fp)) < 0 || size > 8L * 1024L * 1024L)
    {
        fclose(fp);
        return false;
    }

    rewind(fp);
    text = malloc((size_t)size + 1U);

    if (text == NULL)
    {
        fclose(fp);
        return false;
    }

    size = (long)fread(text, 1, (size_t)size, fp);
    text[size] = '\0';
    fclose(fp);

    l->count = 0U;

    s = text;
    while (isspace((unsigned char)*s))
        s++;

    if (*s == '{')
        loadJson(l, text);
    else
        loadText(l, text);

    free(text);

    snprintf(l->path, sizeof(l->path), "%s", path);
    l->mtime = (stat(path, &st) == 0) ? (long)st.st_mtime : 0L;

    /* The FCS rooms take the free ids from 00010 up, in the order of the file */
    for (unsigned int i = 0U; i < l->fcsCount; i++)
    {
        Reflector* r;
        char text2[16];

        do
        {
            id++;
        } while (idUsed(l, id));

        r = addItem(l);

        if (r == NULL)
            break;

        snprintf(text2, sizeof(text2), "%05u", id);
        memcpy(r->id, text2, 6U);
        putField(r->name, 16U, l->fcsRooms[i].id, true);
        putField(r->desc, 14U, l->fcsRooms[i].desc, true);
        memcpy(r->count, "000", 4U);
        r->type = REFL_FCS;
        snprintf(r->designator, sizeof(r->designator), "%s", r->id);
        snprintf(r->rawName, sizeof(r->rawName), "%s", l->fcsRooms[i].id);
    }

    if (l->count == 0U)
        return false;

    qsort(l->items, l->count, sizeof(Reflector), compareName);

    return true;
}

/*
 * reflistRefresh()
 *
 * Reloads the list when its file has changed.
 *
 * Parameters:
 *   l - The list.
 *
 * Returns: true if the list was loaded again.
 */
bool reflistRefresh(RefList* l)
{
    struct stat st;

    if (l->path[0] == '\0' || stat(l->path, &st) != 0 || (long)st.st_mtime == l->mtime)
        return false;

    return reflistLoad(l, l->path);
}

/*
 * reflistFindById()
 *
 * Finds a reflector by its five digit id.
 *
 * Parameters:
 *   l  - The list.
 *   id - The five digit id.
 *
 * Returns: The reflector, or NULL if there is none with that id.
 */
Reflector* reflistFindById(RefList* l, const char* id)
{
    for (unsigned int i = 0U; i < l->count; i++)
    {
        if (strncmp(l->items[i].id, id, 5U) == 0)
            return &l->items[i];
    }

    return NULL;
}

/*
 * reflistFindByName()
 *
 * Finds a reflector by its name as it is in the list (16 characters, upper
 * case).
 *
 * Parameters:
 *   l    - The list.
 *   name - The name (not case sensitive).
 *
 * Returns: The reflector, or NULL if there is none with that name.
 */
Reflector* reflistFindByName(RefList* l, const char* name)
{
    char full[17];

    putField(full, 16U, name, true);

    for (unsigned int i = 0U; i < l->count; i++)
    {
        if (memcmp(l->items[i].name, full, 16U) == 0)
            return &l->items[i];
    }

    return NULL;
}

/*
 * reflistFind()
 *
 * Finds a reflector by a number (the id), by its list name or by the name
 * given in the file.  Trailing spaces are ignored.
 *
 * Parameters:
 *   l   - The list.
 *   key - The id or name.
 *
 * Returns: The reflector, or NULL if it is not found.
 */
Reflector* reflistFind(RefList* l, const char* key)
{
    char trimmed[64];
    size_t n;
    Reflector* r;

    snprintf(trimmed, sizeof(trimmed), "%s", key);
    n = strlen(trimmed);

    while (n > 0U && isspace((unsigned char)trimmed[n - 1U]))
        trimmed[--n] = '\0';

    if (n == 0U)
        return NULL;

    if (isdigit((unsigned char)trimmed[0]))
    {
        bool digits = true;

        for (size_t i = 0U; i < n; i++)
            digits = digits && isdigit((unsigned char)trimmed[i]);

        if (digits)
        {
            char id[16];

            snprintf(id, sizeof(id), "%05lu", strtoul(trimmed, NULL, 10) % 100000UL);
            r = reflistFindById(l, id);

            if (r != NULL)
                return r;
        }
    }

    r = reflistFindByName(l, trimmed);

    if (r != NULL)
        return r;

    for (unsigned int i = 0U; i < l->count; i++)
    {
        if (l->items[i].type == REFL_YSF && l->items[i].rawName[0] != '\0' && strcasecmp(l->items[i].rawName, trimmed) == 0)
            return &l->items[i];
    }

    return NULL;
}

/*
 * reflistSearch()
 *
 * Searches the names of the reflectors for a text (the whole name is
 * searched, as YSFReflectors does), and sorts the found ones by name.
 *
 * Parameters:
 *   l    - The list.
 *   text - The text to look for, not case sensitive.
 *   out  - Receives pointers to the reflectors found.
 *   max  - Size of out.
 *
 * Returns: The number of reflectors found.
 */
unsigned int reflistSearch(RefList* l, const char* text, const Reflector** out, unsigned int max)
{
    char needle[64];
    size_t n;
    unsigned int found = 0U;

    snprintf(needle, sizeof(needle), "%s", text);
    n = strlen(needle);

    while (n > 0U && isspace((unsigned char)needle[n - 1U]))
        needle[--n] = '\0';

    for (size_t i = 0U; i < n; i++)
        needle[i] = (char)toupper((unsigned char)needle[i]);

    for (unsigned int i = 0U; i < l->count && found < max; i++)
    {
        char name[17];
        size_t len;

        memcpy(name, l->items[i].name, 17U);
        len = strlen(name);

        while (len > 0U && isspace((unsigned char)name[len - 1U]))
            name[--len] = '\0';

        /* the whole name is searched, as YSFReflectors does */
        for (size_t pos = 0U; pos < len; pos++)
        {
            if (strncmp(name + pos, needle, n) == 0 && (len - pos) >= n)
            {
                out[found++] = &l->items[i];
                break;
            }
        }
    }

    qsort((void*)out, found, sizeof(out[0]), comparePtr);

    return found;
}
