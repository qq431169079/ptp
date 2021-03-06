/* Copyright (c) 2017-2019 Foudil Brétel.  All rights reserved. */
#ifndef OPTIONS_H
#define OPTIONS_H

#include <limits.h>
#include <netdb.h>
#include "log.h"

struct config {
    char       conf_dir[PATH_MAX];
    char       bind_addr[NI_MAXHOST];
    char       bind_port[NI_MAXSERV];
    log_type_t log_type;
    int        log_level;
    size_t     max_peers;
};

extern const struct config CONFIG_DEFAULT;

int options_parse(struct config *conf, const int argc, char *const argv[]);

#endif /* OPTIONS_H */
