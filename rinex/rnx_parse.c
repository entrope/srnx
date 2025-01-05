/** rinex_parse.c - RINEX parsing utilities.
 * Copyright 2020-2021 Michael Poole.
 * SPDX-License-Identifier: MIT-Modern-Variant
 */

#include "rinex/rnx_priv.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/** rnx_ensure_sats ensures that \a p has enough capacity to hold
 * \a p->base.epoch.n_sats satellites of data.
 */
static rinex_error_t rnx_ensure_sats(struct rnx_v234_parser *p)
{
    if (p->sats_alloc < p->base.epoch.n_sats)
    {
        void *new_ptr;
        size_t new_len;
        int new_count;

        /* How many slots do we want to have? */
        new_count = p->sats_alloc ? p->sats_alloc : p->base.epoch.n_sats;
        while (new_count < p->base.epoch.n_sats)
        {
            new_count *= 2;
        }
        if ((size_t)new_count > SIZE_MAX / sizeof(p->base.sats[0]))
        {
            p->base.error_line = __LINE__;
            return RINEX_ERR_BAD_FORMAT;
        }

        /* (Re-)Allocate the array. */
        new_len = new_count * sizeof(p->base.sats[0]);
        new_ptr = realloc(p->base.sats, new_len);
        if (!new_ptr)
        {
            p->base.error_line = __LINE__;
            return RINEX_ERR_SYSTEM;
        }

        /* Write back the new array. */
        p->base.sats = new_ptr;
        p->sats_alloc = new_count;
    }

    return RINEX_SUCCESS;
}

RNX_RESOLVE(rinex_error_t, read_v2_observations,
    (struct rnx_v234_parser *p, const char *epoch, const char *obs),
    (p, epoch, obs),
    avx2, neon)

/* Doc comment in rnx_priv.h */
int rnx_v2_parse_time(struct rnx_v234_parser *p, const char *line)
{
    int64_t i64;
    int yy, mm, dd, hh, min, n_sats;

    i64 = 0;
    yy = mm = dd = hh = min = n_sats = 0;
    p->base.epoch.flag = line[28];
    if (line[28] < '0' || line[28] > '6'
        || parse_uint(&yy, line+1, 2) || parse_uint(&mm, line+4, 2)
        || parse_uint(&dd, line+7, 2) || parse_uint(&hh, line+10, 2)
        || parse_uint(&min, line+13, 2) || parse_uint(&n_sats, line+29, 3)
        || parse_fixed(&i64, line+15, 11, 7))
    {
        if (line[28] < '2' || line[28] == '6')
        {
            p->base.error_line = __LINE__;
            return RINEX_ERR_BAD_FORMAT;
        }
    }
    yy += (yy < 80) ? 2000 : 1900;
    p->base.epoch.yyyy_mm_dd = (yy * 100 + mm) * 100 + dd;
    p->base.epoch.hh_mm = hh * 100 + min;
    p->base.epoch.sec_e7 = i64;
    p->base.epoch.n_sats = n_sats;
    return 0;
}

