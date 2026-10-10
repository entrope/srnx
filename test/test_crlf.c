/** test/test_crlf.c - TAP tests for reading files with CR LF line endings.
 * Copyright 2026 Michael Poole.
 * SPDX-License-Identifier: MIT-Modern-Variant
 *
 *  - The CR LF filter stream (rnx_crlf_stream) must turn each CR LF into
 *    LF and pass everything else through, however the underlying stream
 *    splits its data and however much the reader asks for at a time.
 *  - RINEX 2, RINEX 3, CRINEX 1 and CRINEX 3 files with CR LF line
 *    endings must parse exactly like their LF originals.
 */

#include "rinex/rnx_priv.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tap.h>

#include "bin/abmf00glp_crx.h"
#include "bin/abmf00glp_rnx.h"
#include "bin/at012000_20d.h"
#include "bin/at012000_20o.h"

/* ---- A stream that delivers its data a few bytes at a time ---- */

struct trickle_stream
{
    struct rinex_stream base;
    const char *data;
    unsigned int len, start, end, chunk;
};

static int trickle_advance(
    struct rinex_stream *stream_base,
    unsigned int req_size,
    unsigned int step
)
{
    struct trickle_stream *stream = (struct trickle_stream *)stream_base;

    (void)req_size;
    if (step > stream->base.size)
        return EINVAL;
    stream->start += step;
    stream->end += stream->chunk;
    if (stream->end > stream->len)
        stream->end = stream->len;
    stream->base.buffer = (char *)stream->data + stream->start;
    stream->base.size = stream->end - stream->start;
    return 0;
}

static void trickle_destroy(struct rinex_stream *stream_base)
{
    free(stream_base);
}

/** Make a trickle stream over \a data, which must be readable for
 * RINEX_EXTRA bytes past \a len.
 */
static struct rinex_stream *trickle_stream(
    const char *data,
    unsigned int len,
    unsigned int chunk
)
{
    struct trickle_stream *stream = calloc(1, sizeof *stream);

    if (!stream)
        BAIL_OUT("out of memory");
    stream->base.advance = trickle_advance;
    stream->base.destroy = trickle_destroy;
    stream->base.buffer = (char *)data;
    stream->data = data;
    stream->len = len;
    stream->chunk = chunk;
    return &stream->base;
}

/* ---- Helpers ---- */

/** Return a copy of \a text with RINEX_EXTRA zero bytes of padding. */
static char *padded_copy(const char *text, unsigned int len)
{
    char *res = calloc(1, len + RINEX_EXTRA);

    if (!res)
        BAIL_OUT("out of memory");
    memcpy(res, text, len);
    return res;
}

/** Return \a text with each LF replaced by CR LF, padded. */
static char *to_crlf(const unsigned char *text, unsigned int len,
    unsigned int *p_len)
{
    char *res = calloc(1, 2 * len + RINEX_EXTRA);
    unsigned int ii, jj;

    if (!res)
        BAIL_OUT("out of memory");
    for (ii = jj = 0; ii < len; ++ii)
    {
        if (text[ii] == '\n')
            res[jj++] = '\r';
        res[jj++] = text[ii];
    }
    *p_len = jj;
    return res;
}

/** Read everything from \a stream, \a req bytes at a time, into a
 * malloc'd buffer.  Returns NULL if the stream fails.
 */
static char *drain(struct rinex_stream *stream, unsigned int req,
    unsigned int *p_len)
{
    char *res = NULL;
    unsigned int len = 0;

    for (;;)
    {
        if (stream->advance(stream, req, stream->size))
        {
            free(res);
            return NULL;
        }
        if (stream->size == 0)
            break;
        res = realloc(res, len + stream->size + 1);
        memcpy(res + len, stream->buffer, stream->size);
        len += stream->size;
    }
    *p_len = len;
    return res ? res : calloc(1, 1);
}

/* ---- Tests ---- */

/** Check the filter on \a input against the expected output, with
 * several chunk and request sizes.
 */
static void test_filter(const char *name, const char *input,
    const char *expected)
{
    static const unsigned int chunks[] = { 1, 2, 3, 7, 4096 };
    static const unsigned int reqs[] = { 1, 5, 80, 4096 };
    unsigned int in_len = strlen(input), exp_len = strlen(expected);
    unsigned int ii, jj, out_len, n_bad = 0;
    char *data = padded_copy(input, in_len);

    for (ii = 0; ii < sizeof chunks / sizeof chunks[0]; ++ii)
    {
        for (jj = 0; jj < sizeof reqs / sizeof reqs[0]; ++jj)
        {
            struct rinex_stream *inner, *crlf;
            char *out;

            inner = trickle_stream(data, in_len, chunks[ii]);
            crlf = rnx_crlf_stream(inner);
            out = drain(crlf, reqs[jj], &out_len);
            if (!out || out_len != exp_len || memcmp(out, expected, exp_len))
            {
                diag("%s: chunk %u, req %u: got %u bytes", name,
                    chunks[ii], reqs[jj], out ? out_len : 0);
                ++n_bad;
            }
            free(out);
            crlf->destroy(crlf);
            inner->destroy(inner);
        }
    }
    ok(n_bad == 0, "CR LF filter: %s", name);
    free(data);
}

/** Parse \a lf_text and a CR LF copy of it, and check that both parsers
 * produce the same header, records and final status.
 */
