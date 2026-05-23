/** rnxcmp.c - RINEX observation file comparison utility.
 * Copyright 2020 Michael Poole.
 * SPDX-License-Identifier: MIT-Modern-Variant
 */

#include "rinex/rinex_load.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct rinex_data data1, data2;
static int had_diff;
static int full_mode;
static const char *file1_name, *file2_name;

/** found_mismatch exits immediately in non-full mode after a diff is found. */
static void found_mismatch(void)
{
    if (!full_mode)
    {
        free_rinex_data(&data1);
        free_rinex_data(&data2);
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

/** Compare two epochs by timestamp.  Returns -1, 0, or 1. */
static int epoch_cmp(const struct rinex_epoch *a, const struct rinex_epoch *b)
{
    if (a->yyyy_mm_dd != b->yyyy_mm_dd)
        return a->yyyy_mm_dd < b->yyyy_mm_dd ? -1 : 1;
    if (a->hh_mm != b->hh_mm)
        return a->hh_mm < b->hh_mm ? -1 : 1;
    if (a->sec_e7 != b->sec_e7)
        return a->sec_e7 < b->sec_e7 ? -1 : 1;
    return 0;
}

/** Per-satellite cursor for walking epoch presence ranges. */
struct sat_cursor
{
    int range_idx;    /* index into sv->when[] */
    int obs_idx;      /* cumulative observation index */
};

/** Advance cursor to \a epoch_idx.  Returns 1 if present, 0 if absent. */
static int cursor_at_epoch(
    struct sat_cursor *c,
    const struct rinex_satellite_data *sv,
    int epoch_idx
)
{
    if (!sv)
        return 0;

    /* Advance past ranges that end before this epoch. */
    while (c->range_idx < sv->when_used
        && sv->when[c->range_idx].end <= epoch_idx)
    {
        c->obs_idx += sv->when[c->range_idx].end
                    - sv->when[c->range_idx].start;
        c->range_idx++;
    }

    /* Is the satellite present at this epoch? */
    if (c->range_idx < sv->when_used
        && sv->when[c->range_idx].start <= epoch_idx
        && epoch_idx < sv->when[c->range_idx].end)
    {
        return 1;
    }
    return 0;
}

/** Get the current observation index for a satellite at its current cursor
 * position, assuming cursor_at_epoch() returned 1.
 */
static int cursor_obs_idx(
    const struct sat_cursor *c,
    const struct rinex_satellite_data *sv,
    int epoch_idx
)
{
    return c->obs_idx + (epoch_idx - sv->when[c->range_idx].start);
}

/** Compare satellite observations at a matched epoch. */
static void compare_epoch(
    int epoch_idx,
    struct sat_cursor *c1, int n_sv1,
    struct sat_cursor *c2, int n_sv2
)
{
    char ts[32];
    int ii, n_sv_max, sys;

    epoch_ts(&data1.epoch[epoch_idx], ts);

    if (data1.epoch[epoch_idx].clock_offset != data2.epoch[epoch_idx].clock_offset)
    {
        printf("%s: epoch %s clock_offset %lld vs %lld\n",
            file1_name, ts,
            (long long)data1.epoch[epoch_idx].clock_offset,
            (long long)data2.epoch[epoch_idx].clock_offset);
        found_mismatch();
    }

    /* Walk all satellite systems and numbers. */
    for (sys = 0; sys < 32; ++sys)
    {
        struct rinex_system_data *s1 = &data1.sys[sys];
        struct rinex_system_data *s2 = &data2.sys[sys];
        int n1 = s1->sv.end - s1->sv.start;
        int n2 = s2->sv.end - s2->sv.start;

        n_sv_max = n1 > n2 ? n1 : n2;
        for (ii = 0; ii < n_sv_max; ++ii)
        {
            struct rinex_satellite_data *sv1 = NULL, *sv2 = NULL;
            int present1 = 0, present2 = 0;
            int oi1 = 0, oi2 = 0;

            if (ii < n1)
                sv1 = data1.sv[s1->sv.start + ii];
            if (ii < n2)
                sv2 = data2.sv[s2->sv.start + ii];
            if (!sv1 && !sv2)
                continue;

            /* Use the per-satellite cursors. */
            if (sv1)
            {
                int ci = s1->sv.start + ii;
                present1 = cursor_at_epoch(&c1[ci], sv1, epoch_idx);
                if (present1)
                    oi1 = cursor_obs_idx(&c1[ci], sv1, epoch_idx);
            }
            if (sv2)
            {
                int ci = s2->sv.start + ii;
                present2 = cursor_at_epoch(&c2[ci], sv2, epoch_idx);
                if (present2)
                    oi2 = cursor_obs_idx(&c2[ci], sv2, epoch_idx);
            }

            if (!present1 && !present2)
                continue;

            if (present1 && !present2)
            {
                printf("%s: epoch %s sat %s only in file1\n",
                    file1_name, ts, sv1->id);
                found_mismatch();
                continue;
            }
            if (!present1 && present2)
            {
                printf("%s: epoch %s sat %s only in file2\n",
                    file1_name, ts, sv2->id);
                found_mismatch();
                continue;
            }

            /* Both present -- compare observations. */
            {
                int n_obs = s1->n_obs < s2->n_obs ? s1->n_obs : s2->n_obs;
                int jj, diff = 0;

                for (jj = 0; jj < n_obs; ++jj)
                {
                    int64_t o1, o2;
                    char l1 = ' ', l2 = ' ', ss1 = ' ', ss2 = ' ';

                    o1 = (sv1->start[jj] >= 0)
                        ? data1.obs[sv1->start[jj] + oi1] : INT64_MIN;
                    o2 = (sv2->start[jj] >= 0)
                        ? data2.obs[sv2->start[jj] + oi2] : INT64_MIN;
                    if (sv1->start[jj] >= 0)
                    {
                        l1 = data1.lli[sv1->start[jj] + oi1];
                        ss1 = data1.ssi[sv1->start[jj] + oi1];
                    }
                    if (sv2->start[jj] >= 0)
                    {
                        l2 = data2.lli[sv2->start[jj] + oi2];
                        ss2 = data2.ssi[sv2->start[jj] + oi2];
                    }

                    if (o1 != o2 || l1 != l2 || ss1 != ss2)
                    {
                        if (!diff)
                        {
                            printf("%s: epoch %s sat %s obs differ:\n",
                                file1_name, ts, sv1->id);
                            diff = 1;
                        }
                        printf("  obs[%d]: %lld(lli='\\%o',ssi='\\%o') vs "
                            "%lld(lli='\\%o',ssi='\\%o')\n",
                            jj,
                            (long long)o1, (unsigned char)l1, (unsigned char)ss1,
                            (long long)o2, (unsigned char)l2, (unsigned char)ss2);
                    }
                }
                if (diff)
                    found_mismatch();
            }
        }
    }
}

/** Format the timestamp of the observation epoch nearest to \a epoch_index.
 * epoch_index is the count of observation epochs before the event, so we
 * use epoch[epoch_index] (the first epoch after the event) when available,
 * falling back to epoch[epoch_index-1] when the event trails all epochs.
 */
static void event_ts(int epoch_index, const struct rinex_data *data, char buf[32])
{
    int ii = epoch_index < data->epoch_used ? epoch_index : data->epoch_used - 1;
    if (ii >= 0)
        epoch_ts(&data->epoch[ii], buf);
    else
        snprintf(buf, 32, "(no epochs)");
}

/** Compare special event records between the two files. */
static void compare_events(void)
{
    int i1 = 0, i2 = 0;
    int n1 = data1.event_used, n2 = data2.event_used;

    while (i1 < n1 || i2 < n2)
    {
        const struct rinex_event *e1, *e2;
        int idx1 = (i1 < n1) ? data1.event[i1].epoch_index : INT_MAX;
        int idx2 = (i2 < n2) ? data2.event[i2].epoch_index : INT_MAX;
        char ts[32];

        if (idx1 < idx2)
        {
            event_ts(idx1, &data1, ts);
            printf("%s: event at %s only in file1\n", file1_name, ts);
            found_mismatch();
            i1++;
        }
        else if (idx2 < idx1)
        {
            event_ts(idx2, &data2, ts);
            printf("%s: event at %s only in file2\n", file1_name, ts);
            found_mismatch();
            i2++;
        }
        else
        {
            e1 = &data1.event[i1];
            e2 = &data2.event[i2];
            if (e1->text_len != e2->text_len
                || memcmp(e1->text, e2->text, (size_t)e1->text_len) != 0)
            {
                event_ts(idx1, &data1, ts);
                printf("%s: event at %s text differs\n", file1_name, ts);
                found_mismatch();
            }
            i1++;
            i2++;
        }
    }
}

static void compare_files(void)
{
    struct sat_cursor *c1, *c2;
    char ts[32];
    int i1, i2, n1, n2, n_sv1, n_sv2, cmp, epoch_idx;

    n1 = data1.epoch_used;
    n2 = data2.epoch_used;

    /* Figure out total satellite slot counts for cursor arrays. */
    n_sv1 = n_sv2 = 0;
    {
        int sys;
        for (sys = 0; sys < 32; ++sys)
        {
            int e1 = data1.sys[sys].sv.end;
            int e2 = data2.sys[sys].sv.end;
            if (e1 > n_sv1) n_sv1 = e1;
            if (e2 > n_sv2) n_sv2 = e2;
        }
    }

    c1 = calloc(n_sv1 ? n_sv1 : 1, sizeof *c1);
    c2 = calloc(n_sv2 ? n_sv2 : 1, sizeof *c2);
    if (!c1 || !c2)
    {
        fprintf(stderr, "Unable to allocate cursor arrays\n");
        exit(EXIT_FAILURE);
    }

    /* Walk both epoch arrays in parallel. */
    i1 = i2 = 0;
    while (i1 < n1 && i2 < n2)
    {
        cmp = epoch_cmp(&data1.epoch[i1], &data2.epoch[i2]);
        if (cmp < 0)
        {
            epoch_ts(&data1.epoch[i1], ts);
            printf("%s: epoch %s only in file1\n", file1_name, ts);
            found_mismatch();
            i1++;
        }
        else if (cmp > 0)
        {
            epoch_ts(&data2.epoch[i2], ts);
            printf("%s: epoch %s only in file2\n", file1_name, ts);
            found_mismatch();
            i2++;
        }
        else
        {
            /* Both files have this epoch (use i1 as the canonical index
             * for satellite cursor advancement).
             */
            compare_epoch(i1, c1, n_sv1, c2, n_sv2);
            i1++;
            i2++;
        }
    }

    /* Report remaining epochs in file1. */
    while (i1 < n1)
    {
        epoch_ts(&data1.epoch[i1], ts);
        printf("%s: epoch %s only in file1\n", file1_name, ts);
        found_mismatch();
        i1++;
    }

    /* Report remaining epochs in file2. */
    while (i2 < n2)
    {
        epoch_ts(&data2.epoch[i2], ts);
        printf("%s: epoch %s only in file2\n", file1_name, ts);
        found_mismatch();
        i2++;
    }

    free(c1);
    free(c2);

    compare_events();
}

int main(int argc, char *argv[])
{
    const char *err;
    int ii;

    /* Parse arguments. */
    for (ii = 1; ii < argc; ++ii)
    {
        if (!strcmp(argv[ii], "--full"))
        {
            full_mode = 1;
            memmove(&argv[ii], &argv[ii + 1],
                (argc - ii - 1) * sizeof(argv[0]));
            --argc;
            --ii;
        }
    }

    if (argc != 3)
    {
        fprintf(stderr, "Usage: rnxcmp [--full] file1 file2\n");
        return EXIT_FAILURE;
    }

    file1_name = argv[1];
    file2_name = argv[2];

    /* Load both files. */
    err = rinex_load_file(file1_name, &data1);
    if (err)
    {
        fprintf(stderr, "Unable to load %s: %s\n", file1_name, err);
        return EXIT_FAILURE;
    }

    err = rinex_load_file(file2_name, &data2);
    if (err)
    {
        fprintf(stderr, "Unable to load %s: %s\n", file2_name, err);
        free_rinex_data(&data1);
        return EXIT_FAILURE;
    }

    /* Compare the two files. */
    compare_files();

    free_rinex_data(&data1);
    free_rinex_data(&data2);

    return had_diff ? EXIT_FAILURE : EXIT_SUCCESS;
}
