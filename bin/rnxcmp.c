/** rnxcmp.c - RINEX observation file comparison utility.
 * Copyright 2020 Michael Poole.
 * SPDX-License-Identifier: MIT-Modern-Variant
 */

#include "rinex/rinex_load.h"
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

/** Compare satellite observations at a matched epoch, which is
 * data1.epoch[i1] and data2.epoch[i2].  The files need not start at the
 * same epoch, so the indices can differ.
 */
static void compare_epoch(
    int i1, struct sat_cursor *c1,
    int i2, struct sat_cursor *c2
)
{
    char ts[32];
    int ii, n_sv_max, sys;

    epoch_ts(&data1.epoch[i1], ts);

    if (data1.epoch[i1].clock_offset != data2.epoch[i2].clock_offset)
    {
        printf("%s: epoch %s clock_offset %lld vs %lld\n",
            file1_name, ts,
            (long long)data1.epoch[i1].clock_offset,
            (long long)data2.epoch[i2].clock_offset);
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
                present1 = cursor_at_epoch(&c1[ci], sv1, i1);
                if (present1)
                    oi1 = cursor_obs_idx(&c1[ci], sv1, i1);
            }
            if (sv2)
            {
                int ci = s2->sv.start + ii;
                present2 = cursor_at_epoch(&c2[ci], sv2, i2);
                if (present2)
                    oi2 = cursor_obs_idx(&c2[ci], sv2, i2);
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

/** event_key locates a special event record within its file. */
struct event_key
{
    /** has_time is set if the event's own epoch line has a timestamp,
     * which is then in #time.
     */
    int has_time;
    struct rinex_epoch time;

    /** prev and next are the observation epochs just before and after
     * the event, or NULL at the start or end of the file.
     */
    const struct rinex_epoch *prev, *next;
};

/** Parse the unsigned, right-justified integer in \a line[start] to
 * \a line[start+width-1].  Returns 0 on success, -1 if the field is
 * blank, beyond \a len, or not a number.
 */
static int parse_field(const char *line, int len, int start, int width,
    int *out)
{
    int ii, n_digits = 0;

    if (start + width > len)
        return -1;
    *out = 0;
    for (ii = start; ii < start + width; ++ii)
    {
        if (line[ii] >= '0' && line[ii] <= '9')
        {
            *out = *out * 10 + (line[ii] - '0');
            ++n_digits;
        }
        else if (line[ii] != ' ' || n_digits)
            return -1;
    }
    return n_digits ? 0 : -1;
}

/** Parse the F11.7 seconds field at \a line[start] into \a *sec_e7.
 * Returns 0 on success, -1 on failure.
 */
static int parse_seconds(const char *line, int len, int start, int *sec_e7)
{
    int whole, frac, ii;

    if (start + 11 > len || line[start + 3] != '.'
        || parse_field(line, len, start, 3, &whole))
    {
        return -1;
    }
    for (ii = start + 4, frac = 0; ii < start + 11; ++ii)
    {
        if (line[ii] < '0' || line[ii] > '9')
            return -1;
        frac = frac * 10 + (line[ii] - '0');
    }
    *sec_e7 = whole * 10000000 + frac;
    return 0;
}

/** Fill \a key for event \a ev of \a data. */
static void event_key(
    struct event_key *key,
    const struct rinex_event *ev,
    const struct rinex_data *data
)
{
    const char *line = ev->text;
    const char *eol = memchr(line, '\n', ev->text_len);
    int len = eol ? (int)(eol - line) : ev->text_len;
    int yy, mm, dd, hh, mi;
    int v2 = data->rinex_version == 2;

    key->prev = ev->epoch_index > 0
        ? &data->epoch[ev->epoch_index - 1] : NULL;
    key->next = ev->epoch_index < data->epoch_used
        ? &data->epoch[ev->epoch_index] : NULL;

    /* The time fields are blank for some events (flags 2-5). */
    key->has_time = v2
        ? !parse_field(line, len, 1, 2, &yy) && !parse_field(line, len, 4, 2, &mm)
          && !parse_field(line, len, 7, 2, &dd) && !parse_field(line, len, 10, 2, &hh)
          && !parse_field(line, len, 13, 2, &mi)
          && !parse_seconds(line, len, 15, &key->time.sec_e7)
        : !parse_field(line, len, 2, 4, &yy) && !parse_field(line, len, 7, 2, &mm)
          && !parse_field(line, len, 10, 2, &dd) && !parse_field(line, len, 13, 2, &hh)
          && !parse_field(line, len, 16, 2, &mi)
          && !parse_seconds(line, len, 18, &key->time.sec_e7);
    if (!key->has_time)
        return;
    if (v2)
        yy += (yy < 80) ? 2000 : 1900;
    key->time.yyyy_mm_dd = (yy * 100 + mm) * 100 + dd;
    key->time.hh_mm = hh * 100 + mi;
}

/** Compare two optional epochs; NULL sorts first.  Returns -1, 0 or 1. */
static int opt_epoch_cmp(const struct rinex_epoch *a, const struct rinex_epoch *b)
{
    if (!a || !b)
        return (a != NULL) - (b != NULL);
    return epoch_cmp(a, b);
}

/** Compare the positions of two events that have timestamps.
 * Returns -1, 0 or 1.
 */
static int timed_event_cmp(const struct event_key *a, const struct event_key *b)
{
    return epoch_cmp(&a->time, &b->time);
}

/** Compare the positions of two events without timestamps.  Returns 0
 * if they are at the same position, else -1 or 1 according to which
 * comes first.
 *
 * Each such event lies between its neighbouring observation epochs, and
 * two of them match if either neighbour is the same in both files, so
 * that an epoch missing from one file does not stop them matching.
 */
static int untimed_event_cmp(const struct event_key *a, const struct event_key *b)
{
    if (!opt_epoch_cmp(a->prev, b->prev)
        || (a->next && b->next && !epoch_cmp(a->next, b->next))
        || (!a->next && !b->next))
    {
        return 0;
    }
    return opt_epoch_cmp(a->prev, b->prev);
}

/** Describe the position of an event with key \a key. */
static void event_desc(const struct event_key *key, char buf[64])
{
    char ts[32];

    if (key->has_time)
    {
        epoch_ts(&key->time, ts);
        snprintf(buf, 64, "event at %s", ts);
    }
    else if (key->prev)
    {
        epoch_ts(key->prev, ts);
        snprintf(buf, 64, "untimed event after %s", ts);
    }
    else
    {
        snprintf(buf, 64, "untimed event before the first epoch");
    }
}

/** event_ref is an event, its position, and its index in its file. */
struct event_ref
{
    struct event_key key;
    const struct rinex_event *ev;
    int idx;
};

/** qsort() comparator: order event_refs by timestamp, then file order. */
static int event_ref_time_cmp(const void *va, const void *vb)
{
    const struct event_ref *a = va, *b = vb;
    int res = epoch_cmp(&a->key.time, &b->key.time);

    return res ? res : a->idx - b->idx;
}

/** Collect the events of \a data that do (if \a timed) or do not have
 * timestamps, in file order.  Returns the number collected, and the
 * malloc'd array in \a *out.
 */
static int collect_events(
    const struct rinex_data *data,
    int timed,
    struct event_ref **out
)
{
    int ii, nn;

    *out = malloc((data->event_used ? data->event_used : 1) * sizeof **out);
    if (!*out)
    {
        fprintf(stderr, "Unable to allocate event arrays\n");
        exit(EXIT_FAILURE);
    }
    for (ii = nn = 0; ii < data->event_used; ++ii)
    {
        struct event_ref *ref = &(*out)[nn];

        event_key(&ref->key, &data->event[ii], data);
        if (!ref->key.has_time != !timed)
            continue;
        ref->ev = &data->event[ii];
        ref->idx = ii;
        ++nn;
    }
    return nn;
}

/** Match the events in \a r1 (from file1) against those in \a r2 (from
 * file2), both ordered consistently with \a cmp, and report events in
 * only one file or with different text.
 */
static void match_events(
    const struct event_ref *r1, int n1,
    const struct event_ref *r2, int n2,
    int (*cmp)(const struct event_key *, const struct event_key *)
)
{
    int i1 = 0, i2 = 0, res;
    char desc[64];

    while (i1 < n1 || i2 < n2)
    {
        if (i1 >= n1)
            res = 1;
        else if (i2 >= n2)
            res = -1;
        else
            res = cmp(&r1[i1].key, &r2[i2].key);

        if (res < 0)
        {
            event_desc(&r1[i1].key, desc);
            printf("%s: %s only in file1\n", file1_name, desc);
            found_mismatch();
            i1++;
        }
        else if (res > 0)
        {
            event_desc(&r2[i2].key, desc);
            printf("%s: %s only in file2\n", file1_name, desc);
            found_mismatch();
            i2++;
        }
        else
        {
            const struct rinex_event *e1 = r1[i1].ev, *e2 = r2[i2].ev;

            if (e1->text_len != e2->text_len
                || memcmp(e1->text, e2->text, (size_t)e1->text_len) != 0)
            {
                event_desc(&r1[i1].key, desc);
                printf("%s: %s text differs\n", file1_name, desc);
                found_mismatch();
            }
            i1++;
            i2++;
        }
    }
}

/** Compare special event records between the two files.
 *
 * Events with timestamps are matched by timestamp (sorted, so the files
 * need not list them in order).  Events without timestamps are matched
 * separately by their neighbouring epochs (see untimed_event_cmp()).
 * The two kinds are matched independently because an untimed event
 * cannot be ordered against timed events that share its interval, so
 * the relative order of an untimed and a timed event is not compared.
 */
static void compare_events(void)
{
    struct event_ref *r1, *r2;
    int n1, n2;

    n1 = collect_events(&data1, 1, &r1);
    n2 = collect_events(&data2, 1, &r2);
    qsort(r1, n1, sizeof *r1, event_ref_time_cmp);
    qsort(r2, n2, sizeof *r2, event_ref_time_cmp);
    match_events(r1, n1, r2, n2, timed_event_cmp);
    free(r1);
    free(r2);

    n1 = collect_events(&data1, 0, &r1);
    n2 = collect_events(&data2, 0, &r2);
    match_events(r1, n1, r2, n2, untimed_event_cmp);
    free(r1);
    free(r2);
}

static void compare_files(void)
{
    struct sat_cursor *c1, *c2;
    char ts[32];
    int i1, i2, n1, n2, n_sv1, n_sv2, cmp;

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
            /* Both files have this epoch. */
            compare_epoch(i1, c1, i2, c2);
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

static void usage(FILE *out, const char *argv0)
{
    fprintf(out, "Usage: %s [--full] [--help] file1 file2\n", argv0);
}

int main(int argc, char *argv[])
{
    const char *err;
    int ii;

    /* Parse arguments. */
    for (ii = 1; ii < argc; ++ii)
    {
        if (!strcmp(argv[ii], "--help"))
        {
            usage(stdout, argv[0]);
            return EXIT_SUCCESS;
        }
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
        usage(stderr, argv[0]);
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
