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

#ifndef YSF_REFLIST_H
#define YSF_REFLIST_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The reflector list as Wires-X shows it: id (5), name (16, upper case),
 * description (14) and user count (3), padded with spaces as YSFReflectors does.
 * Entries come from YSFHosts.json (hostfiles.refcheck.radio) or the older
 * "id;name;description;address;port;count" text file, and the FCS rooms of
 * FCSRooms.txt are added with ids from 00010 up.
 */
typedef enum
{
    REFL_YSF = 0,
    REFL_FCS
} ReflType;

typedef struct
{
    char id[6];             /* 5 digits                                    */
    char name[17];          /* 16 characters, upper case, space padded     */
    char desc[15];          /* 14 characters                               */
    char count[4];          /* 3 digits                                    */
    ReflType type;
    bool wiresX;            /* XLX reflector: Wires-X goes to the network  */
    char address[64];       /* YSF only                                    */
    uint16_t port;
    char designator[16];    /* as in the file                              */
    char country[16];
    char rawName[64];       /* name without the country                    */
    bool useXX;
} Reflector;

typedef struct
{
    Reflector* items;
    unsigned int count;
    unsigned int capacity;
    struct FcsRoom
    {
        char id[12];        /* FCS rooms to add on every load             */
        char desc[32];
    } *fcsRooms;
    unsigned int fcsCount;
    unsigned int fcsCapacity;
    char path[256];         /* the file the list was read from            */
    long mtime;
} RefList;

void reflistInit(RefList* l);
void reflistFree(RefList* l);

void reflistAddFcsRoom(RefList* l, const char* id, const char* name);

/* Reads a FCSRooms.txt ("FCS00100;Repeater;..." lines); returns the number of rooms */
unsigned int reflistReadFcsRooms(RefList* l, const char* path);

/* Replaces the list with the contents of 'path' (JSON or text) plus the FCS rooms; false when nothing was found */
bool reflistLoad(RefList* l, const char* path);

/* Reloads the file when it has changed since it was read */
bool reflistRefresh(RefList* l);

/* Id as five digits, name in any case; NULL when not found */
Reflector* reflistFindById(RefList* l, const char* id);
Reflector* reflistFindByName(RefList* l, const char* name);

/* A reflector by what a person might type: id (00001 or 1), name, country-name or the name without the country */
Reflector* reflistFind(RefList* l, const char* key);

/* Sub string search of the names (case insensitive), sorted by name; returns the number found (up to 'max') */
unsigned int reflistSearch(RefList* l, const char* text, const Reflector** out, unsigned int max);

#ifdef __cplusplus
}
#endif

#endif
