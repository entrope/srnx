/** rinex_load.c - RINEX whole-file loader.
 * Copyright 2024 Michael Poole.
 * SPDX-License-Identifier: MIT-Modern-Variant
 */

#include "rinex/rinex_load.h"
#include "rinex/rnx_priv.h"

#include <limits.h>
#include <string.h>

void free_rinex_data(struct rinex_data *data)
{
    int ii, n_sv = 0;

    /* Deallocate inner pointers. */
    free((char *)data->file_header);
    free(data->epoch);
    free(data->sv);
    free(data->obs);
    free(data->ssi);
    free(data->lli);

    /* Process the constellation metadata. */
    for (ii = 0; ii < 32; ++ii)
    {
        if (data->rinex_version > 2 || ii == 0)
        {
            free(data->sys[ii].obs);
        }
        data->sys[ii].obs = NULL;
    }

    /* Null them to avoid confusion. */
    data->file_header = NULL;
    data->epoch = NULL;
    data->sv = NULL;
    data->obs = NULL;
    data->ssi = NULL;
    data->lli = NULL;
}

const char *rnx_data_init_cons_v2(struct rinex_data *out)
{
    static const char n_types_obs[] = "# / TYPES OF OBSERV";
    char *sep;
    long n_obs;
    int ofs;

    /* How many observables are present? */
    ofs = rnx_find_header(out->file_header, out->file_header_len, n_types_obs, sizeof n_types_obs);
    if (ofs < 0)
        return "unable to find # / TYPES OF OBSERV header";
    n_obs = strtol(out->file_header + ofs, &sep, 10);
    if (n_obs < 1 || *sep != ' ')
        return "corrupt # / TYPES OF OBSERV header";

    /* Initialize the first slot. */
    out->sys[0].obs = calloc(n_obs, 4);
    if (!out->sys[0].obs)
        return "unable to allocate constellation observables array";
    out->sys[0].n_obs = n_obs;
    out->sys[0].sv.start = 0;
    out->sys[0].sv.end = 0;

    /* Copy observable names. */
    for (ofs = 0; ofs < n_obs; ++ofs)
    {
        /* Copy the observable name. */
        out->sys[0].obs[ofs][0] = sep[4];
        out->sys[0].obs[ofs][1] = sep[5];
        out->sys[0].obs[ofs][2] = '\0';
        out->sys[0].obs[ofs][3] = '\0';
        sep += 6;

        /* Are we at the end of the current line? (9 per line) */
        if (ofs % 9 == 8)
        {
            sep = strchr(sep, '\n');
            if (!sep || (sep + 72) > (out->file_header + out->file_header_len))
                return "# / TYPES OF OBSERV ran past end of file header";
            if (strncmp(sep + 61, n_types_obs, sizeof n_types_obs - 1))
                return "# / TYPES OF OBSERV header ended prematurely";
            sep += 7; /* skip newline and first six spaces of next line */
        }
    }

    /* Copy the first slot to other constellations' slots. */
    memcpy(&out->sys['G' & 31], out->sys, sizeof *out->sys);
    memcpy(&out->sys['C' & 31], out->sys, sizeof *out->sys);
    memcpy(&out->sys['E' & 31], out->sys, sizeof *out->sys);
    memcpy(&out->sys['J' & 31], out->sys, sizeof *out->sys);
    memcpy(&out->sys['S' & 31], out->sys, sizeof *out->sys);
    memcpy(&out->sys['R' & 31], out->sys, sizeof *out->sys);

    return NULL;
}

