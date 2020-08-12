/** rinex_stdio.c - RINEX stream for the C standard I/O library.
 * Copyright 2020 Michael Poole.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "rinex.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct rinex_stream_stdio
{
    struct rinex_stream base;

    unsigned int alloc;

    FILE *f;
};

static int rinex_stdio_advance(
    struct rinex_stream *stream_base,
    unsigned int req_size,
    unsigned int step
)
{
    struct rinex_stream_stdio *stream = (struct rinex_stream_stdio *)stream_base;

    if (req_size > INT_MAX)
    {
        return EINVAL;
    }

    if (step > stream->base.size)
    {
        return EINVAL;
    }
    stream->base.size -= step;

    if (stream->alloc < req_size + RINEX_EXTRA)
    {
        unsigned int alloc_size;
        char *new_buf;

        alloc_size = req_size + RINEX_EXTRA;
        new_buf = realloc(stream->base.buffer, alloc_size);
        if (!new_buf)
        {
            return ENOMEM;
        }
        stream->base.buffer = new_buf;
        stream->alloc = alloc_size;
    }

    memmove(stream->base.buffer, stream->base.buffer + step, stream->base.size);

    if (req_size > stream->base.size)
    {
        size_t wanted, nbr;
        
        wanted = req_size - stream->base.size;
        nbr = fread(stream->base.buffer + stream->base.size, 1, wanted, stream->f);
        stream->base.size += nbr;

        if (nbr < wanted)
        {
            if (ferror(stream->f))
            {
                return errno;
            }
            /* XXX: Should we flag EOF? */
        }
    }

    return 0;
}

static void rinex_stdio_destroy(
    struct rinex_stream *stream_base
)
{
    struct rinex_stream_stdio *stream = (struct rinex_stream_stdio *)stream_base;

    if (stream->f != stdin)
    {
        fclose(stream->f);
    }

    free(stream);
}

struct rinex_stream *rinex_stdio_stream(const char *filename)
{
    struct rinex_stream_stdio *str;

    str = calloc(1, sizeof *str);
    if (str == NULL)
    {
        return NULL;
    }

    str->base.advance = rinex_stdio_advance;
    str->base.destroy = rinex_stdio_destroy;
    str->base.buffer = NULL;

    str->f = fopen(filename, "r");
    if (!str->f)
    {
        free(str);
        return NULL;
    }

    return &str->base;
}

struct rinex_stream *rinex_stdin_stream(void)
{
    struct rinex_stream_stdio *str;

    str = calloc(1, sizeof *str);
    if (str == NULL)
    {
        return NULL;
    }

    str->base.advance = rinex_stdio_advance;
    str->base.destroy = rinex_stdio_destroy;
    str->base.buffer = NULL;
    str->f = stdin;

    return &str->base;
}
