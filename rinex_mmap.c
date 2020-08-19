/** rinex_mmap.c - RINEX stream for Unix-style memory-mapped files.
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
#include <fcntl.h>
#include <limits.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>

struct rinex_stream_mmap
{
    struct rinex_stream base;

    /** map is the current mmap view into #fd. */
    char *map;

    /** offset is the offset of #map from the start of the file. */
    off_t offset;

    /** file_size is the total size of the file. */
    off_t file_size;

    /** total is the size of #map. */
    size_t total;

    /** fd is the file descriptor of the file we are reading. */
    int fd;
};

/** dev_zero is a file descriptor for /dev/zero, used to mmap empty
 * pages past the end of real data (when needed).
 */
static int dev_zero = -1;

/** page_size is the value of sysconf(_SC_PAGE_SIZE). */
static long page_size = 0;

static int rinex_mmap_advance(
    struct rinex_stream *stream_base,
    unsigned int req_size,
    unsigned int step
)
{
    struct rinex_stream_mmap *stream = (struct rinex_stream_mmap *)stream_base;
    off_t new_offset, eff_len, base_offset;

    /* Did someone accidentally pass us a negative number? */
    if (req_size > INT_MAX || step > INT_MAX)
    {
        return EINVAL;
    }

    /* Check that we don't step past the end of the file. */
    if (step > stream->file_size - stream->offset)
    {
        return EINVAL;
    }
    new_offset = stream->offset + (stream->base.buffer - stream->map) + step;

    /* If we already have the entire file mapped, we can only advance
     * within the current buffer.
     */
    if (stream->offset + stream->total >= (size_t)stream->file_size + RINEX_EXTRA)
    {
        eff_len = stream->map + stream->total - stream->base.buffer;
        if (step > eff_len)
        {
            step = eff_len;
        }
        stream->base.buffer += step;
        stream->base.size -= step;
        return 0;
    }

    /* We can reduce the requested size to the actual end of the file. */
    if (req_size > stream->file_size - new_offset)
    {
        req_size = stream->file_size - new_offset;
    }

    /* Clean up any previous mapping we have. */
    if (stream->map)
    {
        munmap(stream->map, stream->total);
        stream->base.buffer = NULL;
        stream->base.size = 0;
        stream->map = NULL;
    }

    /* If we want to map to the end of the file, and that is within
     * RINEX_EXTRA bytes of the end of a page, we need to mmap an extra
     * page of zeros.  Unfortunately, the best way to do this is to mmap
     * /dev/zero for our entire desired size and then use MAP_FIXED to
     * replace the start.
     *
     * If we aren't mapping to the end of the file, or the end of the
     * file is at least RINEX_EXTRA bytes before the end of a page, we
     * can just map the target range directly.
     */
    eff_len = (stream->file_size + page_size - 1) & -page_size;
    base_offset = new_offset & -page_size;
    stream->total = (new_offset - base_offset + req_size + RINEX_EXTRA + page_size - 1) & -page_size;
    if (base_offset + stream->total <= (size_t)eff_len)
    {
        /* We can do a simple mmap. */
        stream->map = mmap(NULL, stream->total, PROT_READ, MAP_SHARED,
            stream->fd, base_offset);
        if (stream->map == MAP_FAILED)
        {
            return errno;
        }

success:
        stream->offset = base_offset;
        stream->base.buffer = stream->map + new_offset - base_offset;
        stream->base.size = stream->total - RINEX_EXTRA + base_offset - new_offset;
        return 0;
    }

    stream->map = mmap(NULL, stream->total, PROT_READ, MAP_SHARED, dev_zero, 0);
    if (stream->map == MAP_FAILED)
    {
        return errno;
    }
    if (MAP_FAILED == mmap(stream->map, eff_len - base_offset, PROT_READ,
        MAP_SHARED | MAP_FIXED, stream->fd, base_offset))
    {
        return errno;
    }

    goto success;
}

static void rinex_mmap_destroy(
    struct rinex_stream *stream_base
)
{
    struct rinex_stream_mmap *stream = (struct rinex_stream_mmap *)stream_base;

    if (stream->map)
    {
        munmap(stream->map, stream->total);
    }

    close(stream->fd);

    free(stream);
}

struct rinex_stream *rinex_mmap_stream(const char *filename)
{
    struct rinex_stream_mmap *str;
    struct stat sbuf;
    int res;

    str = calloc(1, sizeof *str);
    if (str == NULL)
    {
        return NULL;
    }

    if (dev_zero < 0)
    {
        dev_zero = open("/dev/zero", O_RDONLY);
        if (dev_zero < 0)
        {
            return NULL;
        }

        page_size = sysconf(_SC_PAGE_SIZE);
        if (page_size <= 0)
        {
            return NULL;
        }
    }

    str->fd = open(filename, O_RDONLY);
    if (str->fd < 0)
    {
        free(str);
        return NULL;
    }

    res = fstat(str->fd, &sbuf);
    if (res < 0)
    {
        free(str);
        return NULL;
    }

    str->base.advance = rinex_mmap_advance;
    str->base.destroy = rinex_mmap_destroy;
    str->base.buffer = NULL;
    str->map = NULL;
    str->file_size = sbuf.st_size;

    return &str->base;
}
