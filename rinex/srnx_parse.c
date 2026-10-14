/** srnx_parse.c - Row-oriented rinex_parser backend for SRNX files.
 * Copyright 2026 Michael Poole.
 * SPDX-License-Identifier: MIT-Modern-Variant
 *
 * This adapts the column-oriented SRNX reader API (srnx_open_obs_by_index(),
 * srnx_read_obs_value(), ...) to the epoch-oriented "pull" contract of
 * struct rinex_parser, so that every driver.c-based tool gains SRNX
 * support with no changes.
 */

#include "rinex/rnx_priv.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** srnx_v1_sat holds the streaming state for one satellite: which epochs
 * it is present in, and one open observation reader (plus fully-decoded
 * LLI/SSI arrays) per known signal.
 */
struct srnx_v1_sat
{
    /** name is the satellite's name, e.g. "G01". */
    struct srnx_satellite_name name;

    /** sys_id is the (possibly expanded, ' ' -> 'G') system letter. */
    char sys_id;

    /** svn is the zero-based satellite number. */
    int svn;

    /** n_obs is the number of possible observation codes for #sys_id. */
    int n_obs;

    /** when holds the epoch-index ranges where this satellite is present. */
    struct rinex_range *when;

    /** when_used is the number of valid entries in #when. */
    int when_used;

    /** when_alloc is the allocated length of #when. */
    int when_alloc;

    /** run_idx is the index of the current or next entry in #when. */
    int run_idx;

    /** value_idx is how many present epochs have been consumed so far;
     * indexes into each signal's #lli / #ssi arrays and tracks how many
     * values have been pulled from the matching #readers entry.
     */
    int value_idx;

    /** obs_used is the total number of present epochs (sum of #when
     * range lengths); the expected length of each signal's LLI/SSI.
     */
    int obs_used;

    /** readers[i] is the open observation reader for signal i, or NULL
     * if this satellite has no data for that signal.  Length #n_obs.
     */
    struct srnx_obs_reader **readers;

    /** lli[i] holds #obs_used decoded LLI characters for signal i, or
     * NULL if this satellite has no data for that signal.  Length
     * #n_obs.
     */
    char **lli;

    /** ssi[i] holds #obs_used decoded SSI characters for signal i, or
     * NULL if this satellite has no data for that signal.  Length
     * #n_obs.
     */
    char **ssi;
};

/** srnx_v1_parser is a rinex_parser backed by an SRNX file. */
struct srnx_v1_parser
{
    /** base is the standard rinex_parser contents. */
    struct rinex_parser base;

    /** srnx is the underlying SRNX reader. */
    struct srnx_reader *srnx;

    /** epochs holds every epoch's timestamp and clock offset. */
    struct rinex_epoch *epochs;

    /** n_epochs is the number of valid entries in #epochs. */
    size_t n_epochs;

    /** epoch_idx is the index of the next epoch to return from #epochs. */
    size_t epoch_idx;

    /** rinex_version is 2, 3 or 4, used to parse event header lines. */
    int rinex_version;

    /** events holds special event records, sorted by epoch_index. */
    struct rinex_event *events;

    /** n_events is the number of valid entries in #events. */
    int n_events;

    /** events_alloc is the allocated length of #events. */
    int events_alloc;

    /** event_idx is the index of the next pending event in #events. */
    int event_idx;

    /** sats holds per-satellite streaming state. */
    struct srnx_v1_sat *sats;

    /** n_sats is the number of valid entries in #sats. */
    int n_sats;

    /** sats_alloc is the allocated length of #base.sats. */
    int sats_alloc;

    /** obs_alloc is the allocated length of #base.obs, #base.lli and
     * #base.ssi.
     */
    int obs_alloc;

    /** records parses observation-shaped EVTF records.  It is created
     * by the first one read (from the RHDR header) and kept after that.
     */
    struct rinex_parser *records;

    /** own_stream is a stream created by rinex_open() (to decompress
     * the file), destroyed along with this parser.
     */
    struct rinex_stream *own_stream;
};

/** Parses the flag and n_sats fields out of a RINEX v2-style epoch
 * header line (the first line of a special event's raw text), matching
 * the column layout used by rnx_v2_parse_time().
 */