static void test_parse(const char *name, const unsigned char *lf_text,
    unsigned int lf_len)
{
    struct rinex_stream *s_lf, *s_crlf;
    struct rinex_parser *p_lf = NULL, *p_crlf = NULL;
    rinex_error_t r_lf, r_crlf;
    char *lf_buf, *crlf_buf;
    unsigned int crlf_len;
    const char *err_lf, *err_crlf;
    int n_records = 0, n_bad = 0, ii;

    lf_buf = padded_copy((const char *)lf_text, lf_len);
    crlf_buf = to_crlf(lf_text, lf_len, &crlf_len);
    s_lf = rinex_buffer_stream(lf_buf, lf_len);
    s_crlf = rinex_buffer_stream(crlf_buf, crlf_len);
    err_lf = rinex_open(&p_lf, s_lf);
    err_crlf = rinex_open(&p_crlf, s_crlf);
    ok(!err_lf && !err_crlf, "%s: CR LF copy opens (%s)", name,
        err_crlf ? err_crlf : "ok");
    if (err_lf || err_crlf)
        goto out;
    ok(p_lf->buffer_len == p_crlf->buffer_len
        && !memcmp(p_lf->buffer, p_crlf->buffer, p_lf->buffer_len)
        && !memcmp(p_lf->n_obs, p_crlf->n_obs, sizeof p_lf->n_obs),
        "%s: headers match", name);

    for (;;)
    {
        const struct rinex_epoch *e1 = &p_lf->epoch, *e2 = &p_crlf->epoch;

        r_lf = p_lf->read(p_lf);
        r_crlf = p_crlf->read(p_crlf);
        if (r_lf != r_crlf)
        {
            diag("record %d: status %d vs %d", n_records, r_lf, r_crlf);
            ++n_bad;
            break;
        }
        if (r_lf != RINEX_SUCCESS)
            break;
        ++n_records;

        if (e1->yyyy_mm_dd != e2->yyyy_mm_dd || e1->hh_mm != e2->hh_mm
            || e1->sec_e7 != e2->sec_e7 || e1->flag != e2->flag
            || e1->n_sats != e2->n_sats
            || e1->clock_offset != e2->clock_offset)
        {
            diag("record %d: epochs differ", n_records);
            ++n_bad;
            continue;
        }
        if (e1->flag != '0' && e1->flag != '1' && e1->flag != '6')
        {
            if (p_lf->buffer_len != p_crlf->buffer_len
                || memcmp(p_lf->buffer, p_crlf->buffer, p_lf->buffer_len))
            {
                diag("record %d: event text differs", n_records);
                ++n_bad;
            }
            continue;
        }
        for (ii = 0; ii < e1->n_sats; ++ii)
        {
            const struct rinex_sv_info *sv1 = &p_lf->sats[ii];
            const struct rinex_sv_info *sv2 = &p_crlf->sats[ii];
            int base = sv1->obs_0, n_obs = p_lf->n_obs[sv1->system & 31];

            if (sv1->system != sv2->system || sv1->number != sv2->number
                || sv1->obs_0 != sv2->obs_0
                || memcmp(p_lf->obs + base, p_crlf->obs + base,
                    n_obs * sizeof *p_lf->obs)
                || memcmp(p_lf->lli + base, p_crlf->lli + base, n_obs)
                || memcmp(p_lf->ssi + base, p_crlf->ssi + base, n_obs))
            {
                diag("record %d: satellite %d differs", n_records, ii);
                ++n_bad;
                break;
            }
        }
    }
    ok(n_bad == 0 && n_records > 0, "%s: %d records match (final status %d)",
        name, n_records, r_lf);

out:
    if (p_lf)
        p_lf->destroy(p_lf);
    if (p_crlf)
        p_crlf->destroy(p_crlf);
    s_lf->destroy(s_lf);
    s_crlf->destroy(s_crlf);
    free(lf_buf);
    free(crlf_buf);
}

int main(void)
{
    struct rinex_stream *stream;
    char *buf;

    plan(19);

    test_filter("plain LF text", "ab\ncd\n", "ab\ncd\n");
    test_filter("CR LF text", "ab\r\ncd\r\n", "ab\ncd\n");
    test_filter("lone CRs are kept", "a\rb\r\r\nc\r", "a\rb\r\nc\r");
    test_filter("CR LF straddling chunks",
        "0123456\r\n89ab\r\r\n\r\nx\r\n", "0123456\n89ab\r\n\nx\n");

    /* rnx_has_crlf looks only at the first line. */
    buf = padded_copy("line\r\nmore\n", 11);
    stream = rinex_buffer_stream(buf, 11);
    ok(rnx_has_crlf(stream), "rnx_has_crlf: first line ends in CR LF");
    stream->destroy(stream);
    stream = rinex_buffer_stream(buf + 6, 5);
    ok(!rnx_has_crlf(stream), "rnx_has_crlf: first line ends in LF");
    stream->destroy(stream);
    free(buf);

    test_parse("RINEX 3", abmf_rnx, abmf_rnx_len);
    test_parse("CRINEX 3", abmf_crx, abmf_crx_len);
    test_parse("RINEX 2", at012000_20o, at012000_20o_len);
    test_parse("CRINEX 1", at012000_20d, at012000_20d_len);

    {
        /* A CR LF file in an unrecognized format must fail cleanly. */
        struct rinex_parser *p = NULL;
        const char *err;

        buf = padded_copy("not a RINEX file\r\n", 18);
        stream = rinex_buffer_stream(buf, 18);
        err = rinex_open(&p, stream);
        ok(err && !p, "unrecognized CR LF file is rejected");
        stream->destroy(stream);
        free(buf);
    }

    done_testing();
}
