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
    int old_alloc, new_alloc, ii;

    if (n <= p->obs_alloc)
        return RINEX_SUCCESS;

    old_alloc = p->obs_alloc;
    new_alloc = old_alloc ? old_alloc : 500;
    while (new_alloc < n)
        new_alloc *= 2;

    p->base.lli = realloc(p->base.lli, new_alloc);
    p->base.ssi = realloc(p->base.ssi, new_alloc);
    p->base.obs = realloc(p->base.obs, new_alloc * sizeof(int64_t));
    crx->state = realloc(crx->state, new_alloc * sizeof(struct obs_state));
    crx->prev_state = realloc(crx->prev_state, new_alloc * sizeof(struct obs_state));
    if (!p->base.lli || !p->base.ssi || !p->base.obs
        || !crx->state || !crx->prev_state)
    {
        p->base.error_line = __LINE__;
        return RINEX_ERR_SYSTEM;
    }
    p->obs_alloc = new_alloc;

    /* Zero-fill new slots. */
    memset(crx->state + old_alloc, 0, (new_alloc - old_alloc) * sizeof(struct obs_state));

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
    int ii, d, arc_order, cur_order;
    int64_t val;
    const char *s = line;

    /* Split the line into n_obs fields using the reference convention:
     * walk character by character; each space or end-of-line terminates
     * a field.  Consecutive spaces produce empty fields (= missing obs).
     * After n_obs fields, remaining characters are the flag string.
     *
     * This matches crx2rnx.c getdiff() behavior exactly.
     */
    for (ii = 0, d = base; ii < n_obs; ++ii, ++d)
    {
        if (*s == ' ' || *s == '\n' || *s == '\0')
        {
            /* Empty field: observation is missing/blank. */
            crx->state[d].used = 0;
            p->obs[d] = 0;
        }
        else if (s[1] == '&')
        {
            /* Arc initialization: N&value */
            arc_order = s[0] - '0';
            s = crx_parse_int64(&val, s + 2);
            if (!s || (arc_order > 5))
                return NULL;

            if (crx->state[d].used == 0)
            {
                crx->state[d].lli = ' ';
                crx->state[d].ssi = ' ';
            }
            crx->state[d].order = arc_order;
            crx->state[d].used = 1;
            crx->state[d].value = val;
            p->obs[d] = val;
        }
        else /* Continuation: delta value. */
        {
            s = crx_parse_int64(&val, s);
            if (!s)
                return NULL;

            cur_order = crx->state[d].used - 1;
            arc_order = crx->state[d].order;

            if (cur_order < arc_order)
            {
                crx->state[d].diff[cur_order] = val;
                crx->state[d].used = cur_order + 2;
            }
            else
            {
                crx->state[d].diff[arc_order - 1] = val;
            }

            switch (crx->state[d].used)
            {
            case 6: crx->state[d].diff[3] += crx->state[d].diff[4]; /* fall through */
            case 5: crx->state[d].diff[2] += crx->state[d].diff[3]; /* fall through */
            case 4: crx->state[d].diff[1] += crx->state[d].diff[2]; /* fall through */
            case 3: crx->state[d].diff[0] += crx->state[d].diff[1]; /* fall through */
            case 2: crx->state[d].value += crx->state[d].diff[0]; /* fall through */
            case 1: p->obs[d] = crx->state[d].value;
            }
        }

        /* Advance past the field separator (space). */
        if (*s == ' ')
            s++;
    }

    /* Everything from s to next_line is the flag string, LLIs and SSIs. */
    for (ii = 0, d = base; ii < n_obs; ++ii, ++d)
    {
        if (*s == '\n' || *s == '\0')
        {
            if (is_init)
                crx->state[d].lli = ' ';
        }
        else
        {
            if (*s == '&')
                crx->state[d].lli = ' ';
            else if (is_init || *s != ' ')
                crx->state[d].lli = *s;
            s++;
        }

        if (*s == '\n' || *s == '\0')
        {
            if (is_init)
                crx->state[d].ssi = ' ';
        }
        else
        {
            if (*s == '&')
                crx->state[d].ssi = ' ';
            else if (is_init || *s != ' ')
                crx->state[d].ssi = *s;
            s++;
        }

        p->lli[d] = (crx->state[d].used == 0) ? ' ' : crx->state[d].lli;
        p->ssi[d] = (crx->state[d].used == 0) ? ' ' : crx->state[d].ssi;
    }

    while (*s != '\n' && *s != '\0')
        s++;
    if (*s == '\n')
        s++;
    return s;
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
 * \returns Zero on success, non-zero on invalid satellite ID.
 */
