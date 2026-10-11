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

/** obs_pair matches one observation code between the two files. */
struct obs_pair
{
    /** code is the NUL-padded observation code, such as "C1C". */
    const char *code;

    /** idx1 and idx2 index the code in each file's list for the
     * satellite system, or are -1 if that file does not list it.
     */
    int idx1, idx2;
};

/** obs_pairs[sys] lists the union of both files' observation codes for
 * the satellite system with index sys, so observations are compared by
 * code rather than by position; n_obs_pairs[sys] is its length.
 */
static struct obs_pair *obs_pairs[32];
static int n_obs_pairs[32];
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
/** Compare one observation (value, LLI and SSI) between the files.  If
 * they differ, report it, first printing a heading naming \a where and
 * \a sat_id unless *\a diff shows one was printed already.
 */
static void compare_value(
    const char *where,
    const char *sat_id,
    const struct obs_pair *pair,
    int64_t o1, char l1, char ss1,
    int64_t o2, char l2, char ss2,
    int *diff
)
{
    if (o1 == o2 && l1 == l2 && ss1 == ss2)
        return;
    if (!*diff)
    {
        printf("%s: %s sat %s obs differ:\n", file1_name, where, sat_id);
        *diff = 1;
    }
    printf("  %.4s: %lld(lli='\\%o',ssi='\\%o') vs "
        "%lld(lli='\\%o',ssi='\\%o')\n",
        pair->code,
        (long long)o1, (unsigned char)l1, (unsigned char)ss1,
        (long long)o2, (unsigned char)l2, (unsigned char)ss2);
}

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

            /* Both present -- compare observations, matched by code.
             * A code that one file does not list (or never observed)
             * is missing there.
             */
            {
                char where[48];
                int jj, diff = 0;

                snprintf(where, sizeof where, "epoch %s", ts);
                for (jj = 0; jj < n_obs_pairs[sys]; ++jj)
                {
                    const struct obs_pair *pair = &obs_pairs[sys][jj];
                    int st1 = pair->idx1 >= 0 ? sv1->start[pair->idx1] : -1;
                    int st2 = pair->idx2 >= 0 ? sv2->start[pair->idx2] : -1;
                    int64_t o1, o2;
                    char l1 = ' ', l2 = ' ', ss1 = ' ', ss2 = ' ';

                    o1 = (st1 >= 0) ? data1.obs[st1 + oi1] : INT64_MIN;
                    o2 = (st2 >= 0) ? data2.obs[st2 + oi2] : INT64_MIN;
                    if (st1 >= 0)
                    {
                        l1 = data1.lli[st1 + oi1];
                        ss1 = data1.ssi[st1 + oi1];
                    }
                    if (st2 >= 0)
                    {
                        l2 = data2.lli[st2 + oi2];
                        ss2 = data2.ssi[st2 + oi2];
                    }

                    compare_value(where, sv1->id, pair, o1, l1, ss1,
                        o2, l2, ss2, &diff);
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

    /** flag is the event's epoch flag ('1' to '6'), or ' ' if unknown. */
    char flag;

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

    key->flag = len > (v2 ? 28 : 31) ? line[v2 ? 28 : 31] : ' ';
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

/** records[0] and records[1] parse flag-1 and flag-6 records from
 * data1 and data2 respectively, so that their observations can be
 * compared by code.  Each is created when first needed; records_failed[]
 * notes a header that could not be used, so it is not tried again.
 */
static struct rinex_parser *records[2];
static int records_failed[2];

/** Parse event \a ev of \a data (file number \a which, 0 or 1), which
 * should be a flag-1 or flag-6 record.  Returns the parser holding the
 * record, or NULL if it cannot be parsed.
 */
static const struct rinex_parser *parse_obs_event(
    int which,
    const struct rinex_data *data,
    const struct rinex_event *ev
)
{
    struct rinex_parser *p;

    if (!records[which] && !records_failed[which]
        && rinex_open_records(&records[which], data->file_header,
            data->file_header_len))
    {
        records_failed[which] = 1;
    }
    p = records[which];
    if (!p || rinex_parse_record(p, ev->text, ev->text_len) != RINEX_SUCCESS
        || (p->epoch.flag != '1' && p->epoch.flag != '6'))
    {
        return NULL;
    }
    return p;
}

/** Find satellite \a sv in the current record of \a p, or NULL. */
static const struct rinex_sv_info *find_sat(
    const struct rinex_parser *p,
    const struct rinex_sv_info *sv
)
{
    int ii;

    for (ii = 0; ii < p->epoch.n_sats; ++ii)
    {
        if (p->sats[ii].system == sv->system && p->sats[ii].number == sv->number)
            return &p->sats[ii];
    }
    return NULL;
}

/** Fetch observation \a idx of \a sv in the current record of \a p,
 * using the parser's missing-value convention (0 with blank indicators)
 * if \a idx is -1 (the file does not list that code).
 */
static void event_value(
    const struct rinex_parser *p,
    const struct rinex_sv_info *sv,
    int idx,
    int64_t *obs, char *lli, char *ssi
)
{
    if (idx < 0 || idx >= p->n_obs[sv->system & 31])
    {
        *obs = 0;
        *lli = *ssi = ' ';
        return;
    }
    *obs = p->obs[sv->obs_0 + idx];
    *lli = p->lli[sv->obs_0 + idx];
    *ssi = p->ssi[sv->obs_0 + idx];
}

/** Compare matched flag-1 or flag-6 records \a e1 and \a e2 (described
 * by \a desc) by their contents, matching observations by code.
 * Returns 0 if either cannot be parsed, so the caller can compare their
 * text instead; else 1.
 */
static int compare_obs_events(
    const char *desc,
    const struct rinex_event *e1,
    const struct rinex_event *e2
)
{
    const struct rinex_parser *p1, *p2;
    char sat_id[5];
    int ii, jj, diff, any_diff = 0;

    p1 = parse_obs_event(0, &data1, e1);
    p2 = parse_obs_event(1, &data2, e2);
    if (!p1 || !p2)
        return 0;

    if (p1->epoch.flag != p2->epoch.flag)
    {
        printf("%s: %s flag %c vs %c\n", file1_name, desc,
            p1->epoch.flag, p2->epoch.flag);
        any_diff = 1;
    }
    if (p1->epoch.clock_offset != p2->epoch.clock_offset)
    {
        printf("%s: %s clock_offset %lld vs %lld\n", file1_name, desc,
            (long long)p1->epoch.clock_offset,
            (long long)p2->epoch.clock_offset);
        any_diff = 1;
    }

    for (ii = 0; ii < p1->epoch.n_sats; ++ii)
    {
        const struct rinex_sv_info *sv1 = &p1->sats[ii];
        const struct rinex_sv_info *sv2 = find_sat(p2, sv1);
        int sys = sv1->system & 31;

        snprintf(sat_id, sizeof sat_id, "%c%02d", sv1->system, sv1->number);
        if (!sv2)
        {
            printf("%s: %s sat %s only in file1\n", file1_name, desc, sat_id);
            any_diff = 1;
            continue;
        }

        diff = 0;
        for (jj = 0; jj < n_obs_pairs[sys]; ++jj)
        {
            const struct obs_pair *pair = &obs_pairs[sys][jj];
            int64_t o1, o2;
            char l1, l2, ss1, ss2;

            event_value(p1, sv1, pair->idx1, &o1, &l1, &ss1);
            event_value(p2, sv2, pair->idx2, &o2, &l2, &ss2);
            compare_value(desc, sat_id, pair, o1, l1, ss1, o2, l2, ss2, &diff);
        }
        any_diff |= diff;
    }
    for (ii = 0; ii < p2->epoch.n_sats; ++ii)
    {
        const struct rinex_sv_info *sv2 = &p2->sats[ii];

        if (find_sat(p1, sv2))
            continue;
        snprintf(sat_id, sizeof sat_id, "%c%02d", sv2->system, sv2->number);
        printf("%s: %s sat %s only in file2\n", file1_name, desc, sat_id);
        any_diff = 1;
    }

    if (any_diff)
        found_mismatch();
    return 1;
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
            int is_obs1 = r1[i1].key.flag == '1' || r1[i1].key.flag == '6';
            int is_obs2 = r2[i2].key.flag == '1' || r2[i2].key.flag == '6';

            /* Observation records hold values in each file's own code
             * order, so compare their contents rather than their text.
             */
            event_desc(&r1[i1].key, desc);
            if (is_obs1 && is_obs2 && compare_obs_events(desc, e1, e2))
            {
                /* compared */
            }
            else if (e1->text_len != e2->text_len
                || memcmp(e1->text, e2->text, (size_t)e1->text_len) != 0)
            {
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

    for (n1 = 0; n1 < 2; ++n1)
    {
        if (records[n1])
            records[n1]->destroy(records[n1]);
        records[n1] = NULL;
    }
}

/** Fill obs_pairs[] with the union of the two files' observation codes
 * for each satellite system: file1's codes in its order, then any codes
 * that only file2 lists.
 */
static void build_obs_pairs(void)
{
    int sys, jj, kk, nn;

    for (sys = 0; sys < 32; ++sys)
    {
        const struct rinex_system_data *s1 = &data1.sys[sys];
        const struct rinex_system_data *s2 = &data2.sys[sys];
        char *used2;

        n_obs_pairs[sys] = 0;
        obs_pairs[sys] = malloc((s1->n_obs + s2->n_obs + 1) * sizeof **obs_pairs);
        used2 = calloc(s2->n_obs + 1, 1);
        if (!obs_pairs[sys] || !used2)
        {
            fprintf(stderr, "Unable to allocate observation code map\n");
            exit(EXIT_FAILURE);
        }

        nn = 0;
        for (jj = 0; jj < s1->n_obs; ++jj)
        {
            obs_pairs[sys][nn].code = s1->obs[jj];
            obs_pairs[sys][nn].idx1 = jj;
            obs_pairs[sys][nn].idx2 = -1;
            for (kk = 0; kk < s2->n_obs; ++kk)
            {
                if (!used2[kk] && !strncmp(s1->obs[jj], s2->obs[kk], 4))
                {
                    obs_pairs[sys][nn].idx2 = kk;
                    used2[kk] = 1;
                    break;
                }
            }
            ++nn;
        }
        for (kk = 0; kk < s2->n_obs; ++kk)
        {
            if (used2[kk])
                continue;
            obs_pairs[sys][nn].code = s2->obs[kk];
            obs_pairs[sys][nn].idx1 = -1;
            obs_pairs[sys][nn].idx2 = kk;
            ++nn;
        }
        n_obs_pairs[sys] = nn;
        free(used2);
    }
}

static void compare_files(void)
{
    struct sat_cursor *c1, *c2;
    char ts[32];
    int i1, i2, n1, n2, n_sv1, n_sv2, cmp, ii;

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

    build_obs_pairs();

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

    for (ii = 0; ii < 32; ++ii)
        free(obs_pairs[ii]);
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
