/** rinex_bridge.c - Python-friendly bridge to the RINEX loader.
 * Copyright 2024 Michael Poole.
 * SPDX-License-Identifier: MIT-Modern-Variant
 */

#include "rinex/rinex.h"
#include "rinex/rinex_load.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct rinex_data *rb_load(const char *filename, const char **err_out)
{
    struct rinex_stream *stream;
    struct rinex_data *data;
    const char *err;

    stream = rinex_mmap_stream(filename);
    if (!stream)
    {
        if (err_out)
            *err_out = "unable to open file";
        return NULL;
    }

    data = calloc(1, sizeof *data);
    if (!data)
    {
        stream->destroy(stream);
        if (err_out)
            *err_out = "out of memory";
        return NULL;
    }

    err = rinex_load(stream, data);
    stream->destroy(stream);
    if (err)
    {
        free(data);
        if (err_out)
            *err_out = err;
        return NULL;
    }

    if (err_out)
        *err_out = NULL;
    return data;
}

void rb_free(struct rinex_data *data)
{
    if (data)
    {
        free_rinex_data(data);
        free(data);
    }
}

int rb_epoch_count(const struct rinex_data *data)
{
    return data->epoch_used;
}

int rb_rinex_version(const struct rinex_data *data)
{
    return data->rinex_version;
}

int rb_interval(const struct rinex_data *data)
{
    return data->interval;
}

int rb_n_obs(const struct rinex_data *data, char sys)
{
    return data->sys[sys & 31].n_obs;
}

void rb_obs_name(const struct rinex_data *data, char sys, int idx, char out[4])
{
    const struct rinex_system_data *p = &data->sys[sys & 31];
    if (idx >= 0 && idx < p->n_obs)
        memcpy(out, p->obs[idx], 4);
    else
        memset(out, 0, 4);
}

int rb_list_satellites(const struct rinex_data *data, char sys,
                       char (*out_ids)[4], int max_sats)
{
    const struct rinex_system_data *p = &data->sys[sys & 31];
    int ii, count = 0;

    for (ii = p->sv.start; ii < p->sv.end && count < max_sats; ++ii)
    {
        if (data->sv[ii])
        {
            memcpy(out_ids[count], data->sv[ii]->id, 4);
            ++count;
        }
    }
    return count;
}

int rb_extract_obs(const struct rinex_data *data,
                   const char sat_id[4], int obs_idx,
                   int64_t *out, int out_len)
{
    const struct rinex_system_data *p_sys;
    const struct rinex_satellite_data *p_sv;
    int svn, ii, jj, src;

    p_sys = &data->sys[sat_id[0] & 31];
    svn = (sat_id[1] - '0') * 10 + (sat_id[2] - '0') - 1;
    if (svn < 0 || svn + p_sys->sv.start >= p_sys->sv.end)
        return -1;

    p_sv = data->sv[p_sys->sv.start + svn];
    if (!p_sv)
        return -1;
    if (obs_idx < 0 || obs_idx >= p_sys->n_obs)
        return -1;
    if (p_sv->start[obs_idx] < 0)
        return 0;

    /* Initialize output to missing. */
    for (ii = 0; ii < out_len; ++ii)
        out[ii] = INT64_MIN;

    /* Walk the RLE epoch ranges and copy observations. */
    src = p_sv->start[obs_idx];
    jj = 0;
    for (ii = 0; ii < p_sv->when_used; ++ii)
    {
        int epoch_start = p_sv->when[ii].start;
        int epoch_end = p_sv->when[ii].end;
        int kk;

        for (kk = epoch_start; kk < epoch_end && kk < out_len; ++kk)
        {
            out[kk] = data->obs[src + jj];
            ++jj;
        }
        if (kk >= out_len)
            break;
    }

    return data->epoch_used;
}

int rb_extract_lli(const struct rinex_data *data,
                   const char sat_id[4], int obs_idx,
                   char *out, int out_len)
{
    const struct rinex_system_data *p_sys;
    const struct rinex_satellite_data *p_sv;
    int svn, ii, jj, src;

    p_sys = &data->sys[sat_id[0] & 31];
    svn = (sat_id[1] - '0') * 10 + (sat_id[2] - '0') - 1;
    if (svn < 0 || svn + p_sys->sv.start >= p_sys->sv.end)
        return -1;

    p_sv = data->sv[p_sys->sv.start + svn];
    if (!p_sv)
        return -1;
    if (obs_idx < 0 || obs_idx >= p_sys->n_obs)
        return -1;
    if (p_sv->start[obs_idx] < 0)
        return 0;

    memset(out, ' ', out_len);

    src = p_sv->start[obs_idx];
    jj = 0;
    for (ii = 0; ii < p_sv->when_used; ++ii)
    {
        int epoch_start = p_sv->when[ii].start;
        int epoch_end = p_sv->when[ii].end;
        int kk;

        for (kk = epoch_start; kk < epoch_end && kk < out_len; ++kk)
        {
            out[kk] = data->lli[src + jj];
            ++jj;
        }
        if (kk >= out_len)
            break;
    }

    return data->epoch_used;
}

int rb_extract_epochs_sod(const struct rinex_data *data,
                          double *out, int out_len)
{
    int ii, n;

    n = data->epoch_used;
    if (n > out_len)
        n = out_len;

    for (ii = 0; ii < n; ++ii)
    {
        const struct rinex_epoch *e = &data->epoch[ii];
        out[ii] = (e->hh_mm / 100) * 3600.0
                + (e->hh_mm % 100) * 60.0
                + e->sec_e7 / 1e7;
    }

    return n;
}