const char *rnx_data_init_cons_v34(struct rinex_data *out)
{
    static const char sys_n_obs_types[] = "SYS / # / OBS TYPES";
    struct rinex_system_data *p_cons;
    char cons, *sep;
    long n_obs;
    int ofs;
 
    /* Find the observable-list header. */
    ofs = rnx_find_header(out->file_header, out->file_header_len, sys_n_obs_types, sizeof sys_n_obs_types);
    if (ofs < 0)
        return "unable to find SYS / # / OBS TYPES header";

    /* Process the per-constellation data. */
    sep = (char *)out->file_header + ofs;
    while (!strncmp(sep + 60, sys_n_obs_types, sizeof sys_n_obs_types))
    {
        /* Get the constellation ID and observable count. */
        cons = *sep;
        n_obs = strtol(sep + 1, &sep, 10);
        if (n_obs < 1 || cons == ' ' || *sep != ' ')
            return "corrupt SYS / # / OBS TYPES header";
        p_cons = &out->sys[cons & 31];
        if (p_cons->n_obs > 0)
            return "repeated constellation ID for SYS / # / OBS TYPES";
        p_cons->n_obs = n_obs;
        p_cons->obs = calloc(n_obs, 4);
        if (!p_cons->obs)
            return "unable to allocate constellation observables array";

        /* Copy the observable names. */
        for (ofs = 0; ofs < n_obs; ++ofs)
        {
            /* Copy the observable name. */
            p_cons->obs[ofs][0] = sep[1];
            p_cons->obs[ofs][1] = sep[2];
            p_cons->obs[ofs][2] = sep[3];
            p_cons->obs[ofs][3] = '\0';
            sep += 4;

            /* Are we at the end of the line? */
            if (ofs % 13 == 12)
            {
                sep = strchr(sep, '\n');
                if (sep == NULL || (sep + 72) > (out->file_header + out->file_header_len))
                    return "SYS / # / OBS TYPES ran past end of file header";
                if (strncmp(sep + 61, sys_n_obs_types, sizeof sys_n_obs_types - 1))
                    return "SYS / # / OBS TYPES header ended prematurely";
                ++sep; /* skip newline */
                if ((ofs + 1) < n_obs)
                    sep += 6; /* skip first six spaces of next line */
            }
        }
    }

    return NULL;
}

/** rnx_data_init_cons populates \a out->sys[] and \a out->sv[] based
 * on \a out->file_header.
 *
 * \returns NULL on success, or a text string explaining the failure.
 */
const char *rnx_data_init_cons(struct rinex_data *out)
{
    static const char interval[] = "INTERVAL";
    int ofs;

    /* Populate out->interval. */
    ofs = rnx_find_header(out->file_header, out->file_header_len, interval, sizeof interval);
    out->interval = (ofs < 0) ? 1 : strtol(out->file_header + ofs, NULL, 10);

    if (out->rinex_version == 2)
        return rnx_data_init_cons_v2(out);

    if (out->rinex_version == 3 || out->rinex_version == 4)
        return rnx_data_init_cons_v34(out);

    return "unknown RINEX version";
}

const char *rnx_load_grow_epochs(struct rinex_data *out)
{
    struct rinex_epoch *new_epochs;

    if (out->epoch_alloc == 0)
    {
        out->epoch_alloc = 86400 / out->interval;
        new_epochs = malloc(out->epoch_alloc * sizeof *out->epoch);
    }
    else
    {
        out->epoch_alloc *= 2;
        new_epochs = realloc(out->epoch, out->epoch_alloc * sizeof *out->epoch);
    }

    if (!new_epochs)
        return "unable to (re-)allocate epoch array";

    out->epoch = new_epochs;
    return NULL;
}

