/* Copyright (c) 2017 Foudil Brétel.  All rights reserved. */
#include <stdio.h>
#include "log.h"
#include "options.h"
#include "server.h"
#include "signals.h"

int main(int argc, char *argv[])
{
    if (!sig_install()) {
        fprintf(stderr, "Could not install signals. Aborting.\n");
        return EXIT_FAILURE;
    }

    struct config conf = CONFIG_DEFAULT;
    int rv = options_parse(&conf, argc, argv);
    if (rv < 2)
        return rv;

    if (!log_init(conf.logtype, conf.loglevel)) {
        fprintf(stderr, "Could not setup logging. Aborting.\n");
        return EXIT_FAILURE;
    }

    log_info("Using config directory: %s", conf.conf_dir);

    server_run(&conf);

    log_shutdown(conf.logtype);

    return EXIT_SUCCESS;
}