static int crx_v2_build_sattbl(
    struct crx_v23_parser *crx,
    const char *prev_svs,
    int n_prev,
    int *sattbl,
    int n_new)
{
    const char *pos = crx->epoch_text + 32;
    int ii, jj;
    char sys;
    unsigned char svn;

    for (ii = 0; ii < n_new; ++ii, pos += 3)
    {
        if (rnx_parse_satid(&sys, &svn, pos))
            return 1;
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
    return 0;
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
        if (res < RINEX_EOF)
            p_->error_line = __LINE__;
        return res;
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
        /* Swap state and prev_state so we can read old data from
         * prev_state while writing the shuffled result into state.
         */
        struct obs_state *tmp = crx->prev_state;
        crx->prev_state = crx->state;
        crx->state = tmp;

        for (ii = 0; ii < n_sats; ++ii)
        {
            int new_base = ii * n_obs;
            if (sattbl[ii] >= 0)
            {
                int old_base = sattbl[ii] * n_obs;
                memcpy(crx->state + new_base, crx->prev_state + old_base, n_obs * sizeof(struct obs_state));
            }
            else
            {
                memset(crx->state + new_base, 0, n_obs * sizeof(struct obs_state));
            }
        }
    }
    else if (is_init)
    {
        /* Reset all diff state. */
        memset(crx->state, 0, p->obs_alloc * sizeof(struct obs_state));
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
        if (rnx_parse_satid(&p_->sats[ii].system, &p_->sats[ii].number, pos))
        {
            p_->error_line = __LINE__;
            p->parse_ofs = res;
            return RINEX_ERR_BAD_FORMAT;
        }
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

/** crx_v2_parse_clock parses the CRX v2 receiver clock offset line.
 *
 * CRX v2 uses the same N&VALUE / delta format as individual observations:
 * an empty line means no clock offset (zero), N&VALUE is an arc init at
 * order N, and a bare integer is a differential update.
 */
static void crx_v2_parse_clock(struct crx_v23_parser *crx, const char *s)
{
    struct rinex_parser *p_ = &crx->base.base;
    struct obs_state *clk = &crx->clk_state;
    int64_t val;
    int cur_order;

    if (*s == '\n' || *s == '\0' || *s == ' ')
    {
        clk->used = 0;
        p_->epoch.clock_offset = 0;
    }
    else if (s[1] == '&')
    {
        clk->order = s[0] - '0';
        crx_parse_int64(&val, s + 2);
        clk->value = val;
        clk->used = 1;
        p_->epoch.clock_offset = val * 1000;
    }
    else
    {
        crx_parse_int64(&val, s);
        cur_order = clk->used - 1;
        if (cur_order < clk->order)
        {
            clk->diff[cur_order] = (int32_t)val;
            clk->used = cur_order + 2;
        }
        else
        {
            clk->diff[clk->order - 1] = (int32_t)val;
        }
        switch (clk->used)
        {
        case 6: clk->diff[3] += clk->diff[4]; /* fall through */
        case 5: clk->diff[2] += clk->diff[3]; /* fall through */
        case 4: clk->diff[1] += clk->diff[2]; /* fall through */
        case 3: clk->diff[0] += clk->diff[1]; /* fall through */
        case 2: clk->value += clk->diff[0];   /* fall through */
        case 1: p_->epoch.clock_offset = clk->value * 1000;
        }
    }
}

/** crx_read_v2 reads an observation data record from \a p_. */
static rinex_error_t crx_read_v2(struct rinex_parser *p_)
{
    struct crx_v23_parser *crx = (struct crx_v23_parser *)p_;
    struct rnx_v234_parser *p = &crx->base;
    const char *line;
    int res, err, line_len, ii;

    /* CRXv2 epoch headers have the full satellite list on a single line
     * (no 12-per-line continuation lines), followed by one clock line.
     * Read the first line; the clock is consumed separately below.
     */
    res = rnx_get_newlines(p_, &p->parse_ofs, NULL, 0, 1);
    if (res <= RINEX_EOF)
        return res;
    line = p_->stream->buffer + p->parse_ofs;
    line_len = strchr(line, '\n') - line;

    /* What is the line format? */
    if (line[0] == ' ') /* delta header line */
    {
        int old_n_sats = p_->epoch.n_sats;

        crx_line_space(crx, line_len);

        /* Apply the update(s) to the epoch header in epoch_text. */
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

        /* Ensure sattab is large enough. */
        if (crx->sattab_alloc < p_->epoch.n_sats)
        {
            int new_alloc = crx->sattab_alloc ? crx->sattab_alloc : 64;
            while (new_alloc < p_->epoch.n_sats)
                new_alloc *= 2;
            crx->sattab = realloc(crx->sattab, new_alloc * sizeof(int));
            if (!crx->sattab)
            {
                p_->error_line = __LINE__;
                return RINEX_ERR_SYSTEM;
            }
            crx->sattab_alloc = new_alloc;
        }

        /* Build satellite reorder table for delta epochs. */
        if (crx_v2_build_sattbl(crx, p_->buffer, old_n_sats,
            crx->sattab, p_->epoch.n_sats))
        {
            p_->error_line = __LINE__;
            return RINEX_ERR_BAD_FORMAT;
        }

        /* Advance past the epoch header line, parse the clock line. */
        p->parse_ofs = res;
        res = rnx_get_newlines(p_, &p->parse_ofs, NULL, 0, 1);
        if (res <= RINEX_EOF)
        {
            p_->error_line = __LINE__;
            return RINEX_ERR_BAD_FORMAT;
        }
        crx_v2_parse_clock(crx, p_->stream->buffer + p->parse_ofs);
        p->parse_ofs = res;

        /* Read and decompress the observations. */
        return crx_v2_read_obs(crx, 0, crx->sattab);
    }
    else if (line[0] != '&' || line_len < 32) /* must be a full init header */
    {
        p_->error_line = __LINE__;
        return RINEX_ERR_BAD_FORMAT;
    }
    else if (line[28] == '0' || line[28] == '1' || line[28] == '6')
    {
        /* Observation epoch: copy the header line to epoch_text. */
        crx_line_space(crx, line_len + 1);
        crx->epoch_text[0] = ' ';
        memcpy(crx->epoch_text + 1, line + 1, line_len - 1);
        crx->epoch_text[line_len] = '\0';

        /* Parse the timestamp, epoch flag, etc. */
        err = rnx_v2_parse_time(p, crx->epoch_text);
        if (err < 0)
        {
            return err;
        }

        /* Advance past the epoch header line, parse the clock line. */
        p->parse_ofs = res;
        res = rnx_get_newlines(p_, &p->parse_ofs, NULL, 0, 1);
        if (res <= RINEX_EOF)
        {
            p_->error_line = __LINE__;
            return RINEX_ERR_BAD_FORMAT;
        }
        crx_v2_parse_clock(crx, p_->stream->buffer + p->parse_ofs);
        p->parse_ofs = res;

        /* Read and decompress observations (full initialization). */
        return crx_v2_read_obs(crx, 1, NULL);
    }
    else /* epoch flag 2-5: special event record */
    {
        int n_lines, eol;
        char flag = line[28];

        /* How many lines in this special event record? */
        if (parse_uint(&n_lines, line + 29, 3))
        {
            p_->error_line = __LINE__;
            return RINEX_ERR_BAD_FORMAT;
        }
        /* Skip the marker line + n_lines following header records. */
        eol = rnx_get_newlines(p_, &p->parse_ofs, NULL, 0, n_lines + 1);
        if (eol <= RINEX_EOF)
        {
            return RINEX_ERR_BAD_FORMAT;
        }
        err = rnx_copy_text(p, eol);
        if (err == RINEX_SUCCESS)
        {
            /* Replace leading '&' with a space to be RINEX. */
            p->base.buffer[0] = ' ';
            p->parse_ofs = eol;
            p_->epoch.flag = flag;
        }
        return err;
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
    int res, line_len, ii, nn, n_sats, n_obs_sys, is_init, old_n_sats;
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
    old_n_sats = p_->epoch.n_sats;

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

    /* Count total observation slots needed (and validate satellite IDs). */
    nn = 0;
    for (ii = 0; ii < n_sats; ++ii)
    {
        char sys;
        unsigned char num;
        if (rnx_parse_satid(&sys, &num, crx->epoch_text + 41 + 3 * ii))
        {
            p_->error_line = __LINE__;
            p->parse_ofs = res;
            return RINEX_ERR_BAD_FORMAT;
        }
        nn += p_->n_obs[sys & 31];
    }

    /* Ensure observation arrays are large enough. */
    err = crx_ensure_obs(crx, nn);
    if (err != RINEX_SUCCESS)
    {
        p->parse_ofs = res;
        return err;
    }

    /* On init epochs, reset diff state for all slots.
     * On delta epochs, shuffle diff state to match the new satellite
     * order.  p_->sats[] still holds the previous epoch's satellites.
     */
    if (is_init)
    {
        memset(crx->state, 0, p->obs_alloc * sizeof(struct obs_state));
    }
    else
    {
        int new_nn, jj, old_nn, old_n_obs;
        struct obs_state *tmp;

        /* Swap state and prev_state so we can read old data from
         * prev_state while writing the shuffled result into state.
         */
        tmp = crx->prev_state;
        crx->prev_state = crx->state;
        crx->state = tmp;

        new_nn = 0;
        for (ii = 0; ii < n_sats; ++ii)
        {
            char sys = crx->epoch_text[41 + 3 * ii];
            unsigned char svn = (crx->epoch_text[42 + 3 * ii] - '0') * 10
                              + (crx->epoch_text[43 + 3 * ii] - '0');
            n_obs_sys = p_->n_obs[sys & 31];

            /* Find this satellite in the old list. */
            old_nn = 0;
            for (jj = 0; jj < old_n_sats; ++jj)
            {
                old_n_obs = p_->n_obs[p_->sats[jj].system & 31];
                if (p_->sats[jj].system == sys
                    && p_->sats[jj].number == svn)
                {
                    memcpy(crx->state + new_nn,
                           crx->prev_state + old_nn,
                           n_obs_sys * sizeof(struct obs_state));
                    break;
                }
                old_nn += old_n_obs;
            }
            if (jj == old_n_sats)
            {
                memset(crx->state + new_nn, 0,
                       n_obs_sys * sizeof(struct obs_state));
            }
            new_nn += n_obs_sys;
        }
    }

    /* Decompress each satellite's observation line. */
    nn = 0;
    for (ii = 0; ii < n_sats; ++ii)
    {
        char sys = crx->epoch_text[41 + 3 * ii];
        unsigned char svn = (crx->epoch_text[42 + 3 * ii] - '0') * 10
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

    free(p->sattab);
    free(p->prev_state);
    free(p->state);
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
        crx->state = calloc(crx->base.obs_alloc, sizeof(struct obs_state));
        crx->prev_state = calloc(crx->base.obs_alloc, sizeof(struct obs_state));
        if (!crx->epoch_text || !crx->state || !crx->prev_state)
        {
            err = "Memory allocation failed";
        }
    }

    return err;
}