/* Doc comment in rnx_priv.h. */
rinex_error_t rnx_read_v2(struct rinex_parser *p_)
{
    struct rnx_v234_parser *p = (struct rnx_v234_parser *)p_;
    const char *line;
    rinex_error_t err;
    int res, nn, n_sats, body_ofs, line_len;

    /* Make sure we have an epoch to parse. */
    res = rnx_get_newlines(p_, &p->parse_ofs, NULL, 0, 1);
    if (res <= RINEX_EOF)
    {
        return res;
    }
    if ((uint64_t)res < p->parse_ofs + 33)
    {
        p->parse_ofs = res;
        p_->error_line = __LINE__;
        return RINEX_ERR_BAD_FORMAT;
    }
    line_len = res - 1 - p->parse_ofs;
    line = p->base.stream->buffer + p->parse_ofs;

    /* Parse the timestamp, epoch flag and "number of satellites" field. */
    res = rnx_v2_parse_time(p, line);
    if (res < 0)
    {
        p->parse_ofs += line_len + 1;
        return res;
    }

    /* Is there a receiver clock offset? */
    if (line_len <= 68)
    {
        p->base.epoch.clock_offset = 0;
    }
    else if (line_len > 70)
    {
        if (parse_fixed(&p->base.epoch.clock_offset, line+68, line_len-68, line_len-71))
        {
            p->parse_ofs += line_len + 1;
            p_->error_line = __LINE__;
            return RINEX_ERR_BAD_FORMAT;
        }
    }
    else
    {
        p->parse_ofs += line_len + 1;
        p_->error_line = __LINE__;
        return RINEX_ERR_BAD_FORMAT;
    }

    /* Is it a set of observations or a special event? */
    n_sats = p->base.epoch.n_sats;
    switch (p->base.epoch.flag)
    {
    case '0': case '1': case '6':
        /* Get enough data. */
        nn = (p->base.n_obs[0] + 4) / 5; /* How many lines per satellite? */
        body_ofs = 0;
        res = rnx_get_newlines(p_, &p->parse_ofs, &body_ofs,
            (n_sats + 11) / 12, n_sats * nn);
        if (res <= RINEX_EOF)
        {
            if (res == RINEX_EOF)
            {
                p->parse_ofs = p->base.stream->size;
                res = RINEX_ERR_BAD_FORMAT;
            }
            else
            {
                p->parse_ofs += line_len + 1;
            }
            p_->error_line = __LINE__;
            return res;
        }
        line = p->base.stream->buffer + p->parse_ofs;
        p->parse_ofs = res;

        err = rnx_ensure_sats(p);
        if (err != RINEX_SUCCESS)
        {
            return err;
        }
        return rnx_read_v2_observations(p, line,
            p->base.stream->buffer + body_ofs);

    case '2': case '3': case '4': case '5':
        /* Get the data. */
        res = rnx_get_newlines(p_, &p->parse_ofs, NULL, 0, n_sats + 1);
        if (res <= 0)
        {
            if (res == RINEX_EOF)
            {
                res = RINEX_ERR_BAD_FORMAT;
            }
            p->parse_ofs += line_len + 1;
            p_->error_line = __LINE__;
            err = res;
        }
        else
        {
            p->parse_ofs = res;
            err = rnx_copy_text(p, res);
        }

        /* and we are done */
        return err;
    }

    p_->error_line = __LINE__;
    assert(0 && "logic failure in rnx_read_v2");
    return RINEX_ERR_BAD_FORMAT;
}

RNX_RESOLVE(rinex_error_t, read_v34_observations,
    (struct rnx_v234_parser *p, const char obs[]),
    (p, obs),
    avx2, neon)

