/** rnxcmp.c - RINEX observation file comparison utility.
 * Copyright 2020 Michael Poole.
 * SPDX-License-Identifier: MIT-Modern-Variant
 */

#include "bin/driver.h"
#include <errno.h>

static struct rinex_parser *p2;
static struct rinex_stream *s2;
static int had_diff;
static int full_mode;

/** next_data_epoch reads from p, skipping special records (flag != '0','1').
 * Returns 1 on a data epoch, 0 on EOF, negative on error.
 */
static int next_data_epoch(struct rinex_parser *p)
{
    int res;

    while ((res = p->read(p)) > 0)
    {
        if (p->epoch.flag == '0' || p->epoch.flag == '1')
        {
            return 1;
        }
        /* special record: skip */
    }
    return res;
}

/** found_mismatch exits immediately in non-full mode after a diff is found.
 * Otherwise it sets `had_diff` to 1.
 */
static void found_mismatch(void)
{
    if (!full_mode)
    {
        p2->destroy(p2);
        s2->destroy(s2);
        exit(EXIT_FAILURE);
    }
    had_diff = 1;
}

/** epoch_ts formats an epoch timestamp into buf (caller provides >= 32 bytes). */
static void epoch_ts(const struct rinex_epoch *e, char buf[32])
{
    snprintf(buf, 32, "%08d %04d %d.%07d",
        e->yyyy_mm_dd, e->hh_mm, e->sec_e7 / 10000000, e->sec_e7 % 10000000);
}

/** compare_epoch compares an epoch from p1 against one from p2.
 * Both parsers must already have a data epoch loaded.
 */
static void compare_epoch(struct rinex_parser *p1, struct rinex_parser *p2_,
    const char filename[])
{
    char ts[32];
    int ii, jj, sys_idx, n, diff;

    epoch_ts(&p1->epoch, ts);

    if (p1->epoch.flag != p2_->epoch.flag)
    {
        printf("%s: epoch %s flag '%c' vs '%c'\n",
            filename, ts, p1->epoch.flag, p2_->epoch.flag);
        found_mismatch();
    }

    if (p1->epoch.clock_offset != p2_->epoch.clock_offset)
    {
        printf("%s: epoch %s clock_offset %lld vs %lld\n",
            filename, ts,
            (long long)p1->epoch.clock_offset,
            (long long)p2_->epoch.clock_offset);
        found_mismatch();
    }

    if (p1->epoch.n_sats != p2_->epoch.n_sats)
    {
        printf("%s: epoch %s n_sats %d vs %d\n",
            filename, ts, p1->epoch.n_sats, p2_->epoch.n_sats);
        found_mismatch();
    }

    n = p1->epoch.n_sats < p2_->epoch.n_sats
        ? p1->epoch.n_sats : p2_->epoch.n_sats;

    for (ii = 0; ii < n; ++ii)
    {
        char sys1 = p1->sats[ii].system;
        char sys2 = p2_->sats[ii].system;
        int num1 = p1->sats[ii].number;
        int num2 = p2_->sats[ii].number;

        if (sys1 != sys2 || num1 != num2)
        {
            printf("%s: epoch %s sat[%d] %c%02d vs %c%02d\n",
                filename, ts, ii, sys1, num1, sys2, num2);
            found_mismatch();
            /* observation indices won't correspond; skip obs for this sat */
            continue;
        }

        sys_idx = sys1 & 31;
        /* n_obs counts per system should match if observation types match */
        if (p1->n_obs[sys_idx] != p2_->n_obs[sys_idx])
        {
            printf("%s: epoch %s sat %c%02d n_obs %d vs %d\n",
                filename, ts, sys1, num1,
                p1->n_obs[sys_idx], p2_->n_obs[sys_idx]);
            found_mismatch();
            continue;
        }

        diff = 0;
        for (jj = 0; jj < p1->n_obs[sys_idx]; ++jj)
        {
            int64_t o1 = p1->obs[p1->sats[ii].obs_0 + jj];
            int64_t o2 = p2_->obs[p2_->sats[ii].obs_0 + jj];
            char l1 = p1->lli[p1->sats[ii].obs_0 + jj];
            char l2 = p2_->lli[p2_->sats[ii].obs_0 + jj];
            char s1 = p1->ssi[p1->sats[ii].obs_0 + jj];
            char s2 = p2_->ssi[p2_->sats[ii].obs_0 + jj];

            if (o1 != o2 || l1 != l2 || s1 != s2)
            {
                if (!diff)
                {
                    printf("%s: epoch %s sat %c%02d obs differ:\n",
                        filename, ts, sys1, num1);
                    diff = 1;
                }
                printf("  obs[%d]: %lld(lli='\\%o',ssi='\\%o') vs %lld(lli='\\%o',ssi='\\%o')\n",
                    jj,
                    (long long)o1, (unsigned char)l1, (unsigned char)s1,
                    (long long)o2, (unsigned char)l2, (unsigned char)s2);
            }
        }
        if (diff)
        {
            found_mismatch();
        }
    }
}