static int srnx_v1_parse_event_v2(struct rinex_epoch *epoch, const char *line)
{
    int yy, mm, dd, hh, min, n_sats;
    int64_t i64 = 0;

    epoch->flag = line[28];
    if (line[28] < '2' || line[28] > '5')
        return EINVAL;

    yy = mm = dd = hh = min = n_sats = 0;
    /* These fields may be blank for pure event records; ignore failures
     * except for the mandatory flag and n_sats fields. */
    parse_uint(&yy, line + 1, 2);
    parse_uint(&mm, line + 4, 2);
    parse_uint(&dd, line + 7, 2);
    parse_uint(&hh, line + 10, 2);
    parse_uint(&min, line + 13, 2);
    parse_fixed(&i64, line + 15, 11, 7);
    if (parse_uint(&n_sats, line + 29, 3))
        return EINVAL;

    yy += (yy < 80) ? 2000 : 1900;
    epoch->yyyy_mm_dd = (yy * 100 + mm) * 100 + dd;
    epoch->hh_mm = hh * 100 + min;
    epoch->sec_e7 = i64;
    epoch->n_sats = n_sats;
    epoch->clock_offset = 0;
    return 0;
}

/** Parses the flag and n_sats fields out of a RINEX v3/v4-style epoch
 * header line, matching the column layout used by rnx_read_v34().
 */
static int srnx_v1_parse_event_v34(struct rinex_epoch *epoch, const char *line)
{
    int yy, mm, dd, hh, min, n_sats, len;
    int64_t i64 = 0;

    if (line[0] != '>' || line[31] < '2' || line[31] > '5')
        return EINVAL;

    yy = mm = dd = hh = min = n_sats = 0;
    parse_uint(&yy, line + 2, 4);
    parse_uint(&mm, line + 7, 2);
    parse_uint(&dd, line + 10, 2);
    parse_uint(&hh, line + 13, 2);
    parse_uint(&min, line + 16, 2);
    parse_fixed(&i64, line + 18, 11, 7);
    if (parse_uint(&n_sats, line + 32, 3))
        return EINVAL;

    epoch->yyyy_mm_dd = (yy * 100 + mm) * 100 + dd;
    epoch->hh_mm = hh * 100 + min;
    epoch->sec_e7 = i64;
    epoch->flag = line[31];
    epoch->n_sats = n_sats;
    epoch->clock_offset = 0;

    /* Any picosecond digits follow the (unused) clock offset field. */
    for (len = 0; line[len] != '\n' && line[len] != '\0'; ++len)
    {
        /* no operation */
    }
    rnx_v34_parse_ps(epoch, line + 56, len > 56 ? len - 56 : 0);
    return 0;
}

/** Ensures #base.sats has room for at least \a n_sats entries. */
static rinex_error_t srnx_v1_ensure_sats(struct srnx_v1_parser *p, int n_sats)
{
    void *new_ptr;
    int new_count;

    if (p->sats_alloc >= n_sats)
        return RINEX_SUCCESS;

    new_count = p->sats_alloc ? p->sats_alloc : 16;
    while (new_count < n_sats)
        new_count *= 2;

    new_ptr = realloc(p->base.sats, (size_t)new_count * sizeof p->base.sats[0]);
    if (!new_ptr)
    {
        rnx_errorf(&p->base, "allocation failed for satellite array of length %d", new_count);
        return RINEX_ERR_SYSTEM;
    }
    p->base.sats = new_ptr;
    p->sats_alloc = new_count;
    return RINEX_SUCCESS;
}

/** Ensures #base.obs, #base.lli and #base.ssi have room for at least
 * \a n_obs entries.
 */
static rinex_error_t srnx_v1_ensure_obs(struct srnx_v1_parser *p, int n_obs)
{
    void *new_obs, *new_lli, *new_ssi;
    int new_count;

    if (p->obs_alloc >= n_obs)
        return RINEX_SUCCESS;

    new_count = p->obs_alloc ? p->obs_alloc : 64;
    while (new_count < n_obs)
        new_count *= 2;

    new_obs = realloc(p->base.obs, (size_t)new_count * sizeof p->base.obs[0]);
    if (!new_obs)
    {
        rnx_errorf(&p->base, "allocation failed for observation array of length %d", new_count);
        return RINEX_ERR_SYSTEM;
    }
    p->base.obs = new_obs;

    new_lli = realloc(p->base.lli, (size_t)new_count);
    if (!new_lli)
    {
        rnx_errorf(&p->base, "allocation failed for LLI array of length %d", new_count);
        return RINEX_ERR_SYSTEM;
    }
    p->base.lli = new_lli;

    new_ssi = realloc(p->base.ssi, (size_t)new_count);
    if (!new_ssi)
    {
        rnx_errorf(&p->base, "allocation failed for SSI array of length %d", new_count);
        return RINEX_ERR_SYSTEM;
    }
    p->base.ssi = new_ssi;

    p->obs_alloc = new_count;
    return RINEX_SUCCESS;
}

