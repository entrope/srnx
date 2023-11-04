/** rinex_scan.c - RINEX observation file scanning utility.
 * Copyright 2020 Michael Poole.
 * SPDX-License-Identifier: MIT-Modern-Variant
 */

#include "bin/driver.h"

void process_file(struct rinex_parser *p, const char filename[])
{
    static uint64_t sat_obs[100 * 32];
    uint64_t tmp_sat_obs;
    int count, max_obs, tot_obs, max_sats, ii, jj, n_obs, sys_obs;
    int sys_idx, sat_idx, act_obs, sat_ofs;

    memset(sat_obs, 0, sizeof(sat_obs));
    for (count = max_obs = max_sats = tot_obs = 0; ; ++count)
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

        for (ii = n_obs = 0; ii < p->epoch.n_sats; ++ii)
        {
            sys_idx = p->sats[ii].system & 31;
            sat_idx = p->sats[ii].number + sys_idx;
            sat_ofs = p->sats[ii].obs_0;
            tmp_sat_obs = sat_obs[sat_idx];
            sys_obs = p->n_obs[sys_idx];
            for (jj = 0; jj < sys_obs; ++jj)
            {
                int non_zero = (p->obs[sat_ofs + jj] != 0);
                tmp_sat_obs |= (uint64_t)non_zero << jj;
                n_obs += non_zero;
            }
            sat_obs[sat_idx] = tmp_sat_obs;
        }

        tot_obs += n_obs;
        if (max_obs < n_obs)
        {
            max_obs = n_obs;
        }

        if (verbose)
        {
            printf("%08d %04d %9d %2d %3d\n",
                p->epoch.yyyy_mm_dd, p->epoch.hh_mm, p->epoch.sec_e7,
                p->epoch.n_sats, n_obs);
        }
    }

    act_obs = 0;
    for (ii = 0; ii < 100 * 32; ++ii)
    {
        act_obs += __builtin_popcountll(sat_obs[ii]);
    }

    printf("%s,%d,%d,%d,%d,%d,%d\n", filename,
        count, max_obs, max_sats, p->n_obs[0], act_obs, tot_obs);
}

void start(void)
{
    printf("filename,epochs,maxobs,maxsats,sysobs,satobs,totobs\n");
}
