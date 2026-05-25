/** rnx_load.c - Load-and-free wrapper for profiling.
 * Copyright 2024 Michael Poole.
 * SPDX-License-Identifier: MIT-Modern-Variant
 */

#include "rinex/rinex_load.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char *argv[])
{
    struct rinex_data data;
    const char *err;
    int ii;

    if (argc < 2)
    {
        fprintf(stderr, "Usage: %s <file> [file ...]\n", argv[0]);
        return EXIT_FAILURE;
    }

    for (ii = 1; ii < argc; ++ii)
    {
        data.file_header = NULL;
        data.epoch = NULL;
        data.sv = NULL;
        data.obs = NULL;
        data.ssi = NULL;
        data.lli = NULL;
        data.event = NULL;

        err = rinex_load_file(argv[ii], &data);
        if (err)
        {
            fprintf(stderr, "%s: %s\n", argv[ii], err);
            free_rinex_data(&data);
            continue;
        }

        free_rinex_data(&data);
    }

    return EXIT_SUCCESS;
}
