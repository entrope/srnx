/** rinex_p.c - Private utility functions for RINEX parsing.
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

#include "rinex_p.h"
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

long page_size;

/** dev_zero is a file descriptor for /dev/zero, used to mmap empty
 * pages past the end of real data (when needed).
 */
static int dev_zero;

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