/** Frees everything owned by a single srnx_v1_sat entry. */
static void srnx_v1_free_sat(struct srnx_v1_sat *sv)
{
    int ii;

    free(sv->when);
    for (ii = 0; ii < sv->n_obs; ++ii)
    {
        if (sv->readers)
            srnx_free_obs_reader(sv->readers[ii]);
        if (sv->lli)
            free(sv->lli[ii]);
        if (sv->ssi)
            free(sv->ssi[ii]);
    }
    free(sv->readers);
    free(sv->lli);
    free(sv->ssi);
}

/** Loads the special-event records (from EVTF chunks) into #events. */
static int srnx_v1_load_events(struct srnx_v1_parser *p)
{
    const char *event_text;
    size_t event_len;
    uint64_t epoch_index;
    int res;

    event_text = NULL;
    while ((res = srnx_next_special_event(p->srnx, &event_text,
            &event_len, &epoch_index)) == 0)
    {
        struct rinex_event *ev;
        char *text;

        if (p->n_events >= p->events_alloc)
        {
            struct rinex_event *new_ev;
            int new_alloc = p->events_alloc ? p->events_alloc * 2 : 8;
            new_ev = realloc(p->events, (size_t)new_alloc * sizeof *new_ev);
            if (!new_ev)
            {
                rnx_errorf(&p->base, "allocation failed for event array of length %d", new_alloc);
                return RINEX_ERR_SYSTEM;
            }
            p->events = new_ev;
            p->events_alloc = new_alloc;
        }

        text = malloc(event_len);
        if (!text)
        {
            rnx_errorf(&p->base, "allocation failed for event text of length %zu", event_len);
            return RINEX_ERR_SYSTEM;
        }
        memcpy(text, event_text, event_len);

        ev = &p->events[p->n_events++];
        ev->epoch_index = (int)epoch_index;
        ev->text_len = (int)event_len;
        ev->text = text;
    }

    if (res != SRNX_NO_CHUNK)
    {
        rnx_errorf(&p->base, "srnx_next_special_event failed: %d", res);
        return RINEX_ERR_BAD_FORMAT;
    }
    return RINEX_SUCCESS;
}

/** Builds the per-satellite SOCD index: presence runs plus one open
 * observation reader (and fully-decoded LLI/SSI) per known signal.
 */
