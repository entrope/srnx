/** driver.c - RINEX file processing driver.
 * Copyright 2020 Michael Poole.
 * SPDX-License-Identifier: MIT-Modern-Variant
 */

#include "bin/driver.h"
#include <errno.h>
#include <wordexp.h>

int verbose;

__attribute__((weak))
void start(void)
{
    /* this function intentionally left blank */
}

__attribute__((weak))
void finish(void)
{
    /* this function intentionally left blank */
}

__attribute__((weak))
int main(int argc, char *argv[])
{
    wordexp_t we;
    const char *filename;
    struct rinex_stream *s = NULL;
    struct rinex_parser *p = NULL;
    const char *err;
    int ii, use_mmap;
    int res;

    start();

    we.we_wordc = 0;
    we.we_wordv = NULL;
    we.we_offs = 300;
    if (argc == 1)
    {
        res = wordexp("2020_200/m*.20o", &we, WRDE_NOCMD);
        if (res != 0)
        {
            return EXIT_FAILURE;
        }
        argv = we.we_wordv - 1;
        argc = we.we_wordc + 1;
    }

    for (ii = 1, use_mmap = 1; ii < argc; ++ii)
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

    if (we.we_wordv)
    {
        wordfree(&we);
    }

    finish();

    return EXIT_SUCCESS;
}
