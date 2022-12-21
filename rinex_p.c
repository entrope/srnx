/** rinex_p.c - Private utility functions for RINEX parsing.
 * Copyright 2020 Michael Poole.
 * SPDX-License-Identifier: MIT-Modern-Variant
 */

#include "rinex_p.h"
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

long page_size;

/** dev_zero is a file descriptor for /dev/zero, used to mmap empty
 * pages past the end of real data (when needed).
 */
static int dev_zero;

/* Documentation comment in rinex_p.h. */
int rnx_mmap_init(void)
{
    if (!page_size)
    {
        page_size = sysconf(_SC_PAGE_SIZE);
        if (page_size <= 0)
        {
            return 1;
        }

        dev_zero = open("/dev/zero", O_RDONLY);
        if (dev_zero < 0)
        {
            return 2;
        }
    }

    return 0;
}

/* Documentation comment in rinex_p.h. */
void *rnx_mmap_padded(int fd, off_t offset, size_t f_len, size_t tot_len)
{
    void *addr;

    if (!page_size && rnx_mmap_init())
    {
        return MAP_FAILED;
    }

    addr = mmap(NULL, tot_len, PROT_READ, MAP_SHARED, dev_zero, 0);
    if (addr != MAP_FAILED)
    {
        if (MAP_FAILED == mmap(addr, f_len, PROT_READ, MAP_SHARED | MAP_FIXED, fd, offset))
        {
            munmap(addr, tot_len);
            return MAP_FAILED;
        }
    }

    return addr;
}

/* Documentation comment in rinex_p.h. */
void *memmem
(
    const char *haystack, size_t h_size,
    const char *needle, size_t n_size
)
{
    const char *ptr;

    if (n_size > h_size)
    {
        assert(n_size <= h_size);
        return NULL;
    }

    for (ptr = haystack; ptr < haystack + h_size - n_size; ++ptr)
    {
        ptr = memchr(ptr, needle[0], haystack + h_size - ptr);
        if (!ptr)
        {
            return NULL;
        }

        if (!memcmp(ptr, needle, n_size))
        {
            return (void *)ptr;
        }
    }

    return NULL;
}

/* Documentation comment in rinex_p.h. */
int rnx_copy_text(
    struct rnx_v234_parser *p,
    int eol_ofs
)
{
    p->base.buffer_len = eol_ofs - p->parse_ofs;

    while (p->buffer_alloc < p->base.buffer_len)
    {
        p->buffer_alloc <<= 1;
    }
    p->base.buffer = realloc(p->base.buffer, p->buffer_alloc);
    if (!p->base.buffer)
    {
        p->base.error_line = __LINE__;
        return RINEX_ERR_SYSTEM;
    }
    memcpy(p->base.buffer, p->base.stream->buffer + p->parse_ofs,
        p->base.buffer_len);
    return RINEX_SUCCESS;
}

/* Documentation comment in rinex_p.h. */
int rnx_find_header
(
    const char in[],
    size_t in_size,
    const char header[],
    size_t sizeof_header
)
{
    char *pos;
    int ofs = 0, ii;

    while (1)
    {
        pos = memmem(in + ofs, in_size, header, sizeof_header - 1);
        if (!pos)
        {
            return RINEX_ERR_BAD_FORMAT;
        }
        if ((pos < in + 60) || (pos[-61] != '\n'))
        {
            ofs = (pos - in) + 1;
            continue;
        }

        for (ii = sizeof_header - 1; ii < 21; ++ii)
        {
            if (pos[ii] != ' ')
            {
                break;
            }
        }
        if (pos[ii] != '\n')
        {
            ofs = (pos - in) + ii;
            continue;
        }

        return pos - in - 60;
    }
}

RNX_RESOLVE(int, get_n_newlines,
    (struct rinex_parser *p, uint64_t whence, int n_lines),
    (p, whence, n_lines),
    avx2, neon)

/* Documentation comment in rinex_p.h. */
int rnx_get_newlines(
    struct rinex_parser *p,
    uint64_t *p_whence,
    int *p_body_ofs,
    int n_header,
    int n_body
)
{
    int ii = *p_whence, jj, res;

    if (n_header > 0)
    {
        ii = rnx_get_n_newlines(p, ii, n_header);
        if (ii > 0)
        {
            *p_body_ofs = ii;
            jj = rnx_get_n_newlines(p, ii, n_body);
        }
        else
        {
            jj = 0;
        }
    }
    else
    {
        jj = rnx_get_n_newlines(p, ii, n_body);
    }
    if (jj > 0)
    {
        return jj;
    }

    /* We should advance the stream (reading more data) and try again,
     * but if there is no old data to discard, we must have hit EOF.
     */
    if (*p_whence == 0)
    {
        p->error_line = __LINE__;
        return RINEX_EOF;
    }

    res = p->stream->advance(p->stream, BLOCK_SIZE, *p_whence);
    if (res)
    {
        errno = res;
        p->error_line = __LINE__;
        return RINEX_ERR_SYSTEM;
    }
    *p_whence = 0;

    return rnx_get_newlines(p, p_whence, p_body_ofs, n_header, n_body);
}

/* Documentation comment in rinex_p.h. */
int parse_fixed
(
    int64_t *p_out,
    const char *start,
    int width,
    int frac
)
{
    int64_t accum;
    int negate;
    int point;
    int ii;

    /* Skip leading whitespace. */
    ii = 0;
    point = width - frac;
    while ((ii < point - 1) && (start[ii] == ' '))
    {
        ++ii;
    }

    /* Is there a minus sign? */
    negate = 0;
    if ((ii < point - 1) && (start[ii] == '-'))
    {
        negate = 1;
        ++ii;
    }

    /* Get any remaining digits. */
    accum = 0;
    for (; ii < point - 1; ++ii)
    {
        if (!isdigit(start[ii]))
        {
            return EINVAL;
        }
        accum = (accum * 10) + (start[ii] - '0');
    }

    /* Check presence of decimal place. */
    if (start[ii++] != '.')
    {
        return EINVAL;
    }

    /* Add any additional digits. */
    for (; (ii < width) && isdigit(start[ii]); ++ii)
    {
        accum = (accum * 10) + (start[ii] - '0');
    }

    /* Check trailing spaces / newline. */
    while (ii < width)
    {
        accum *= 10;
        if (start[ii] == '\n')
        {
            /* stay at the newline */
        }
        else if (start[ii++] != ' ')
        {
            return EINVAL;
        }
    }

    /* All done. */
    *p_out = negate ? -accum : accum;
    return 0;
}

/* Documentation comment in rinex_p.h. */
int parse_uint
(
    int *p_out,
    const char *start,
    int width
)
{
    int value = 0;

    /* Skip past leading whitespace. */
    for (; width > 0 && *start == ' '; width--, start++)
    {
        /* no operation */
    }

    for (; width > 0; width--, start++)
    {
        if (!isdigit(*start))
        {
            return EINVAL;
        }

        value = value * 10 + *start - '0';
    }

    *p_out = value;
    return 0;
}