static int srnx_v1_load_sats(struct srnx_v1_parser *p)
{
    struct srnx_satellite_name *sat_names = NULL;
    uint64_t n_sat_names = 0;
    int res, ii, jj;

    res = srnx_get_satellites(p->srnx, &sat_names, &n_sat_names);
    if (res)
    {
        rnx_errorf(&p->base, "srnx_get_satellites failed: %d", res);
        return RINEX_ERR_BAD_FORMAT;
    }

    if (!n_sat_names)
    {
        free(sat_names);
        return RINEX_SUCCESS;
    }
    p->sats = calloc((size_t)n_sat_names, sizeof *p->sats);
    if (!p->sats)
    {
        free(sat_names);
        rnx_errorf(&p->base, "allocation failed for %"PRIu64" satellites", n_sat_names);
        return RINEX_ERR_SYSTEM;
    }

    for (ii = 0; ii < (int)n_sat_names; ++ii)
    {
        struct srnx_satellite_name name = sat_names[ii];
        struct srnx_v1_sat *sv;
        char sys_id = name.name[0];
        int svn = (name.name[1] - '0') * 10 + (name.name[2] - '0') - 1;
        int n_obs;

        if (sys_id == ' ')
            sys_id = 'G';
        n_obs = p->base.n_obs[sys_id & 31];
        if (n_obs <= 0 || svn < 0)
            continue; /* unsupported system or bad SV id; skip */

        sv = &p->sats[p->n_sats];
        memset(sv, 0, sizeof *sv);
        sv->name = name;
        sv->sys_id = sys_id;
        sv->svn = svn;
        sv->n_obs = n_obs;

        res = srnx_get_presence_runs(p->srnx, name, n_obs,
            &sv->when, &sv->when_used, &sv->when_alloc, &sv->obs_used);
        if (res)
        {
            free(sat_names);
            rnx_errorf(&p->base, "srnx_get_presence_runs failed for %.3s: %d", name.name, res);
            return RINEX_ERR_BAD_FORMAT;
        }
        ++p->n_sats;

        if (sv->obs_used == 0)
            continue;

        sv->readers = calloc((size_t)n_obs, sizeof *sv->readers);
        sv->lli = calloc((size_t)n_obs, sizeof *sv->lli);
        sv->ssi = calloc((size_t)n_obs, sizeof *sv->ssi);
        if (!sv->readers || !sv->lli || !sv->ssi)
        {
            free(sat_names);
            rnx_errorf(&p->base, "allocation failed for %.3s signal state", name.name);
            return RINEX_ERR_SYSTEM;
        }

        for (jj = 0; jj < n_obs; ++jj)
        {
            struct srnx_obs_reader *rdr = NULL;
            int n_values;

            res = srnx_open_obs_by_index(p->srnx, name, jj, &rdr);
            if (res == SRNX_UNKNOWN_CODE)
                continue; /* satellite has no data for this signal */
            if (res)
            {
                free(sat_names);
                rnx_errorf(&p->base, "srnx_open_obs_by_index failed for %.3s: %d", name.name, res);
                return RINEX_ERR_BAD_FORMAT;
            }

            res = srnx_read_obs_ssi_lli(rdr, &n_values, &sv->lli[jj], &sv->ssi[jj]);
            if (res)
            {
                srnx_free_obs_reader(rdr);
                free(sat_names);
                rnx_errorf(&p->base, "srnx_read_obs_ssi_lli failed for %.3s: %d", name.name, res);
                return RINEX_ERR_BAD_FORMAT;
            }

            sv->readers[jj] = rdr;
        }
    }

    free(sat_names);
    return RINEX_SUCCESS;
}

/** Decode an observation-shaped EVTF using the ordinary RINEX parser.
 * Its observations are returned for this read only, without consuming any
 * EPOC entry or advancing the column readers. */
static rinex_error_t srnx_v1_read_observation_event(struct srnx_v1_parser *p,
    const struct rinex_event *ev)
{
    const char *header, *error;
    size_t header_len;
    struct rinex_parser *event;
    rinex_error_t result;
    int ii, n_obs = 0;

    if (!p->records)
    {
        if (srnx_get_header(p->srnx, &header, &header_len))
            return RINEX_ERR_BAD_FORMAT;
        if (header_len > INT_MAX)
            return RINEX_ERR_BAD_FORMAT;
        error = rinex_open_records(&p->records, header, (int)header_len);
        if (error)
        {
            rnx_errorf(&p->base, "invalid observation EVTF header: %s", error);
            return RINEX_ERR_BAD_FORMAT;
        }
    }
    event = p->records;

    result = rinex_parse_record(event, ev->text, ev->text_len);
    if (result != RINEX_SUCCESS)
    {
        rnx_errorf(&p->base, "invalid observation EVTF: %s", event->errmsg);
        return result == RINEX_ERR_SYSTEM ? result : RINEX_ERR_BAD_FORMAT;
    }
    for (ii = 0; ii < event->epoch.n_sats; ++ii)
        n_obs += event->n_obs[p->rinex_version == 2 ? 0 : event->sats[ii].system & 31];
    result = srnx_v1_ensure_sats(p, event->epoch.n_sats);
    if (result != RINEX_SUCCESS) return result;
    result = srnx_v1_ensure_obs(p, n_obs);
    if (result != RINEX_SUCCESS) return result;
    p->base.epoch = event->epoch;
    if (event->epoch.n_sats)
        memcpy(p->base.sats, event->sats, event->epoch.n_sats * sizeof *event->sats);
    if (n_obs)
    {
        memcpy(p->base.obs, event->obs, n_obs * sizeof *event->obs);
        memcpy(p->base.lli, event->lli, n_obs);
        memcpy(p->base.ssi, event->ssi, n_obs);
    }
    return RINEX_SUCCESS;
}