/* Doc comment in rnx_priv.h. */
rinex_error_t rnx_read_v34(struct rinex_parser *p_)
{
    struct rnx_v234_parser *p = (struct rnx_v234_parser *)p_;
    const char *line;
    int64_t i64;
    int res, yy, mm, dd, hh, min, n_sats, line_len;
    rinex_error_t err;

    /* Make sure we have an epoch to parse. */
    res = rnx_get_newlines(p_, &p->parse_ofs, NULL, 0, 1);
    if (res <= RINEX_EOF)
    {
        return res;
    }
    if ((uint64_t)res < p->parse_ofs + 35)
    {
        p->parse_ofs = res;
        p_->error_line = __LINE__;
        return RINEX_ERR_BAD_FORMAT;
    }
    line_len = res - 1 - p->parse_ofs;
    line = p->base.stream->buffer + p->parse_ofs;

    /* Parse the timestamp, epoch flag and "number of satellites" field. */
    if ((line[0] != '>' || line[31] < '0' || line[31] > '6')
        || parse_uint(&yy, line+2, 4) || parse_uint(&mm, line+7, 2)
        || parse_uint(&dd, line+10, 2) || parse_uint(&hh, line+13, 2)
        || parse_uint(&min, line+16, 2) || parse_uint(&n_sats, line+32, 3)
        || parse_fixed(&i64, line+18, 11, 7))
    {
        p->parse_ofs = res;
        p_->error_line = __LINE__;
        return RINEX_ERR_BAD_FORMAT;
    }
    p->base.epoch.yyyy_mm_dd = (yy * 100 + mm) * 100 + dd;
    p->base.epoch.hh_mm = hh * 100 + min;
    p->base.epoch.sec_e7 = i64;
    p->base.epoch.flag = line[31];
    p->base.epoch.n_sats = n_sats;

    /* Is there a receiver clock offset? */
    p->parse_ofs = res;
    if (line_len <= 43) /* 6X reserved field ends at column 41, need at
                         * least the units digit or the decimal point. */
    {
        p->base.epoch.clock_offset = 0;
    }
    else if (line_len <= 56)
    {
        if (parse_fixed(&p->base.epoch.clock_offset, line + 41,
            line_len - 41, line_len - 44))
        {
            p_->error_line = __LINE__;
            return RINEX_ERR_BAD_FORMAT;
        }
    }
    else
    {
        p_->error_line = __LINE__;
        return RINEX_ERR_BAD_FORMAT;
    }

    /* Get enough data. */
    res = rnx_get_newlines(p_, &p->parse_ofs, NULL, 0, n_sats);
    if (res <= RINEX_EOF)
    {
        p->parse_ofs = p->base.stream->size;
        return res;
    }
    line_len = res - p->parse_ofs;
    line = p->base.stream->buffer + p->parse_ofs;
    p->parse_ofs = res;

    /* Is it a set of observations or a special event? */
    switch (p->base.epoch.flag)
    {
    case '0': case '1': case '6':
        err = rnx_ensure_sats(p);
        if (err != RINEX_SUCCESS)
        {
            return err;
        }
        return rnx_read_v34_observations(p, line);

    case '2': case '3': case '4': case '5':
        /* We already did most of the work. */
        memcpy(p->base.buffer, line, line_len);
        p->base.buffer_len = line_len;
        return RINEX_SUCCESS;
    }

    p_->error_line = __LINE__;
    assert(0 && "logic failure in rnx_read_v3");
    return RINEX_ERR_BAD_FORMAT;
}

/* Doc comment in rnx_priv.h */
void rnx_free_v23(struct rinex_parser *p_)
{
    struct rnx_v234_parser *p = (struct rnx_v234_parser *)p_;

    free(p->base.buffer);
    free(p->base.lli);
    free(p->base.ssi);
    free(p->base.obs);
    free(p);
}

/* Doc comment in rinex.h */
const char *rinex_find_header
(
    const struct rinex_parser *p,
    const char label[],
    unsigned int sizeof_label
)
{
    int ofs;

    ofs = rnx_find_header(p->buffer, p->buffer_len, label, sizeof_label);
    if (ofs < 0)
    {
        return NULL;
    }

    return p->buffer + ofs;
}

