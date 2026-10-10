/** rinex_crlf.c - RINEX stream that converts CR LF line endings to LF.
 * Copyright 2026 Michael Poole.
 * SPDX-License-Identifier: MIT-Modern-Variant
 */

#include "rinex/rnx_priv.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

/* The RINEX and CRINEX parsers expect lines to end with a bare LF.
 * Files written on Windows end lines with CR LF, so rinex_open() wraps
 * their streams in this filter, which copies data from the underlying
 * stream into its own buffer, dropping each CR that precedes an LF.
 */

struct rinex_stream_crlf
{
    struct rinex_stream base;

    /** inner is the underlying stream.  It is owned by the caller of
     * rinex_open(), so destroying this stream does not touch it.
     */
    struct rinex_stream *inner;

    /** in_ofs is how many bytes of \a inner->buffer have been consumed. */
    unsigned int in_ofs;

    /** alloc is the allocated length of #base.buffer. */
    unsigned int alloc;

    /** eof is set once \a inner has no more data. */
    int eof;
};

/** Copy bytes from the inner stream's buffer into \a stream's buffer,
 * dropping CRs that precede LFs, until \a stream holds \a req_size bytes
 * or the inner buffer is used up.  A CR at the end of the inner buffer
 * is held back (unless at end of file) until we know what follows it.
 */
static void rnx_crlf_copy(struct rinex_stream_crlf *stream, unsigned int req_size)
{
    const char *in = stream->inner->buffer;
    unsigned int in_end = stream->inner->size;
    unsigned int ii = stream->in_ofs;
    unsigned int out = stream->base.size;
    char *dest = stream->base.buffer;

    while (out < req_size && ii < in_end)
    {
        if (in[ii] == '\r')
        {
            if (ii + 1 == in_end && !stream->eof)
                break;
            if (ii + 1 < in_end && in[ii + 1] == '\n')
            {
                ++ii;
                continue;
            }
        }
        dest[out++] = in[ii++];
    }

    stream->in_ofs = ii;
    stream->base.size = out;
}

static int rnx_crlf_advance(
    struct rinex_stream *stream_base,
    unsigned int req_size,
    unsigned int step
)
{
    struct rinex_stream_crlf *stream = (struct rinex_stream_crlf *)stream_base;
    unsigned int remaining;
    int res;

    if (req_size > INT_MAX || step > stream->base.size)
    {
        return EINVAL;
    }

    if (stream->alloc < req_size + RINEX_EXTRA)
    {
        char *new_buf;

        new_buf = realloc(stream->base.buffer, req_size + RINEX_EXTRA);
        if (!new_buf)
        {
            return ENOMEM;
        }
        stream->base.buffer = new_buf;
        stream->alloc = req_size + RINEX_EXTRA;
    }

    stream->base.size -= step;
    memmove(stream->base.buffer, stream->base.buffer + step, stream->base.size);

    for (;;)
    {
        rnx_crlf_copy(stream, req_size);
        if (stream->base.size >= req_size || stream->eof)
        {
            break;
        }

        /* Discard what we have consumed and ask for more data. */
        remaining = stream->inner->size - stream->in_ofs;
        res = stream->inner->advance(stream->inner, BLOCK_SIZE, stream->in_ofs);
        if (res)
        {
            return res;
        }
        stream->in_ofs = 0;
        if (stream->inner->size <= remaining)
        {
            stream->eof = 1;
        }
    }

    memset(stream->base.buffer + stream->base.size, 0, RINEX_EXTRA);
    return 0;
}

static void rnx_crlf_destroy(struct rinex_stream *stream_base)
{
    free(stream_base->buffer);
    free(stream_base);
}

/* Doc comment is in rnx_priv.h. */
int rnx_has_crlf(const struct rinex_stream *stream)
{
    const char *eol;

    eol = memchr(stream->buffer, '\n', stream->size);
    return eol && eol > stream->buffer && eol[-1] == '\r';
}

/* Doc comment is in rnx_priv.h. */
struct rinex_stream *rnx_crlf_stream(struct rinex_stream *inner)
{
    struct rinex_stream_crlf *stream;

    stream = calloc(1, sizeof *stream);
    if (!stream)
    {
        return NULL;
    }

    stream->base.advance = rnx_crlf_advance;
    stream->base.destroy = rnx_crlf_destroy;
    stream->inner = inner;

    return &stream->base;
}