void start(int *argc, char *argv[])
{
    const char *file2;
    const char *err;
    int ii;

    if (*argc < 3)
    {
        fprintf(stderr, "Usage: rnxcmp [--mmap|--stdio] [-v] [--full] file1 file2\n");
        exit(EXIT_FAILURE);
    }

    /* Scan for --full among non-file arguments (everything except the last). */
    for (ii = 1; ii < *argc - 1; ++ii)
    {
        if (!strcmp(argv[ii], "--full"))
        {
            full_mode = 1;
            memmove(&argv[ii], &argv[ii + 1],
                (*argc - ii - 1) * sizeof(argv[0]));
            --(*argc);
            break;
        }
    }

    file2 = argv[*argc - 1];
    --(*argc);

    s2 = !strcmp(file2, "-") ? rinex_stdin_stream()
        : rinex_stdio_stream(file2);
    if (!s2)
    {
        fprintf(stderr, "Unable to open %s: %s\n", file2, strerror(errno));
        exit(EXIT_FAILURE);
    }

    err = rinex_open(&p2, s2);
    if (err)
    {
        fprintf(stderr, "Unable to open %s as RINEX: %s\n", file2, err);
        s2->destroy(s2);
        exit(EXIT_FAILURE);
    }
}

void process_file(struct rinex_parser *p1, const char filename[])
{
    char ts[32];
    int r1, r2;

    for (;;)
    {
        r1 = next_data_epoch(p1);
        if (r1 < 0)
        {
            printf("Error reading file1 %s: %d (line %d)\n",
                filename, r1, p1->error_line);
            break;
        }
        if (r1 == 0)
        {
            break;
        }

        r2 = next_data_epoch(p2);
        if (r2 < 0)
        {
            epoch_ts(&p1->epoch, ts);
            printf("Error reading file2 at epoch %s: %d (line %d)\n",
                ts, r2, p2->error_line);
            found_mismatch();
            return;
        }
        if (r2 == 0)
        {
            epoch_ts(&p1->epoch, ts);
            printf("file2 ended before file1 at epoch %s\n", ts);
            found_mismatch();
            return;
        }

        /* Compare timestamps; if they differ, advance the earlier file. */
        while (r1 > 0 && r2 > 0)
        {
            int cmp = 0;

            if (p1->epoch.yyyy_mm_dd != p2->epoch.yyyy_mm_dd)
                cmp = p1->epoch.yyyy_mm_dd < p2->epoch.yyyy_mm_dd ? -1 : 1;
            else if (p1->epoch.hh_mm != p2->epoch.hh_mm)
                cmp = p1->epoch.hh_mm < p2->epoch.hh_mm ? -1 : 1;
            else if (p1->epoch.sec_e7 != p2->epoch.sec_e7)
                cmp = p1->epoch.sec_e7 < p2->epoch.sec_e7 ? -1 : 1;

            if (cmp < 0)
            {
                epoch_ts(&p1->epoch, ts);
                printf("%s: epoch %s only in file1\n", filename, ts);
                found_mismatch();
                r1 = next_data_epoch(p1);
                if (r1 < 0)
                {
                    printf("Error reading file1 %s: %d (line %d)\n",
                        filename, r1, p1->error_line);
                    return;
                }
            }
            else if (cmp > 0)
            {
                epoch_ts(&p2->epoch, ts);
                printf("%s: epoch %s only in file2\n", filename, ts);
                found_mismatch();
                r2 = next_data_epoch(p2);
                if (r2 < 0)
                {
                    printf("Error reading file2 at epoch %s: %d (line %d)\n",
                        ts, r2, p2->error_line);
                    return;
                }
                if (r2 == 0)
                {
                    epoch_ts(&p1->epoch, ts);
                    printf("file2 ended before file1 at epoch %s\n", ts);
                    found_mismatch();
                    return;
                }
            }
            else
            {
                compare_epoch(p1, p2, filename);
                break;
            }
        }
    }
}

void finish(void)
{
    char ts[32];
    int r2;

    /* Check for any remaining data epochs in file2. */
    r2 = next_data_epoch(p2);
    if (r2 > 0)
    {
        epoch_ts(&p2->epoch, ts);
        printf("file2 has extra epochs starting at %s\n", ts);
        found_mismatch();
    }
    else if (r2 < 0)
    {
        printf("Error reading file2 tail: %d (line %d)\n",
            r2, p2->error_line);
        found_mismatch();
    }

    p2->destroy(p2);
    s2->destroy(s2);

    if (had_diff)
    {
        exit(EXIT_FAILURE);
    }
}
