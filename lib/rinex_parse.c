/** rinex_parse.c - RINEX parsing utilities.
 * Copyright 2020-2021 Michael Poole.
 * SPDX-License-Identifier: MIT-Modern-Variant
 */

#include "lib/rinex_p.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

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

static int rnx_v2_parse_time(struct rnx_v234_parser *p, const char *line)
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

/** rnx_read_v2 reads an observation data record from \a p_. */
static rinex_error_t rnx_read_v2(struct rinex_parser *p_)
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
    if (res < 33)
    {
        p_->error_line = __LINE__;
        return RINEX_ERR_BAD_FORMAT;
    }
    line_len = res - 1 - p->parse_ofs;
    line = p->base.stream->buffer + p->parse_ofs;

    /* Parse the timestamp, epoch flag and "number of satellites" field. */
    res = rnx_v2_parse_time(p, line);
    if (res < 0)
    {
        return res;
    }

    /* Is there a receiver clock offset? */
    if (line_len <= 68)
    {
        p->base.epoch.clock_offset = 0;
    }
    else if (line_len == 80)
    {
        if (parse_fixed(&p->base.epoch.clock_offset, line+68, 12, 9))
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
                res = RINEX_ERR_BAD_FORMAT;
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
            p_->error_line = __LINE__;
            err = res;
        }
        else
        {
            err = rnx_copy_text(p, res);
            if (err == RINEX_SUCCESS)
            {
                p->parse_ofs = res;
            }
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

/** rnx_read_v34 reads an observation data record from \a p_. */
static rinex_error_t rnx_read_v34(struct rinex_parser *p_)
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
    if (res < 35)
    {
        p_->error_line = __LINE__;
        return RINEX_ERR_BAD_FORMAT;
    }
    line_len = res - p->parse_ofs;
    line = p->base.stream->buffer + p->parse_ofs;
    p->parse_ofs = res;

    /* Parse the timestamp, epoch flag and "number of satellites" field. */
    if ((line[0] != '>' || line[31] < '0' || line[31] > '6')
        || parse_uint(&yy, line+2, 4) || parse_uint(&mm, line+7, 2)
        || parse_uint(&dd, line+10, 2) || parse_uint(&hh, line+13, 2)
        || parse_uint(&min, line+16, 2) || parse_uint(&n_sats, line+32, 3)
        || parse_fixed(&i64, line+18, 11, 7))
    {
        p_->error_line = __LINE__;
        return RINEX_ERR_BAD_FORMAT;
    }
    p->base.epoch.yyyy_mm_dd = (yy * 100 + mm) * 100 + dd;
    p->base.epoch.hh_mm = hh * 100 + min;
    p->base.epoch.sec_e7 = i64;
    p->base.epoch.flag = line[28];
    p->base.epoch.n_sats = n_sats;

    /* Is there a receiver clock offset? */
    if (line_len <= 44)
    {
        p->base.epoch.clock_offset = 0;
    }
    else if (line_len == 59)
    {
        if (parse_fixed(&p->base.epoch.clock_offset, line+44, 15, 12))
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

/** rnx_free_v23 deallocates \a p_, which must be a rnx_v234_parser. */
static void rnx_free_v23(struct rinex_parser *p_)
{
    struct rnx_v234_parser *p = (struct rnx_v234_parser *)p_;

    free(p->base.buffer);
    free(p->base.lli);
    free(p->base.ssi);
    free(p->base.obs);
    free(p);
}

/** crx_line_space ensures \a crx->epoch_text >= \a line_len. */
static int crx_line_space(struct crx_v23_parser *crx, int line_len)
{
    if (crx->epoch_alloc <= line_len)
    {
        int new_len = crx->epoch_alloc * 2;
        if (new_len <= line_len)
            new_len = line_len + 1;
        crx->epoch_text = realloc(crx->epoch_text, new_len);
        if (!crx->epoch_text)
            return RINEX_ERR_SYSTEM;
    }

    return 0;
}

/** crx_read_v2_move_sv moves a satellite. */
static int crx_read_v2_move_sv(
    struct crx_v23_parser *crx,
    int idx,
    int n_obs,
    const char sv[2]
)
{
    int jj, kk;

    /* We found a change.  If the satelite already has history, it will
     * be later in the array, so we swap the two.
     */
    for (jj = idx + 1; jj < crx->base.base.epoch.n_sats; ++jj)
    {
        if (crx->base.base.buffer[2*jj+0] == sv[0]
            && crx->base.base.buffer[2*jj+1] == sv[1])
        {
            break;
        }
    }

    /* Is the satellite new? */
    if (jj == crx->base.base.epoch.n_sats)
    {
        /* TODO: May need to increase buffer sizes. */
    }
    else /* "Swap" satellites idx and jj. */
    {
        /* In practice, we have to shift the observations from idx+1
         * through jj (inclusive) down to idx.
         */
        for (kk = 0; kk < n_obs; ++kk) ;
        /* crx->base.base.lli[...] */
    }

    /* cf. mty22000.20d line 1774 vs mty22000.20o line 8080,
     * then .20d line 1818 vs .20o line 8284,
     * or .20d line 10499 vs .20o line 48673
     */

    return 0;
}

/** crx_read_v2_sv_list checks an updated CRX satellite list. */
static int crx_read_v2_sv_list(struct crx_v23_parser *crx)
{
    char *old, *pos;
    int n_sat, n_obs, ii;
    char sv[2];

    /* How many satellites are listed now? */
    if (parse_uint(&n_sat, crx->epoch_text + 30, 2))
    {
        crx->base.base.error_line = __LINE__;
        return RINEX_ERR_BAD_FORMAT;
    }

    /* Check for changes in the SV list. */
    old = crx->base.base.buffer;
    pos = crx->epoch_text + 32;
    n_obs = crx->base.base.n_obs[old[0] & 31]; /* constant for RINEX v2 */
    for (ii = 0; ii < n_sat; ++ii)
    {
        sv[0] =  pos[0];
        sv[1] = (pos[1] - '0') * 10 + (pos[2] - '0');
        if (sv[0] != old[0] || sv[1] != old[1])
        {
            crx_read_v2_move_sv(crx, ii, n_obs, sv);
        }

        old += 2 + (n_obs + 7) / 8;
        pos += 3;
    }

    /* Did the number of satellites go down? */
    if (n_sat < crx->base.base.epoch.n_sats)
    {
        crx->base.base.epoch.n_sats = n_sat;
    }

    return 0;
}

/** crx_read_v2 reads an observation data record from \a p_. */
static rinex_error_t crx_read_v2(struct rinex_parser *p_)
{
    struct crx_v23_parser *crx = (struct crx_v23_parser *)p_;
    struct rnx_v234_parser *p = &crx->base;
    const char *line;
    int res, err, line_len, ii;

    /* CRXv2 epoch headers have date+time & SV list, then clock offset. */
    res = rnx_get_newlines(p_, &p->parse_ofs, NULL, 0, 2);
    if (res < 21)
    {
        if (res <= RINEX_EOF)
        {
            return res;
        }

        /* At EOF, try getting a single line instead. */
        if ((res > RINEX_EOF)
            || ((res = rnx_get_newlines(p_, &p->parse_ofs, NULL, 0, 1)) < 20))
        {
            /* error_line already set by rnx_get_newlines() */
        }
        /* Getting here should mean a single-line special event at EOF.
        * These are represented as full initialization lines, but
        * with an epoch flag of 2 through 5 inclusive.
        */
        else
        {
           line = p_->stream->buffer + p->parse_ofs;
            if (line[0] == '&' && line[28] >= '2' && line[28] <= '5')
            {
crx_v2_copy_text:
                res = rnx_copy_text(p, res);
                if (res == RINEX_SUCCESS)
                {
                    /* Replace leading '&' with a space to be RINEX. */
                    p->base.buffer[0] = ' ';
                    p->parse_ofs = res;
                }
                return res;
            }
            else /* The file looks corrupt or truncated. */
            {
                p_->error_line = __LINE__;
            }
        }

        return RINEX_ERR_BAD_FORMAT;
    }
    line = p_->stream->buffer + p->parse_ofs;
    line_len = strchr(line, '\n') - line;

    /* A valid event header has at least event type and line count. */
    if (line_len < 32)
    {
        p_->error_line = __LINE__;
        return RINEX_ERR_BAD_FORMAT;
    }

    /* What is the line format? */
    if (line[0] == ' ') /* delta header line */
    {
        crx_line_space(crx, res);

        /* Apply the update(s) to the header line. */
        for (ii = 1; ii < res; ++ii)
        {
            if (line[ii] == ' ')
            {
                /* leave the existing character */
            }
            else if (line[ii] == '&')
            {
                crx->epoch_text[ii] = ' ';
            }
            else
            {
                crx->epoch_text[ii] = line[ii];
            }
        }

        /* Discard any residual trailing spaces. */
        while (crx->epoch_text[ii-1] == ' ')
            ii--;
        crx->epoch_text[ii] = '\0';

        /* Parse the timestamp, epoch flag, etc. */
        err = rnx_v2_parse_time(p, crx->epoch_text);
        if (err == 0 && ii > 33)
        {
            err = crx_read_v2_sv_list(crx);
        }
        if (err < 0)
        {
            return err;
        }

        /* TODO: Handle the compressed data record.  The first line is
         * mandatorily blank.
         */
        return RINEX_SUCCESS;
    }
    else if (line[0] != '&') /* otherwise should be a full initialization header line */
    {
        p_->error_line = __LINE__;
        return RINEX_ERR_BAD_FORMAT;
    }
    else if (line[28] == '0' || line[28] == '1') /* epoch flag 0 or 1 */
    {
        crx_line_space(crx, res);

        /* Copy the initialization header line. */
        memcpy(crx->epoch_text + 1, line + 1, res - 1);
        crx->epoch_text[0] = ' ';
        crx->epoch_text[res] = '\0';

        /* Parse the timestamp, epoch flag, etc. */
        err = rnx_v2_parse_time(p, crx->epoch_text);
        if (err < 0)
        {
            return err;
        }

        /* TODO: Fully initialize decompression state.
         * The next line is mandatorily blank.
         */
        return RINEX_SUCCESS;
    }
    else /* epoch flag > 1, a special event record */
    {
        int n_lines;

        /* How many lines in this special event record? */
        if (parse_uint(&n_lines, line + 29, 3))
        {
            p_->error_line = __LINE__;
            return RINEX_ERR_BAD_FORMAT;
        }
        res = rnx_get_newlines(p_, &p->parse_ofs, NULL, 0, n_lines);
        if (res <= RINEX_EOF)
        {
            return RINEX_ERR_BAD_FORMAT;
        }
        goto crx_v2_copy_text;
    }
}

/** crx_read_v34 reads an observation data record from \a p_. */
static rinex_error_t crx_read_v34(struct rinex_parser *p_)
{
    struct crx_v23_parser *crx = (struct crx_v23_parser *)p_;
    struct rnx_v234_parser *p = &crx->base;

    (void)p;
    return RINEX_SUCCESS;
}

/** crx_free_v23 deallocates \a p_, which must be a crx_v23_parser. */
static void crx_free_v23(struct rinex_parser *p_)
{
    struct crx_v23_parser *p = (struct crx_v23_parser *)p_;

    free(p->diff);
    free(p->order);
    free(p->epoch_text);
    rnx_free_v23(p_);
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

static const char rinex_version_type[] = "RINEX VERSION / TYPE";

const char *crx_open_v23
(
    struct crx_v23_parser *crx,
    struct rinex_stream *stream
)
{
    const char *err;
    int ofs;

    /* Sanity check the format. */
    if (memcmp("COMPACT RINEX FORMAT", stream->buffer + 20, 20))
    {
        return "Unexpected CRX header line";
    }

    /* Find the RINEX VERSION / TYPE line to get the RINEX version. */
    ofs = rnx_find_header(stream->buffer, stream->size, rinex_version_type, 20);
    if (ofs < 1)
    {
        return "Could not find RINEX VERSION / TYPE";
    }

    err = rnx_open_v23(&crx->base, stream, ofs);
    if (err)
    {
        /* report that error */
    }
    else if (crx->base.base.read == rnx_read_v2)
    {
        if (memcmp("1.0 ", stream->buffer, 4))
        {
            err = "Expected CRINEX 1.0 for RINEX v2.x";
        }
        else
        {
            crx->base.base.read = crx_read_v2;
        }
    }
    else if (crx->base.base.read == rnx_read_v34)
    {
        if (memcmp("3.0 ", stream->buffer, 4))
        {
            err = "Expected CRINEX 3.0 for RINEX v3.x/v4.x";
        }
        else
        {
            crx->base.base.read = crx_read_v34;
        }
    }
    else
    {
        /* Allocate RINEX decompression fields. */
        crx->epoch_alloc = 200;
        crx->epoch_text = calloc(crx->epoch_alloc, 1);
        crx->order = calloc(crx->base.obs_alloc, 2);
        crx->diff = calloc(crx->base.obs_alloc, 10 * sizeof(int));
        if (!crx->epoch_text || !crx->order || !crx->diff)
        {
            err = "Memory allocation failed";
        }
    }

    return err;
}

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
        return strerror(errno);
    }

    /* Is it an uncompressed RINEX file? */
    if (!memcmp(rinex_version_type, stream->buffer + 60, 20))
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
