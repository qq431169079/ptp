/* Copyright (c) 2017-2019 Foudil Brétel.  All rights reserved. */
#ifndef LOOKUP_H
#define LOOKUP_H

#include <stdlib.h>
#include <string.h>

/**
 * Good practice to use sthg_NONE as first enum element so we can later just
 * test (!lookup_by...).
 */
typedef struct {
    int         id;
    const char *name;
} lookup_entry;

static inline const char *lookup_by_id(const lookup_entry names[], const int id)
{
    while (names->name && names->id != id)
        names++;
    return names->name;
}

static inline int lookup_by_name(const lookup_entry names[], const char name[], size_t slen)
{
    while (names->name && strncmp(names->name, name, slen) != 0)
        names++;
    return names->id;
}

#endif /* LOOKUP_H */
