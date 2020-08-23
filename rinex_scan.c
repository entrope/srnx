/** rinex_scan.c - RINEX observation file scanning utility.
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

#include "driver.h"
#include <stdio.h>

void process_file(struct rinex_parser *p, const char filename[])
{
    int count, max_sigs;

    for (count = max_sigs = 0; ; ++count)
    {
        int res = p->read(p);
        if (res <= 0)
        {
            if (res < 0)
            {
                printf("Error parsing %s: %d (line %d)\n", filename,
                    res, p->error_line);
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
