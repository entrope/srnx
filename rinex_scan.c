/** rinex_scan.c - RINEX scanning utility.
 * Copyright 2020 Michael Poole.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "rinex.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char *argv[])
{
    const char *filename;
    struct rinex_stream *s;
    struct rinex_parser *p = NULL;
    enum rinex_error err;
    int ii, res, count, max_sigs, use_mmap;

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

        filename = argv[ii];
        s = use_mmap
            ? rinex_mmap_stream(filename)
            : rinex_stdio_stream(filename);
        if (!s)
        {
            printf("Unable to mmap %s: %s\n", filename, strerror(errno));
            continue;
        }

        err = rinex_open(&p, s);
        if (err != RINEX_SUCCESS)
        {
            printf("Unable to open %s: %d\n", filename, err);
            continue;
        }

        for (count = max_sigs = 0; ; ++count)
        {
            res = p->read(p);
            if (res <= 0)
            {
                if (res < 0)
                {
                    printf("Error parsing %s: %d\n", filename, res);
                }
                break;
            }
            if (max_sigs < p->signal_len)
            {
                max_sigs = p->signal_len;
            }
        }

        printf("%s: %d records, max %d signals\n", filename, count, max_sigs);
    }

    if (p)
    {
        p->destroy(p);
    }

    return EXIT_SUCCESS;
}
