/** crx_parse.c - Compressed RINEX parsing utilities.
 * Copyright 2023 Michael Poole.
 * SPDX-License-Identifier: MIT-Modern-Variant
 */

#include "rinex/rnx_priv.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/** crx_parse_int64 parses a signed decimal integer.
 * \param[out] p_out Receives the parsed value.
 * \param[in] s Input pointer, positioned at the first character.
 * \returns Pointer past the parsed number, or NULL on error.
 */
static const char *crx_parse_int64(int64_t *p_out, const char *s)
{
    int64_t val = 0;
    int neg = 0;

    if (*s == '-') { neg = 1; s++; }
    if (*s < '0' || *s > '9') return NULL;
    while (*s >= '0' && *s <= '9')
        val = val * 10 + (*s++ - '0');
    *p_out = neg ? -val : val;
    return s;
}

/** crx_ensure_obs ensures the CRX parser can hold at least \a n
 * observation slots in obs/lli/ssi/order/diff/sat_flags.
 *
 * \param[in,out] crx The compressed-RINEX object.
 * \param[in] n Minimum total number of observations to allow for \a crx.
 */
static rinex_error_t crx_ensure_obs(struct crx_v23_parser *crx, int n)
{
    struct rnx_v234_parser *p = &crx->base;
    int64_t *new_diff;
    int old_alloc, new_alloc, kk;

    if (n <= p->obs_alloc)
        return RINEX_SUCCESS;

    old_alloc = p->obs_alloc;
    new_alloc = old_alloc ? old_alloc : 500;
    while (new_alloc < n)
        new_alloc *= 2;

    p->base.lli = realloc(p->base.lli, new_alloc);
    p->base.ssi = realloc(p->base.ssi, new_alloc);
    p->base.obs = realloc(p->base.obs, new_alloc * sizeof(int64_t));
    crx->order = realloc(crx->order, new_alloc * 2);
    crx->sat_flags = realloc(crx->sat_flags, new_alloc * 2);
    if (!p->base.lli || !p->base.ssi || !p->base.obs
        || !crx->order || !crx->sat_flags)
    {
        p->base.error_line = __LINE__;
        return RINEX_ERR_SYSTEM;
    }
    p->obs_alloc = new_alloc;

    /* Reallocate diff in order-major layout: must move existing data. */
    new_diff = calloc(new_alloc, 10 * sizeof(int64_t));
    if (!new_diff)
    {
        p->base.error_line = __LINE__;
        return RINEX_ERR_SYSTEM;
    }
    if (crx->diff)
    {
        for (kk = 0; kk < 10; ++kk)
            memcpy(new_diff + kk * new_alloc,
                    crx->diff + kk * old_alloc,
                    old_alloc * sizeof(int64_t));
        free(crx->diff);
    }
    crx->diff = new_diff;

    /* Zero-fill new slots in order and sat_flags. */
    memset(crx->order + old_alloc * 2, 0, (new_alloc - old_alloc) * 2);
    memset(crx->sat_flags + old_alloc * 2, 0, (new_alloc - old_alloc) * 2);

    return RINEX_SUCCESS;
}

/** crx_decompress_obs decompresses one satellite's observation line.
 *
 * Splits the line into n_obs fields separated by spaces, then applies
 * differential decompression.
 * The remainder of the line after n_obs fields is the flag string,
 * processed with repair semantics.
 *
 * \param[in,out] crx The compressed-RINEX object.
 * \param[in] line Start of the satellite's observation data line.
 * \param[in] base Index into diff/order arrays for this satellite's first obs.
 * \param[in] n_obs Number of observables for this satellite's system.
 * \param[in] is_init True if this is an initialization epoch.
 * \returns Pointer past the newline, or NULL on parse error.
 */
