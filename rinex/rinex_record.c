/** rinex_record.c - Parse individual RINEX observation records.
 * Copyright 2026 Michael Poole.
 * SPDX-License-Identifier: MIT-Modern-Variant
 */

#include "rinex/rnx_priv.h"

#include <errno.h>
#include <string.h>

/* Some records are kept as RINEX text apart from their file: special
 * events, and flag-1 and flag-6 observation records in rinex_load()'s
 * rinex_data and in SRNX EVTF chunks.  A record parser is an ordinary
 * RINEX text parser, opened once from the file's header, that reads
 * from a stream holding just the record being parsed.
 */

/** rnx_record_stream is an in-memory stream that owns its buffer, so
 * it can be refilled with each record.
 */
struct rnx_record_stream
{
    struct rinex_stream base;

    /** data is the buffer, with RINEX_EXTRA bytes of padding. */
    char *data;

    /** alloc is the allocated length of #data. */
    unsigned int alloc;
};

static int rnx_record_advance(
    struct rinex_stream *stream_base,
    unsigned int req_size,
    unsigned int step
)
{
    if (step > stream_base->size)
    {
        return EINVAL;
    }
    stream_base->buffer += step;
    stream_base->size -= step;
    (void)req_size;
    return 0;
}

static void rnx_record_destroy(struct rinex_stream *stream_base)
{
    free(((struct rnx_record_stream *)stream_base)->data);
    free(stream_base);
}

/** Replace the contents of \a stream_base, a rnx_record_stream, with
 * \a len bytes of \a text.  Returns zero on success, ENOMEM on failure.
 */
static int rnx_record_set(
    struct rinex_stream *stream_base,
    const char *text,
    unsigned int len
)
{
    struct rnx_record_stream *stream = (struct rnx_record_stream *)stream_base;

    if (stream->alloc < len + RINEX_EXTRA)
    {
        char *new_data = realloc(stream->data, len + RINEX_EXTRA);

        if (!new_data)
        {
            return ENOMEM;
        }
        stream->data = new_data;
        stream->alloc = len + RINEX_EXTRA;
    }

    memcpy(stream->data, text, len);
    memset(stream->data + len, 0, RINEX_EXTRA);
    stream->base.buffer = stream->data;
    stream->base.size = len;
    return 0;
}

/* Doc comment is in rinex.h. */
const char *rinex_open_records
(
    struct rinex_parser **p_parser,
    const char *header,
    int header_len
)
{
    struct rnx_record_stream *stream;
    struct rnx_v234_parser *p;
    const char *err;
    int ofs;

    if (*p_parser)
    {
        (*p_parser)->destroy(*p_parser);
        *p_parser = NULL;
    }

    /* Skip any CRINEX lines: the records are plain RINEX. */
    ofs = header_len > 0
        ? rnx_find_header(header, header_len, rnx_version_type, 21) : -1;
    if (ofs < 0)
    {
        return "Could not find RINEX VERSION / TYPE";
    }

    stream = calloc(1, sizeof *stream);
    if (!stream)
    {
        return "Memory allocation failed";
    }
    stream->base.advance = rnx_record_advance;
    stream->base.destroy = rnx_record_destroy;
    if (rnx_record_set(&stream->base, header + ofs, header_len - ofs))
    {
        rnx_record_destroy(&stream->base);
        return "Memory allocation failed";
    }

    p = calloc(1, sizeof *p);
    if (!p)
    {
        rnx_record_destroy(&stream->base);
        return "Memory allocation failed";
    }
    p->own_stream = &stream->base;
    p->base.destroy = rnx_free_v23;
    *p_parser = &p->base;

    err = rnx_open_v23(p, &stream->base, 0);
    if (err)
    {
        /* If the error string lives inside the parser's errmsg buffer,
         * save a copy before the parser is deallocated. */
        if (err == p->base.errmsg)
        {
            static char saved_err[160];
            strncpy(saved_err, err, sizeof saved_err);
            saved_err[sizeof saved_err - 1] = '\0';
            err = saved_err;
        }
        p->base.destroy(&p->base);
        *p_parser = NULL;
    }

    return err;
}

/** Return how much of \a text (\a len bytes, not necessarily
 * NUL-terminated) to quote in an error message: up to its first line
 * break, and at most 40 bytes.
 */
static int rnx_record_quote_len(const char *text, unsigned int len)
{
    const char *eol;

    if (len > 40)
    {
        len = 40;
    }
    eol = memchr(text, '\n', len);
    return eol ? (int)(eol - text) : (int)len;
}

/* Doc comment is in rinex.h. */
rinex_error_t rinex_parse_record
(
    struct rinex_parser *p_,
    const char *text,
    int text_len
)
{
    struct rnx_v234_parser *p = (struct rnx_v234_parser *)p_;
    struct rinex_stream *stream;
    rinex_error_t res;

    stream = p_->destroy == rnx_free_v23 ? p->own_stream : NULL;
    if (!stream || stream->advance != rnx_record_advance)
    {
        rnx_errorf(p_, "parser was not opened by rinex_open_records()");
        return RINEX_ERR_BAD_FORMAT;
    }
    if (text_len <= 0)
    {
        rnx_errorf(p_, "empty record");
        return RINEX_ERR_BAD_FORMAT;
    }
    if (rnx_record_set(stream, text, (unsigned int)text_len))
    {
        rnx_errorf(p_, "allocation failed for record of length %d", text_len);
        errno = ENOMEM;
        return RINEX_ERR_SYSTEM;
    }

    p->parse_ofs = 0;
    res = p_->read(p_);
    if (res == RINEX_EOF)
    {
        rnx_errorf(p_, "incomplete record: %.*s",
            rnx_record_quote_len(text, (unsigned int)text_len), text);
        return RINEX_ERR_BAD_FORMAT;
    }
    if (res == RINEX_SUCCESS && p->parse_ofs != stream->size)
    {
        rnx_errorf(p_, "extra text after record: %.*s",
            rnx_record_quote_len(stream->buffer + p->parse_ofs,
                stream->size - (unsigned int)p->parse_ofs),
            stream->buffer + p->parse_ofs);
        return RINEX_ERR_BAD_FORMAT;
    }
    return res;
}