/** rnx_open_v2 reads the observation codes in \a p->base.header. */
static const char *rnx_open_v2(struct rnx_v234_parser *p)
{
    static const char n_obs[] = "# / TYPES OF OBSERV";
    const char *line;
    int res, value;
    char obs_type;

    /* What type of observations are in this file? */
    obs_type = p->base.buffer[40];
    if (!strchr(" GRSEM", obs_type))
    {
        return "Invalid satellite system for file";
    }

    /* Find the (first?) PRN / # OF OBS line. */
    line = rinex_find_header(&p->base, n_obs, sizeof n_obs);
    if (!line)
    {
        return "Could not find PRN / # OF OBS line";
    }
    res = parse_uint(&value, line, 6);
    if (res || (value < 1))
    {
        return "Invalid number of observations";
    }
    p->base.n_obs[' ' & 31] = value;
    if (obs_type == 'M')
    {
        p->base.n_obs['E' & 31] = value;
        p->base.n_obs['G' & 31] = value;
        p->base.n_obs['R' & 31] = value;
        p->base.n_obs['S' & 31] = value;
    }
    else
    {
        p->base.n_obs[obs_type & 31] = value;
        if (obs_type == ' ')
        {
            p->base.n_obs['G' & 31] = value;
        }
    }

    /* Initially assume 500 observations/epoch is enough space. */
    p->obs_alloc = 500;
    p->base.lli = calloc(p->obs_alloc, 1);
    p->base.ssi = calloc(p->obs_alloc, 1);
    p->base.obs = calloc(p->obs_alloc, sizeof p->base.obs[0]);
    if (!p->base.lli || !p->base.ssi || !p->base.obs)
    {
        return "Memory allocation failed";
    }

    return NULL;
}

/** rnx_open_v34 reads the observation codes in \a p->base.header. */
static const char *rnx_open_v34(struct rnx_v234_parser *p)
{
    static const char sys_n_obs[] = "SYS / # / OBS TYPES";
    const char *line;
    int res, ii, n_obs;
    char sys_id;

    /* Find the (first) SYS / # / OBS TYPES line. */
    line = rinex_find_header(&p->base, sys_n_obs, sizeof sys_n_obs);
    if (!line)
    {
        return "Could not find SYS / # / OBS TYPES line";
    }

    /* Keep going until we find a different header label. */
    while (!memcmp(line + 60, sys_n_obs, sizeof(sys_n_obs) - 1))
    {
        /* How many observations for this system? */
        sys_id = line[0];
        res = parse_uint(&n_obs, line + 3, 3);
        if (res || (n_obs < 1))
        {
            return "Invalid number of observations";
        }
        p->base.n_obs[sys_id & 31] = n_obs;

        /* Scan past following lines, 13 observation codes per line. */
        for (ii = 13; ii < n_obs; ii += 13)
        {
            line = strchr(line, '\n') + 1;
            if ((line[0] != ' ')
                || memcmp(line + 60, sys_n_obs, sizeof(sys_n_obs) - 1))
            {
                return "Expected a successor SYS / # / OBS TYPES line";
            }
        }

        /* Skip to the next line. */
        line = strchr(line, '\n') + 1;
    }

    /* Initially assume 500 observations is enough. */
    p->obs_alloc = 500;
    p->base.lli = calloc(p->obs_alloc, 1);
    p->base.ssi = calloc(p->obs_alloc, 1);
    p->base.obs = calloc(p->obs_alloc, sizeof p->base.obs[0]);
    if (!p->base.lli || !p->base.ssi || !p->base.obs)
    {
        return "Memory allocation failed";
    }

    return NULL;
}

/** Copy \a in_len bytes of header from \a in to \a out.
 *
 * Replace each newline sequences (CR, LF, CRLF) with a single '\n'.
 * Remove trailing spaces from each header line.
 *
 * \param[out] out Receives cleaned-up header.
 * \param[in] in Input header to copy from.
 * \param[in] in_len Length of header to copy, including the final
 *   newline sequence.
 * \returns Length of output on success, RINEX_ERR_BAD_FORMAT on illegal
 *   line length.
 */
static rinex_error_t rnx_copy_header
(
    char out[],
    const char in[],
    int in_len
)
{
    int ii, jj, last_eol, line_len;

    for (ii = jj = last_eol = 0; ii < in_len; ++ii)
    {
        /* Convert CRLF to '\n'. */
        out[jj] = in[ii];

        /* Are we at an EOL? */
        if (out[jj] == '\n')
        {
            /* Check total line length. */
            line_len = jj - last_eol;
            if (line_len < 61 || line_len > 80)
            {
                return RINEX_ERR_BAD_FORMAT;
            }

            /* Trim trailing whitespace. */
            while (out[jj-1] == ' ')
            {
                jj--;
            }
            out[jj] = '\n';
            last_eol = jj+1;
        }
        ++jj;
    }

    return jj;
}