const char *rnx_load_grow_system(struct rinex_data *out, char sys_id, int svn)
{
    struct rinex_system_data *p_sys;
    struct rinex_satellite_data **new_sv;
    int ii, n_sv, tot_sv, old_pos, old_n_sv;

    /* Where was this system before? */
    p_sys = &out->sys[sys_id & 31];
    old_pos = p_sys->sv.start;
    old_n_sv = p_sys->sv.end - old_pos;

    /* How many other satellites are already used? */
    for (ii = tot_sv = 0; ii < 32; ++ii)
    {
        tot_sv += out->sys[ii].sv.end - out->sys[ii].sv.start;
        if (out->sys[ii].sv.start > old_pos)
        {
            out->sys[ii].sv.start -= old_n_sv;
            out->sys[ii].sv.end -= old_n_sv;
        }
    }
 
    /* How many satellites do we want for this system afterwards? */
    if (old_n_sv == 0)
    {
        switch (sys_id)
        {
        case 'C' & 31: n_sv = 38; break; /* Beidou */
        case 'E' & 31: n_sv = 32; break; /* Galileo */
        case 'G' & 31: n_sv = 32; break; /* GPS */
        case 'I' & 31: n_sv = 8; break; /* NavIC */
        case 'J' & 31: n_sv = 10; break; /* QZSS */
        case 'R' & 31: n_sv = 32; break; /* GLONASS */
        case 'S' & 31: n_sv = 58; break; /* SBAS */
        default: return "unknown satellite system";
        }
    }
    while (n_sv <= svn)
        n_sv *= 2;

    /* Did this constellation already have entries? */
    if (old_n_sv > 0)
    {
        /* Reallocate out->sv. */
        new_sv = calloc(sizeof *new_sv, tot_sv + n_sv - old_n_sv);
        if (!new_sv)
            return "unable to grow satellite array";

        /* Copy old satellite data to the new positions. */
        memcpy(new_sv, out->sv, p_sys->sv.start * sizeof *new_sv);
        memcpy(new_sv + p_sys->sv.start, out->sv + p_sys->sv.end,
            (tot_sv - old_n_sv) * sizeof *out->sv);
        memcpy(new_sv + tot_sv - old_n_sv, out->sv + p_sys->sv.start,
            old_n_sv * sizeof *out->sv);
        free(out->sv);
    }
    else if (tot_sv == 0)
    {
        /* Allocate initial satellite array. */
        new_sv = calloc(sizeof *new_sv, n_sv);
        if (!new_sv)
            return "unable to allocate satellite array";
    }
    else
    {
        /* Add new constellation at end of existing satellite array. */
        new_sv = realloc(out->sv, (tot_sv + n_sv) * sizeof *out->sv);
        if (!new_sv)
            return "unable to grow satellite array";
    }

    out->sv = new_sv;
    p_sys->sv.start = tot_sv - old_n_sv;
    p_sys->sv.end = p_sys->sv.start + n_sv;
    return NULL;
}

const char *rnx_load_alloc_satellite(struct rinex_data *out, char sys_id, int svn)
{
    struct rinex_satellite_data *p_sv;
    int n_obs, ii;

    /* Allocate *p_sv. */
    n_obs = out->sys[sys_id & 31].n_obs;
    p_sv = malloc(sizeof *p_sv + (n_obs - 1) * sizeof p_sv->start[0]);
    if (!p_sv)
        return "unable to allocate satellite data";

    /* Allocate initial when[] for the satellite. */
    p_sv->when_used = 1;
    p_sv->when_alloc = 4;
    p_sv->when = calloc(p_sv->when_alloc, sizeof *p_sv->when);
    if (!p_sv->when)
    {
        free(p_sv);
        return "unable to allocate initial epoch range for satellite";
    }
    p_sv->when[0].start = out->epoch_used;

    /* Save the satellite's ID. */
    p_sv->id[0] = sys_id;
    p_sv->id[1] = '0' + (svn / 10);
    p_sv->id[2] = '0' + (svn % 10);
    p_sv->id[3] = '\0';

    /* Zero the satellite's observation ranges. */
    p_sv->obs_used = 0;
    p_sv->obs_alloc = 0;
    for (ii = 0; ii < n_obs; ++ii)
        p_sv->start[ii] = -1;

    return NULL;
}

const char *rnx_load_grow_when(struct rinex_satellite_data *p_sv)
{
    struct rinex_range *new_when;

    p_sv->when_alloc *= 2;
    new_when = realloc(p_sv->when, p_sv->when_alloc * sizeof *p_sv->when);
    if (!new_when)
        return "unable to grow satellite epoch range";
    p_sv->when = new_when;

    return NULL;
}

#define RNX_OBS_RESERVED 4

