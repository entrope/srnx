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
    const char sv[2])
{
    int jj, kk;

    /* We found a change.  If the satelite already has history, it will
     * be later in the array, so we swap the two.
     */
    for (jj = idx + 1; jj < crx->base.base.epoch.n_sats; ++jj)
    {
        if (crx->base.base.buffer[2 * jj + 0] == sv[0] && crx->base.base.buffer[2 * jj + 1] == sv[1])
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
        for (kk = 0; kk < n_obs; ++kk)
            ;
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
        sv[0] = pos[0];
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
        while (crx->epoch_text[ii - 1] == ' ')
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
void crx_free_v23(struct rinex_parser *p_)
{
    struct crx_v23_parser *p = (struct crx_v23_parser *)p_;

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
    ofs = rnx_find_header(stream->buffer, stream->size, rnx_version_type, 20);
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
