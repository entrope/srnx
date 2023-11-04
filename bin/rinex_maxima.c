/** rinex_maxima.c - RINEX observation file scanning utility.
 * Copyright 2021 Michael Poole.
 * SPDX-License-Identifier: MIT-Modern-Variant
 */

#include "bin/driver.h"

void process_file(struct rinex_parser *p, const char filename[])
{
    unsigned char const_max[32];
    unsigned char const_sats[32];
    int count, max_sats, ii;
    const char *buffer;

    memset(const_max, 0, sizeof const_max);
    for (count = max_sats = 0; ; ++count)
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

        /* Ignore lines that do not include observations. */
        if (p->epoch.flag != '0' && p->epoch.flag != '1')
        {
            continue;
        }
        if (max_sats < p->epoch.n_sats)
        {
            max_sats = p->epoch.n_sats;
        }
        memset(const_sats, 0, sizeof const_sats);

        /* Allocate satellites to constellations. */
        buffer = p->buffer;
        for (ii = 0; ii < p->epoch.n_sats; ++ii)
        {
            int gnss = buffer[0] & 31;
            int sys_obs = p->n_obs[gnss];
            ++const_sats[gnss];
            buffer += 2 + ((sys_obs + 7) >> 3);
        }

        if (verbose)
        {
            for (ii = 0; ii < 32; ++ii)
            {
                if (const_max[ii] < const_sats[ii])
                {
                    printf("const_max['%c']: %d -> %d at %d:%d\n",
                        '@' + ii, const_max[ii], const_sats[ii],
                        p->epoch.hh_mm, p->epoch.sec_e7);
                    const_max[ii] = const_sats[ii];
                }
            }
        }
        else
        {
            for (ii = 0; ii < 32; ++ii)
            {
                if (const_max[ii] < const_sats[ii])
                    const_max[ii] = const_sats[ii];
            }
        }
    }

    printf("%s,%d,%d,%d,%d,%d,%d\n",
        filename, count, max_sats, const_max['G' & 31],
        const_max['E' & 31], const_max['R' & 31], const_max['C' & 31]);
}