static rinex_error_t srnx_v1_read(struct rinex_parser *p_)
{
    struct srnx_v1_parser *p = (struct srnx_v1_parser *)p_;
    int ii, jj, base;
    rinex_error_t err;

    /* Any pending special events fire (in order) before the epoch they
     * precede.
     */
    if (p->event_idx < p->n_events
        && p->events[p->event_idx].epoch_index <= (int)p->epoch_idx)
    {
        struct rinex_event *ev = &p->events[p->event_idx++];
        struct rinex_epoch epoch;
        int res, flag_pos = p->rinex_version == 2 ? 28 : 31;

        if (ev->text_len > flag_pos
            && (ev->text[flag_pos] == '1' || ev->text[flag_pos] == '6'))
        {
            err = srnx_v1_read_observation_event(p, ev);
            if (err != RINEX_SUCCESS) return err;
            epoch = p_->epoch;
            res = 0;
        }
        else
        {
            memset(&epoch, 0, sizeof epoch);
            res = (p->rinex_version == 2)
                ? srnx_v1_parse_event_v2(&epoch, ev->text)
                : srnx_v1_parse_event_v34(&epoch, ev->text);
        }
        if (res)
        {
            /* rnx_read_v34() has a pre-existing bug (see BUGS.md) where
             * it drops a v3/v4 special event's own header line before
             * it ever reaches rinex_load()/rnx2srnx, so the EVTF chunk
             * for such an event holds only its data lines with no
             * reconstructable header.  Degrade gracefully rather than
             * failing outright: treat the whole text as data lines,
             * borrowing the following epoch's timestamp (if any) as an
             * approximation.
             */
            int kk;

            memset(&epoch, 0, sizeof epoch);
            if (p->epoch_idx < p->n_epochs)
            {
                epoch.yyyy_mm_dd = p->epochs[p->epoch_idx].yyyy_mm_dd;
                epoch.hh_mm = p->epochs[p->epoch_idx].hh_mm;
                epoch.sec_e7 = p->epochs[p->epoch_idx].sec_e7;
            }
            epoch.flag = '4';
            for (kk = 0; kk < ev->text_len; ++kk)
                if (ev->text[kk] == '\n')
                    ++epoch.n_sats;
        }

        p_->epoch = epoch;
        if (p_->buffer_len < ev->text_len)
        {
            char *new_buf = realloc(p_->buffer, (size_t)ev->text_len);
            if (!new_buf)
            {
                rnx_errorf(p_, "allocation failed for event buffer of length %d", ev->text_len);
                return RINEX_ERR_SYSTEM;
            }
            p_->buffer = new_buf;
        }
        memcpy(p_->buffer, ev->text, (size_t)ev->text_len);
        p_->buffer_len = ev->text_len;
        return RINEX_SUCCESS;
    }

    if (p->epoch_idx >= p->n_epochs)
        return RINEX_EOF;

    p_->epoch = p->epochs[p->epoch_idx];
    /* srnx_get_epochs() does not record a per-epoch flag (EPOC entries
     * are always ordinary observation epochs); '0' matches the same
     * convention used when re-emitting SRNX epochs as RINEX text (see
     * emit_v2_epoch_header()/emit_v34_epoch_header() in srnx2rnx.c).
     */
    p_->epoch.flag = '0';
    p_->epoch.n_sats = 0;

    /* Collect satellites present at this epoch. */
    base = 0;
    for (ii = 0; ii < p->n_sats; ++ii)
    {
        struct srnx_v1_sat *sv = &p->sats[ii];
        struct rinex_sv_info *out_sv;
        int present;

        while (sv->run_idx < sv->when_used
            && (int)p->epoch_idx >= sv->when[sv->run_idx].end)
            ++sv->run_idx;

        present = sv->run_idx < sv->when_used
            && (int)p->epoch_idx >= sv->when[sv->run_idx].start
            && (int)p->epoch_idx < sv->when[sv->run_idx].end;
        if (!present)
            continue;

        err = srnx_v1_ensure_sats(p, p_->epoch.n_sats + 1);
        if (err != RINEX_SUCCESS)
            return err;
        err = srnx_v1_ensure_obs(p, base + sv->n_obs);
        if (err != RINEX_SUCCESS)
            return err;

        out_sv = &p_->sats[p_->epoch.n_sats];
        out_sv->system = (sv->name.name[0] == ' ') ? ' ' : sv->sys_id;
        out_sv->number = (unsigned char)(sv->svn + 1);
        out_sv->obs_0 = (unsigned short)base;

        for (jj = 0; jj < sv->n_obs; ++jj)
        {
            int64_t value;
            char lli, ssi;

            if (sv->readers && sv->readers[jj]
                && sv->value_idx < sv->obs_used)
            {
                int res = srnx_read_obs_value(sv->readers[jj], &value);
                if (res)
                {
                    rnx_errorf(p_, "srnx_read_obs_value failed for %.3s code %d (value_idx=%d obs_used=%d): %d",
                        sv->name.name, jj, sv->value_idx, sv->obs_used, res);
                    return RINEX_ERR_BAD_FORMAT;
                }
                lli = sv->lli[jj][sv->value_idx];
                ssi = sv->ssi[jj][sv->value_idx];
            }
            else
            {
                /* Absent observation: INT64_MIN with blank LLI/SSI,
                 * matching the text and CRX parsers.
                 */
                value = INT64_MIN;
                lli = ' ';
                ssi = ' ';
            }

            p_->obs[base + jj] = value;
            p_->lli[base + jj] = lli;
            p_->ssi[base + jj] = ssi;
        }

        ++sv->value_idx;
        base += sv->n_obs;
        ++p_->epoch.n_sats;
    }

    ++p->epoch_idx;
    return RINEX_SUCCESS;
}

