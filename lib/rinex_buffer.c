/** rinex_buffer.c - RINEX stream for user-provided buffers.
 * Copyright 2022 Michael Poole.
 * SPDX-License-Identifier: MIT-Modern-Variant
 */

#include "lib/rnx_priv.h"

static int rinex_buffer_advance(
    struct rinex_stream *stream_base,
    unsigned int req_size,
    unsigned int step
)
{
    if (step > stream_base->size)
    {
        step = stream_base->size;
    }
    stream_base->buffer += step;
    stream_base->size -= step;
    (void)req_size;
    return 0;
}

static void rinex_buffer_destroy(
    struct rinex_stream *stream_base
)
{
    free(stream_base);
}

struct rinex_stream *rinex_buffer_stream(const void *buffer, int nbytes)
{
    struct rinex_stream *str;

    str = calloc(1, sizeof *str);
    if (str == NULL)
    {
        return NULL;
    }

    /* TODO: ensure RINEX_EXTRA bytes of zeros at the end of the buffer */

    str->advance = rinex_buffer_advance;
    str->destroy = rinex_buffer_destroy;
    str->buffer = (void *)buffer;
    str->size = nbytes;

    return str;
}