int rnx_load_realloc_obs(struct rinex_data *out, int start, int len, int req)
{
    int prev, curr, best, best_prev, best_size, alloc;

    /* Is this the very first allocation? */
    if (!out->obs)
    {
        /* TODO */
    }

    /* Find the best fit for `req`. */
    best = best_prev = -1;
    best_size = INT_MAX;
    for (prev = 0; prev > 0; prev = curr)
    {
        curr = out->obs[prev + 1];
        if (out->obs[curr] < req)
            continue;
        if (out->obs[curr] < best_size)
        {
            best = curr;
            best_prev = prev;
            best_size = out->obs[curr];
        }
    }

    /* If we found a best fit, use it. */
    if (best > 0)
    {
        /* If this block is bigger than we want, make a new block with
         * the leftover space.
         */
        if (best_size > req)
        {
            out->obs[best + req + 0] = out->obs[best + 0] - req;
            out->obs[best + req + 1] = out->obs[best + 1];
            out->obs[best_prev + 1] = best + req;
        }
        else
        {
            out->obs[best_prev + 1] = out->obs[best + 1];
        }

        /* If we are reallocating a block, copy the old data over
         * and mark the old block as free.
         */
        if (len > 0)
        {
            memcpy(out->obs + best, out->obs + start, len * sizeof *out->obs);
            memcpy(out->ssi + best, out->ssi + start, len);
            memcpy(out->lli + best, out->lli + start, len);

            out->obs[start + 0] = len;
            out->obs[start + 1] = out->obs[1];
            out->obs[1] = start;
        }

        return best;
    }

    /* TODO: implement (grow out->obs) */

    return -1;
}

const char *rnx_load_grow_obs(struct rinex_data *out, struct rinex_satellite_data *p_sv)
{
    struct rinex_system_data *p_sys;
    int ii, req, idx;

    p_sys = &out->sys[p_sv->id[0] & 31];
    req = p_sv->obs_alloc * 3 / 2;
    if (req < 256)
        req = 256;
    for (ii = 0; ii < p_sys->n_obs; ++ii)
    {
        /* Unused observables are easy. */
        if (p_sv->start[ii] < 0)
            continue;

        idx = rnx_load_realloc_obs(out, p_sv->start[ii], p_sv->obs_alloc, req);
        if (idx < 0)
            return "unable to grow observation array";
        p_sv->start[ii] = idx;
    }
    p_sv->obs_alloc = req;
    return NULL;
}

const char *rnx_load_alloc_obs(struct rinex_data *out, struct rinex_satellite_data *p_sv, int idx)
{
    int pos, ii;

    if (!p_sv->obs_alloc)
        p_sv->obs_alloc = 86400 / out->interval / 3;

    pos = rnx_load_realloc_obs(out, -1, 0, p_sv->obs_alloc);
    if (pos < 0)
        return "unable to allocate observation array";
    p_sv->start[idx] = pos;
    for (ii = 0; ii < p_sv->obs_used; ++ii)
    {
        out->obs[pos + ii] = INT64_MIN;
        out->lli[pos + ii] = ' ';
        out->ssi[pos + ii] = ' ';
    }
    return NULL;
}