static void srnx_v1_destroy(struct rinex_parser *p_)
{
    struct srnx_v1_parser *p = (struct srnx_v1_parser *)p_;
    int ii;

    if (p->sats)
    {
        for (ii = 0; ii < p->n_sats; ++ii)
            srnx_v1_free_sat(&p->sats[ii]);
        free(p->sats);
    }

    if (p->events)
    {
        for (ii = 0; ii < p->n_events; ++ii)
            free(p->events[ii].text);
        free(p->events);
    }

    free(p->epochs);
    free(p_->sats);
    free(p_->obs);
    free(p_->lli);
    free(p_->ssi);
    free(p_->buffer);

    srnx_close(p->srnx);
    if (p->records)
        p->records->destroy(p->records);

    /* Unlike srnx->data (an SRNX-owned mmap), #base.stream is owned by
     * the caller of rinex_open(), exactly as for the text/CRX parsers;
     * it is not destroyed here unless rinex_open() created it.
     */
    if (p->own_stream)
        p->own_stream->destroy(p->own_stream);
    free(p);
}

/* Doc comment in rnx_priv.h. */
const char *srnx_open_parser(
    struct rinex_parser **p_parser,
    struct rinex_stream *stream,
    struct rinex_stream *own_stream
)
{
    struct srnx_v1_parser *p;
    const char *rhdr;
    size_t rhdr_len, n_epochs;
    char *buf, *end;
    int res;

    p = calloc(1, sizeof *p);
    if (!p)
        return "Memory allocation failed";
    p->own_stream = own_stream;
    p->base.stream = stream;
    p->base.read = srnx_v1_read;
    p->base.destroy = srnx_v1_destroy;
    *p_parser = &p->base;

    if (!srnx_reset(&p->srnx))
        return rnx_errorf(&p->base, "allocation failed for SRNX reader");

    res = srnx_open_buffer(p->srnx, stream->buffer, stream->size, 0);
    if (res)
        return rnx_errorf(&p->base, "srnx_open_buffer failed: %d", res);

    res = srnx_get_header(p->srnx, &rhdr, &rhdr_len);
    if (res)
        return rnx_errorf(&p->base, "srnx_get_header failed: %d", res);

    buf = malloc(rhdr_len);
    if (!buf)
        return rnx_errorf(&p->base, "allocation failed for file header");
    memcpy(buf, rhdr, rhdr_len);
    p->base.buffer = buf;
    p->base.buffer_len = (int)rhdr_len;

    p->rinex_version = (int)strtol(buf, &end, 10);
    if (p->rinex_version < 2 || p->rinex_version > 4 || *end != '.')
        return rnx_errorf(&p->base, "unrecognized RINEX version %.11s", buf);

    srnx_fill_n_obs(p->srnx, p->base.n_obs);

    res = srnx_get_epochs(p->srnx, &p->epochs, &n_epochs);
    if (res)
        return rnx_errorf(&p->base, "srnx_get_epochs failed: %d", res);
    p->n_epochs = n_epochs;

    res = srnx_v1_load_events(p);
    if (res != RINEX_SUCCESS)
        return p->base.errmsg;

    res = srnx_v1_load_sats(p);
    if (res != RINEX_SUCCESS)
        return p->base.errmsg;

    return NULL;
}