static const char *crx_decompress_obs(
    struct crx_v23_parser *crx,
    const char *line,
    int base,
    int n_obs,
    int is_init)
{
    struct rinex_parser *p = &crx->base.base;
    int obs_alloc = crx->base.obs_alloc;
    int ii, k, arc_order, cur_order;
    int64_t val;
    const char *s, *next_line, *field_start;

    /* Find end of line. */
    next_line = line;
    while (*next_line != '\n' && *next_line != '\0')
        next_line++;

    /* Split the line into n_obs fields using the reference convention:
     * walk character by character; each space or end-of-line terminates
     * a field.  Consecutive spaces produce empty fields (= missing obs).
     * After n_obs fields, remaining characters are the flag string.
     *
     * This matches crx2rnx.c getdiff() behavior exactly.
     */
    s = line;
    for (ii = 0; ii < n_obs; ++ii)
    {
        int d = base + ii;

        /* Find the start and end of this field. */
        field_start = s;
        while (s < next_line && *s != ' ')
            s++;
        /* s now points to a space or next_line. */

        if (field_start == s)
        {
            /* Empty field: observation is missing/blank. */
            crx->order[2 * d + 1] = 0;
            p->obs[d] = 0;
        }
        else if (field_start[0] >= '0' && field_start[0] <= '9'
                 && (field_start + 1) < s && field_start[1] == '&')
        {
            /* Arc initialization: N&value */
            arc_order = field_start[0] - '0';
            if (!crx_parse_int64(&val, field_start + 2))
                return NULL;

            crx->order[2 * d + 0] = arc_order;
            crx->order[2 * d + 1] = 1;
            crx->diff[d + 0 * obs_alloc] = val;
            for (k = 1; k <= arc_order; ++k)
                crx->diff[d + k * obs_alloc] = 0;

            p->obs[d] = val;
        }
        else if (*field_start == '-' || (*field_start >= '0' && *field_start <= '9'))
        {
            /* Continuation: delta value. */
            if (!crx_parse_int64(&val, field_start))
                return NULL;

            cur_order = crx->order[2 * d + 1] - 1;
            arc_order = crx->order[2 * d + 0];

            if (cur_order < arc_order)
            {
                crx->diff[d + (cur_order + 1) * obs_alloc] = val;
                crx->order[2 * d + 1] = cur_order + 2;
            }
            else
            {
                crx->diff[d + arc_order * obs_alloc] += val;
            }

            for (k = crx->order[2 * d + 1] - 1; k > 0; --k)
                crx->diff[d + (k - 1) * obs_alloc] += crx->diff[d + k * obs_alloc];
            p->obs[d] = crx->diff[d + 0 * obs_alloc];
        }
        else
        {
            /* Unrecognized field content. */
            return NULL;
        }

        /* Advance past the field separator (space). */
        if (s < next_line)
            s++;
    }

    /* Everything from s to next_line is the flag string.
     * For init: '&' means space, other chars are literal.
     * For delta: repair semantics (space=keep, &=space, else=replace).
     */
    for (ii = 0; ii < n_obs; ++ii)
    {
        int d = base + ii;
        if (s >= next_line)
        {
            /* No more flag data. */
            if (is_init)
            {
                crx->sat_flags[2 * d + 0] = ' ';
                crx->sat_flags[2 * d + 1] = ' ';
            }
        }
        else if (is_init)
        {
            crx->sat_flags[2 * d + 0] = (*s == '&') ? ' ' : *s;
            s++;
            if (s < next_line)
            {
                crx->sat_flags[2 * d + 1] = (*s == '&') ? ' ' : *s;
                s++;
            }
            else
            {
                crx->sat_flags[2 * d + 1] = ' ';
            }
        }
        else
        {
            if (*s == '&')
                crx->sat_flags[2 * d + 0] = ' ';
            else if (*s != ' ')
                crx->sat_flags[2 * d + 0] = *s;
            s++;
            if (s < next_line)
            {
                if (*s == '&')
                    crx->sat_flags[2 * d + 1] = ' ';
                else if (*s != ' ')
                    crx->sat_flags[2 * d + 1] = *s;
                s++;
            }
        }
        p->lli[d] = crx->sat_flags[2 * d + 0];
        p->ssi[d] = crx->sat_flags[2 * d + 1];
    }

    if (*next_line == '\n')
        next_line++;
    return next_line;
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

/** crx_v2_build_sattbl builds a satellite reorder table.
 *
 * Compares the new satellite list (in epoch_text at offset 32) with the
 * previous one (in prev_svs), and fills sattbl[i] with the index of the
 * i-th new satellite in the old list, or -1 if it is new.
 *
 * \param[in,out] crx CRX parser.
 * \param[in] prev_svs Previous satellite list (2 bytes each: sys + PRN).
 * \param[in] n_prev Number of satellites in the previous epoch.
 * \param[out] sattbl Receives the reorder table, length n_new.
 * \param[in] n_new Number of satellites in the new epoch.
 */
static void crx_v2_build_sattbl(
    struct crx_v23_parser *crx,
    const char *prev_svs,
    int n_prev,
    int *sattbl,
    int n_new)
{
    const char *pos = crx->epoch_text + 32;
    int ii, jj;
    char sys, svn;

    for (ii = 0; ii < n_new; ++ii, pos += 3)
    {
        sys = pos[0];
        svn = (pos[1] - '0') * 10 + (pos[2] - '0');
        sattbl[ii] = -1;
        for (jj = 0; jj < n_prev; ++jj)
        {
            if (prev_svs[2 * jj + 0] == sys && prev_svs[2 * jj + 1] == svn)
            {
                sattbl[ii] = jj;
                break;
            }
        }
    }
}

/** crx_v2_read_obs reads and decompresses observations for a v2 CRX epoch.
 *
 * Reads the clock offset line plus n_sats observation lines from the
 * stream, handles satellite reordering via sattbl, and calls
 * crx_decompress_obs for each satellite.
 *
 * \param crx CRX parser.
 * \param is_init True for initialization epochs.
 * \param sattbl Satellite reorder table (or NULL for init epochs).
 * \returns rinex_error_t status code.
 */
static rinex_error_t crx_v2_read_obs(
    struct crx_v23_parser *crx,
    int is_init,
    const int *sattbl)
{
    struct rnx_v234_parser *p = &crx->base;
    struct rinex_parser *p_ = &p->base;
    const char *obs, *pos;
    int res, ii, nn, n_sats, n_obs;
    rinex_error_t err;

    n_sats = p_->epoch.n_sats;
    n_obs = p_->n_obs[' ' & 31]; /* v2: uniform observation count */

    /* Read n_sats observation lines (clock offset was already consumed
     * as part of the 2-line epoch header read in crx_read_v2).
     */
    res = rnx_get_newlines(p_, &p->parse_ofs, NULL, 0, n_sats);
    if (res <= RINEX_EOF)
    {
        p_->error_line = __LINE__;
        return RINEX_ERR_BAD_FORMAT;
    }
    obs = p_->stream->buffer + p->parse_ofs;

    /* Ensure sats array capacity. */
    err = rnx_ensure_sats(p);
    if (err != RINEX_SUCCESS)
    {
        p->parse_ofs = res;
        return err;
    }

    /* Ensure obs/diff arrays capacity. */
    nn = n_sats * n_obs;
    err = crx_ensure_obs(crx, nn);
    if (err != RINEX_SUCCESS)
    {
        p->parse_ofs = res;
        return err;
    }

    /* Build satellite info and decompress each satellite's line.
     * For delta epochs with satellite reordering, we need to copy
     * diff state from old positions to new positions.
     */
    if (!is_init && sattbl)
    {
        /* Shuffle diff state according to sattbl.
         * sattbl[i] = old index for satellite i, or -1 for new.
         * We need a temp copy since indices may overlap.
         */
        int obs_alloc = p->obs_alloc;
        int old_nn = crx->base.base.epoch.n_sats * n_obs;
        unsigned char *new_order = calloc(obs_alloc, 2);
        int64_t *new_diff = calloc(obs_alloc, 10 * sizeof(int64_t));
        char *new_flags = calloc(obs_alloc, 2);
        if (!new_order || !new_diff || !new_flags)
        {
            free(new_order);
            free(new_diff);
            free(new_flags);
            p_->error_line = __LINE__;
            p->parse_ofs = res;
            return RINEX_ERR_SYSTEM;
        }

        for (ii = 0; ii < n_sats; ++ii)
        {
            int new_base = ii * n_obs;
            if (sattbl[ii] >= 0)
            {
                int old_base = sattbl[ii] * n_obs;
                int kk;
                memcpy(new_order + new_base * 2, crx->order + old_base * 2, n_obs * 2);
                memcpy(new_flags + new_base * 2, crx->sat_flags + old_base * 2, n_obs * 2);
                for (kk = 0; kk < 10; ++kk)
                    memcpy(new_diff + new_base + kk * obs_alloc,
                           crx->diff + old_base + kk * obs_alloc,
                           n_obs * sizeof(int64_t));
            }
            /* else: new satellite — new_order/new_diff already zeroed */
        }

        memcpy(crx->order, new_order, obs_alloc * 2);
        memcpy(crx->sat_flags, new_flags, obs_alloc * 2);
        memcpy(crx->diff, new_diff, obs_alloc * 10 * sizeof(int64_t));
        free(new_order);
        free(new_diff);
        free(new_flags);
    }
    else if (is_init)
    {
        /* Reset all diff state. */
        memset(crx->order, 0, p->obs_alloc * 2);
        memset(crx->sat_flags, 0, p->obs_alloc * 2);
    }

    /* Parse the satellite list from epoch_text and decompress. */
    pos = crx->epoch_text + 32;
    nn = 0;
    for (ii = 0; ii < n_sats; ++ii)
    {
        /* V2 epoch header has 12 SVs per line; continuation lines
         * start at position 32 after the epoch header line.
         * In the epoch_text, the full SV list is contiguous at pos 32+.
         */
        p_->sats[ii].system = pos[0];
        p_->sats[ii].number = (pos[1] - '0') * 10 + (pos[2] - '0');
        p_->sats[ii].obs_0 = nn;
        pos += 3;

        obs = crx_decompress_obs(crx, obs, nn, n_obs, is_init);
        if (!obs)
        {
            p_->error_line = __LINE__;
            p->parse_ofs = res;
            return RINEX_ERR_BAD_FORMAT;
        }
        nn += n_obs;
    }

    /* Save the current satellite list for the next epoch's reordering.
     * Store in p->base.buffer as packed 2-byte entries (sys + binary PRN).
     */
    while (p->buffer_alloc < n_sats * 2)
        p->buffer_alloc <<= 1;
    p_->buffer = realloc(p_->buffer, p->buffer_alloc);
    if (!p_->buffer)
    {
        p_->error_line = __LINE__;
        p->parse_ofs = res;
        return RINEX_ERR_SYSTEM;
    }
    for (ii = 0; ii < n_sats; ++ii)
    {
        p_->buffer[2 * ii + 0] = p_->sats[ii].system;
        p_->buffer[2 * ii + 1] = p_->sats[ii].number;
    }
    p_->buffer_len = n_sats * 2;

    p->parse_ofs = res;
    return RINEX_SUCCESS;
}

/** crx_read_v2 reads an observation data record from \a p_. */
static rinex_error_t crx_read_v2(struct rinex_parser *p_)
{
    struct crx_v23_parser *crx = (struct crx_v23_parser *)p_;
    struct rnx_v234_parser *p = &crx->base;
    const char *line;
    int res, err, line_len, ii;
    int sattbl[100]; /* MAXSAT */

    /* CRXv2 epoch headers have date+time & SV list, then clock offset. */
    res = rnx_get_newlines(p_, &p->parse_ofs, NULL, 0, 2);
    if (res < 21)
    {
        if (res <= RINEX_EOF)
        {
            return res;
        }

        /* At EOF, try getting a single line instead. */
        if ((res > RINEX_EOF) || ((res = rnx_get_newlines(p_, &p->parse_ofs, NULL, 0, 1)) < 20))
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

    /* What is the line format? */
    if (line[0] == ' ') /* delta header line */
    {
        int old_n_sats = p_->epoch.n_sats;

        crx_line_space(crx, line_len);

        /* Apply the update(s) to the header line. */
        for (ii = 1; ii < line_len; ++ii)
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
        ii = strlen(crx->epoch_text);
        while (ii > 0 && crx->epoch_text[ii - 1] == ' ')
            ii--;
        crx->epoch_text[ii] = '\0';

        /* Parse the timestamp, epoch flag, etc. */
        err = rnx_v2_parse_time(p, crx->epoch_text);
        if (err < 0)
        {
            return err;
        }

        /* Build satellite reorder table for delta epochs. */
        crx_v2_build_sattbl(crx, p_->buffer, old_n_sats,
            sattbl, p_->epoch.n_sats);

        /* Advance past the two lines already read (epoch header + next). */
        p->parse_ofs = res;

        /* Read and decompress the observations. */
        return crx_v2_read_obs(crx, 0, sattbl);
    }
    else if (line[0] != '&' || line_len < 32) /* must be a full init header */
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

        /* Advance past the two lines already read (epoch + clock). */
        p->parse_ofs = res;

        /* Read and decompress observations (full initialization). */
        return crx_v2_read_obs(crx, 1, NULL);
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

/** crx_v34_parse_epoch parses the epoch fields from crx->epoch_text.
 * \returns 0 on success, negative rinex_error_t on failure.
 */
static int crx_v34_parse_epoch(struct crx_v23_parser *crx)
{
    struct rnx_v234_parser *p = &crx->base;
    const char *line = crx->epoch_text;
    int64_t sec;
    int yy, mm, dd, hh, min, n_sats, line_len;

    line_len = strlen(line);
    if (line_len < 35 || line[0] != '>'
        || line[31] < '0' || line[31] > '6'
        || parse_uint(&yy, line + 2, 4)
        || parse_uint(&mm, line + 7, 2)
        || parse_uint(&dd, line + 10, 2)
        || parse_uint(&hh, line + 13, 2)
        || parse_uint(&min, line + 16, 2)
        || parse_uint(&n_sats, line + 32, 3)
        || parse_fixed(&sec, line + 18, 11, 7))
    {
        p->base.error_line = __LINE__;
        return RINEX_ERR_BAD_FORMAT;
    }

    p->base.epoch.yyyy_mm_dd = (yy * 100 + mm) * 100 + dd;
    p->base.epoch.hh_mm = hh * 100 + min;
    p->base.epoch.sec_e7 = sec;
    p->base.epoch.flag = line[31];
    p->base.epoch.n_sats = n_sats;

    /* Clock offset from the epoch line (cols 41+). */
    if (line_len <= 43)
    {
        p->base.epoch.clock_offset = 0;
    }
    else if (line_len <= 56)
    {
        if (parse_fixed(&p->base.epoch.clock_offset, line + 41,
            line_len - 41, line_len - 44))
        {
            p->base.error_line = __LINE__;
            return RINEX_ERR_BAD_FORMAT;
        }
    }
    else
    {
        p->base.epoch.clock_offset = 0;
    }

    return 0;
}

/** crx_read_v34 reads an observation data record from \a p_. */
static rinex_error_t crx_read_v34(struct rinex_parser *p_)
{
    struct crx_v23_parser *crx = (struct crx_v23_parser *)p_;
    struct rnx_v234_parser *p = &crx->base;
    const char *line, *obs;
    int res, line_len, ii, nn, n_sats, n_obs_sys, is_init;
    rinex_error_t err;

    /* Get the epoch header line. */
    res = rnx_get_newlines(p_, &p->parse_ofs, NULL, 0, 1);
    if (res <= RINEX_EOF)
        return res;
    line = p_->stream->buffer + p->parse_ofs;
    line_len = strchr(line, '\n') - line;

    if (line_len < 2)
    {
        p_->error_line = __LINE__;
        p->parse_ofs = res;
        return RINEX_ERR_BAD_FORMAT;
    }

    is_init = (line[0] == '>');

    if (is_init)
    {
        /* Full initialization: copy entire epoch line to epoch_text. */
        crx_line_space(crx, line_len);
        memcpy(crx->epoch_text, line, line_len);
        crx->epoch_text[line_len] = '\0';
    }
    else
    {
        /* Delta: apply repair to epoch_text. */
        crx_line_space(crx, line_len);
        for (ii = 0; ii < line_len; ++ii)
        {
            if (line[ii] == '&')
                crx->epoch_text[ii] = ' ';
            else if (line[ii] != ' ')
                crx->epoch_text[ii] = line[ii];
        }
        /* Trim trailing spaces. */
        ii = strlen(crx->epoch_text);
        while (ii > 0 && crx->epoch_text[ii - 1] == ' ')
            ii--;
        crx->epoch_text[ii] = '\0';
    }

    /* Parse epoch fields from epoch_text. */
    err = crx_v34_parse_epoch(crx);
    if (err < 0)
    {
        p->parse_ofs = res;
        return err;
    }
    n_sats = p_->epoch.n_sats;
    p->parse_ofs = res;

    /* Handle special events (flag 2-5). */
    if (p_->epoch.flag >= '2' && p_->epoch.flag <= '5')
    {
        res = rnx_get_newlines(p_, &p->parse_ofs, NULL, 0, n_sats);
        if (res <= RINEX_EOF)
        {
            p_->error_line = __LINE__;
            return RINEX_ERR_BAD_FORMAT;
        }
        err = rnx_copy_text(p, res);
        p->parse_ofs = res;
        return err;
    }

    /* Read clock offset line + n_sats observation lines. */
    res = rnx_get_newlines(p_, &p->parse_ofs, NULL, 0, 1 + n_sats);
    if (res <= RINEX_EOF)
    {
        p_->error_line = __LINE__;
        return RINEX_ERR_BAD_FORMAT;
    }
    obs = p_->stream->buffer + p->parse_ofs;

    /* Skip the clock offset line. */
    obs = memchr(obs, '\n', res - p->parse_ofs);
    if (!obs)
    {
        p_->error_line = __LINE__;
        p->parse_ofs = res;
        return RINEX_ERR_BAD_FORMAT;
    }
    obs++;

    /* Ensure sats array is large enough. */
    err = rnx_ensure_sats(p);
    if (err != RINEX_SUCCESS)
    {
        p->parse_ofs = res;
        return err;
    }

    /* Count total observation slots needed. */
    nn = 0;
    for (ii = 0; ii < n_sats; ++ii)
    {
        char sys = crx->epoch_text[41 + 3 * ii];
        nn += p_->n_obs[sys & 31];
    }

    /* Ensure observation arrays are large enough. */
    err = crx_ensure_obs(crx, nn);
    if (err != RINEX_SUCCESS)
    {
        p->parse_ofs = res;
        return err;
    }

    /* On init epochs, reset diff state for all slots. */
    if (is_init)
    {
        memset(crx->order, 0, p->obs_alloc * 2);
        memset(crx->sat_flags, 0, p->obs_alloc * 2);
    }

    /* Decompress each satellite's observation line. */
    nn = 0;
    for (ii = 0; ii < n_sats; ++ii)
    {
        char sys = crx->epoch_text[41 + 3 * ii];
        int svn = (crx->epoch_text[42 + 3 * ii] - '0') * 10
                + (crx->epoch_text[43 + 3 * ii] - '0');
        n_obs_sys = p_->n_obs[sys & 31];

        p_->sats[ii].system = sys;
        p_->sats[ii].number = svn;
        p_->sats[ii].obs_0 = nn;

        obs = crx_decompress_obs(crx, obs, nn, n_obs_sys, is_init);
        if (!obs)
        {
            p_->error_line = __LINE__;
            p->parse_ofs = res;
            return RINEX_ERR_BAD_FORMAT;
        }
        nn += n_obs_sys;
    }

    p->parse_ofs = res;
    return RINEX_SUCCESS;
}

/** crx_free_v23 deallocates \a p_, which must be a crx_v23_parser. */
void crx_free_v23(struct rinex_parser *p_)
{
    struct crx_v23_parser *p = (struct crx_v23_parser *)p_;

    free(p->sat_flags);
    free(p->diff);
    free(p->order);
    free(p->epoch_text);
    rnx_free_v23(p_);
}

const char *crx_open_v23(
    struct crx_v23_parser *crx,
    struct rinex_stream *stream)
{
    const char *err;
    int ofs;

    /* Sanity check the format. */
    if (memcmp("COMPACT RINEX FORMAT", stream->buffer + 20, 20))
    {
        return "Unexpected CRX header line";
    }

    /* Find the RINEX VERSION / TYPE line to get the RINEX version. */
    ofs = rnx_find_header(stream->buffer, stream->size, rnx_version_type, 21);
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
        err = "Unsupported RINEX version for CRX";
    }

    /* Allocate decompression fields when we successfully set a CRX reader. */
    if (!err)
    {
        crx->epoch_alloc = 200;
        crx->epoch_text = calloc(crx->epoch_alloc, 1);
        crx->order = calloc(crx->base.obs_alloc, 2);
        crx->diff = calloc(crx->base.obs_alloc, 10 * sizeof(int64_t));
        crx->sat_flags = calloc(crx->base.obs_alloc, 2);
        if (!crx->epoch_text || !crx->order || !crx->diff || !crx->sat_flags)
        {
            err = "Memory allocation failed";
        }
    }

    return err;
}