const char *rinex_load(struct rinex_stream *stream, struct rinex_data *out)
{
    struct rinex_parser *p;
    struct rinex_range *p_range;
    struct rinex_system_data *p_sys;
    struct rinex_satellite_data *p_sv;
    const char *errmsg;
    char sys_id, lli, ssi, *buf;
    int64_t obs;
    int ii, jj, kk, bit, ofs, res, svn;

    /* Initialize our output structure. */
    memset(out, 0, sizeof *out);
    out->file_header = NULL;
    out->epoch = NULL;
    out->sv = NULL;
    out->obs = NULL;
    out->ssi = NULL;
    out->lli = NULL;
    out->event = NULL;
    for (ii = 0; ii < 32; ++ii)
    {
        out->sys[ii].obs = NULL;
    }

    /* Open the file. */
    errmsg = rinex_open(&p, stream);
    if (errmsg)
        return errmsg;

    /* Save the RINEX header. */
    out->file_header_len = p->buffer_len;
    buf = malloc(out->file_header_len + 1);
    if (!buf)
        return "unable to allocate file header";
    memcpy(buf, p->buffer, out->file_header_len);
    buf[out->file_header_len] = '\0';
    out->file_header = buf;

    /* What format version is the file? */
    out->rinex_version = strtol(out->file_header, &buf, 10);
    if (out->rinex_version < 2 || out->rinex_version > 4 || *buf != '.')
    {
        free((char *)out->file_header);
        return "unrecognized RINEX version";
    }

    /* Initialize constellation and initial metadata. */
    errmsg = rnx_data_init_cons(out);
    if (errmsg)
        goto fail_errmsg;

    /* Scan the records in the file. */
    while ((res = p->read(p)) > 0)
    {
        /* TODO: Record special event epochs. */
        if (p->epoch.flag != '1' && p->epoch.flag != '6')
            continue;

        /* Copy epoch timestamp, first growing out->epoch if needed. */
        if (out->epoch_used == out->epoch_alloc)
        {
            errmsg = rnx_load_grow_epochs(out);
            if (errmsg)
                goto fail_errmsg;
        }
        memcpy(&out->epoch[out->epoch_used], &p->epoch, sizeof p->epoch);
        ++out->epoch_used;

        /* Process each satellite observed during this epoch. */
        buf = p->buffer;
        for (ii = jj = 0; ii < p->epoch.n_sats; ++ii)
        {
            sys_id = *buf++;
            p_sys = &out->sys[sys_id & 31];
            svn = *buf++ - 1;

            /* Do we have a slot for this satellite? */
            if (svn + p_sys->sv.start >= p_sys->sv.end)
            {
                errmsg = rnx_load_grow_system(out, sys_id, svn);
                if (errmsg)
                    goto fail_errmsg;
            }

            /* Do we have a struct for this satellite? */
            p_sv = out->sv[p_sys->sv.start + svn];
            if (!p_sv)
            {
                errmsg = rnx_load_alloc_satellite(out, sys_id, svn);
                if (errmsg)
                    goto fail_errmsg;
                p_sv = out->sv[p_sys->sv.start + svn];
            }

            /* Is this a new run of epochs? */
            p_range = &p_sv->when[p_sv->when_used - 1];
            if (p_range->end < out->epoch_used)
            {
                if (p_sv->when_used >= p_sv->when_alloc)
                {
                    errmsg = rnx_load_grow_when(p_sv);
                    if (errmsg)
                        goto fail_errmsg;
                }

                p_range = &p_sv->when[p_sv->when_used++];
                p_range->start = out->epoch_used;
            }
            p_range->end = out->epoch_used + 1;

            /* Do we need to grow the observation array? */
            if (p_sv->obs_used >= p_sv->obs_alloc)
            {
                errmsg = rnx_load_grow_obs(out, p_sv);
                if (errmsg)
                    goto fail_errmsg;
            }

            /* Save the observations. */
            for (kk = 0; kk < p_sys->n_obs; ++kk)
            {
                ofs = p_sv->obs_used + p_sv->start[kk];

                /* Was the observation present or defaulted? */
                bit = kk & 7;
                if (*buf & (1 << bit))
                {
                    if (p_sv->start[kk] < 0)
                    {
                        errmsg = rnx_load_alloc_obs(out, p_sv, kk);
                        if (errmsg)
                            goto fail_errmsg;
                    }
                    out->obs[ofs] = p->obs[jj];
                    out->lli[ofs] = p->lli[jj];
                    out->ssi[ofs] = p->ssi[jj];
                    ++jj;
                }
                else if (p_sv->start[kk] >= 0)
                {
                    out->obs[ofs] = INT64_MIN;
                    out->lli[ofs] = ' ';
                    out->ssi[ofs] = ' ';
                }
                if (bit == 7)
                {
                    buf++;
                }
            }

            /* Advance our indexes. */
            if (kk & 7)
            {
                buf++;
            }
            ++p_sv->obs_used;
        }
    }
    if (res < 0)
    {
        snprintf(out->error, sizeof out->error,
            "parser error %d", res);
        errmsg = out->error;
        goto fail_errmsg;
    }

    /* Clean up and exit. */
    p->destroy(p);
    return NULL;

fail_errmsg:
    free_rinex_data(out);
    p->destroy(p);
    return errmsg;
}