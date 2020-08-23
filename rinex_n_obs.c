/** rinex_n_obs.c - Count number of observation codes for RINEX files.
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

int s_count[128];
int hist[129];

void process_file(struct rinex_parser *p, const char filename[])
{
    int count, ii;
    char buf[8];

    if (!memcmp(p->buffer, "     2.", 7))
    {
        static const char n_types_obs[] = "# / TYPES OF OBSERV";
        const char *hdr;

        hdr = rinex_find_header(p, n_types_obs, sizeof n_types_obs);
        if (!hdr)
        {
            printf("%s: could not find %s header\n", filename, n_types_obs);
            return;
        }

        memcpy(buf, hdr, 6);
        buf[6] = '\0';
        count = strtol(buf, NULL, 10);
        ++hist[(count < 128) ? count : 128];
        if (s_count['2'] < count)
        {
            printf("%s: %d\n", filename, count);
            s_count['2'] = count;
        }
    }
    else if (!memcmp(p->buffer, "     3.", 7))
    {
        static const char sys_n_types_obs[] = "SYS / # / OBS TYPES";
        const char *hdr;

        hdr = rinex_find_header(p, sys_n_types_obs, sizeof sys_n_types_obs);
        if (!hdr)
        {
            printf("%s: could not find %s header\n", filename, sys_n_types_obs);
            return;
        }
        do
        {
            if (hdr[0] != ' ')
            {
                count = (hdr[3] - '0') * 100 + (hdr[4] - '0') * 10
                    + (hdr[5] - '0');
                ++hist[(count < 128) ? count : 128];
                ii = hdr[0] & 127;
                if (s_count[ii] < count)
                {
                    s_count[ii] = count;
                }
            }
            hdr = strchr(hdr, '\n') + 1;
        } while (!memcmp(hdr + 60, sys_n_types_obs, sizeof(sys_n_types_obs) - 1));
    }
    else
    {
        printf("%s: unrecognized RINEX version %.9s\n", filename, p->buffer);
    }
}

void finish(void)
{
    int ii;

    printf("Maxima: ");
    for (ii = '2'; ii < 'Z'; ++ii)
    {
        if (s_count[ii] > 0)
        {
            printf("%c: %d\n", (char)ii, s_count[ii]);
        }
    }
    printf("\n");

    printf("Histogram: [");
    for (ii = 0; ii < 128; ++ii)
    {
        printf(" %d", hist[ii]);
    }
    printf(" ]\n");
}
