/** driver.c - RINEX file processing driver.
 * Copyright 2020 Michael Poole.
 * SPDX-License-Identifier: MIT-Modern-Variant
 */

#include "bin/driver.h"
#include <errno.h>

int verbose;

__attribute__((weak))
void start(int *argc, char *argv[])
{
    (void)argc;
    (void)argv;
}

__attribute__((weak))
void finish(void)
{
    /* this function intentionally left blank */
}

static void usage(FILE *out, const char *argv0)
{
    fprintf(out, "Usage: %s [--mmap|--stdio] [-v] [--help] <file|-> ...\n",
        argv0);
}

__attribute__((weak))
int main(int argc, char *argv[])
{
    const char *filename;
    struct rinex_stream *s = NULL;
    struct rinex_parser *p = NULL;
    const char *err;
    int ii, use_mmap;

    if (argc == 1)
    {
        usage(stderr, argv[0]);
        return EXIT_FAILURE;
    }
    for (ii = 1; ii < argc; ++ii)
    {
        if (!strcmp(argv[ii], "--help"))
        {
            usage(stdout, argv[0]);
            return EXIT_SUCCESS;
        }
    }

    start(&argc, argv);

    for (ii = 1, use_mmap = 0; ii < argc; ++ii)
    {
        if (!strcmp(argv[ii], "--mmap"))
        {
            use_mmap = 1;
            continue;
        }
        if (!strcmp(argv[ii], "--stdio"))
        {
            use_mmap = 0;
            continue;
        }
        if (!strcmp(argv[ii], "-v"))
        {
            verbose = 1;
            continue;
        }

        filename = argv[ii];
        s = !strcmp(filename, "-") ? rinex_stdin_stream()
            : use_mmap ? rinex_mmap_stream(filename)
            : rinex_stdio_stream(filename);
        if (!s)
        {
            printf("Unable to open %s: %s\n", filename, strerror(errno));
            continue;
        }

        err = rinex_open(&p, s);
        if (err)
        {
            printf("Unable to rinex_open %s: %s\n", filename, err);
            s->destroy(s);
            continue;
        }

        process_file(p, filename);
        s->destroy(s);
    }

    if (p)
    {
        p->destroy(p);
    }

    finish();

    return EXIT_SUCCESS;
}