const char *rnx_open_v23
(
    struct rnx_v234_parser *p,
    struct rinex_stream *stream,
    int hdr_ofs
)
{
    static const char end_of_header[] = "END OF HEADER";
    const char *err;
    int res;

    /* Check that it's an observation file. */
    if (stream->buffer[hdr_ofs + 20] != 'O')
    {
        return "Not an observation RINEX file";
    }

    /* Check for END OF HEADER. */
    res = rnx_find_header(stream->buffer, stream->size, end_of_header,
        sizeof end_of_header);
    if (res < 1)
    {
        return "Could not find end of header";
    }
    res = strchr(stream->buffer + res, '\n') - stream->buffer + 1;

    /* Allocate the parser structure. */
    p->base.stream = stream;

    /* Copy the header. */
    p->parse_ofs = res;
    p->buffer_alloc = res;
    p->base.buffer = calloc(p->buffer_alloc, 1);
    if (!p->base.buffer)
    {
        return "Memory allocation failed";
    }

    /* Copy the header for the caller's use. */
    res = rnx_copy_header(p->base.buffer, stream->buffer, res);
    p->base.buffer_len = res;
    if (res < 0)
    {
        err = "Invalid header line detected";
    }
    else if (!memcmp("     2.", stream->buffer, 7))
    {
        p->base.read = rnx_read_v2;
        err = rnx_open_v2(p);
    }
    else if (!memcmp("     3.", stream->buffer, 7)
        || !memcmp("     4.", stream->buffer, 7))
    {
        p->base.read = rnx_read_v34;
        err = rnx_open_v34(p);
    }
    else
    {
        err = "Unsupported RINEX version number";
    }

    return err;
}

/* Doc comment is in rnx_priv.h. */
const char rnx_version_type[] = "RINEX VERSION / TYPE";

/* Doc comment is in rinex.h. */
const char *rinex_open
(
    struct rinex_parser **p_parser,
    struct rinex_stream *stream
)
{
    static const char crx_version_type[]   = "CRINEX VERS   / TYPE";
    const char *err;
    int res;

    if (*p_parser)
    {
        (*p_parser)->destroy(*p_parser);
        *p_parser = NULL;
    }

    res = stream->advance(stream, BLOCK_SIZE, 0);
    if (res || stream->size < 80)
    {
        return strerror(res);
    }

    /* Is it an uncompressed RINEX file? */
    if (!memcmp(rnx_version_type, stream->buffer + 60, 20))
    {
        struct rnx_v234_parser *p;

        p = calloc(1, sizeof(struct rnx_v234_parser));
        if (!p)
        {
            return "Memory allocation failed";
        }

        p->base.lli = NULL;
        p->base.ssi = NULL;
        p->base.obs = NULL;
        p->base.destroy = rnx_free_v23;

        *p_parser = &p->base;
        err = rnx_open_v23(p, stream, 0);
    }
    /* Does it use Hatanaka compression? */
    else if (!memcmp(crx_version_type, stream->buffer + 60, 20))
    {
        struct crx_v23_parser *crx;
        struct rnx_v234_parser *p;

        crx = calloc(1, sizeof(struct crx_v23_parser));
        if (!crx)
        {
            return "Memory allocation failed";
        }
        p = &crx->base;
        p->base.lli = NULL;
        p->base.ssi = NULL;
        p->base.obs = NULL;
        crx->epoch_text = NULL;
        crx->order = NULL;
        crx->diff = NULL;
        p->base.destroy = crx_free_v23;

        *p_parser = &p->base;
        err = crx_open_v23(crx, stream);
    }
    else /* what is it? */
    {
        err = "Unrecognized file format";
    }

    if (err && *p_parser)
    {
        (*p_parser)->destroy(*p_parser);
        *p_parser = NULL;
    }

    return err;
}
