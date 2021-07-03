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
    struct rnx_v23_parser *p,
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

/** rnx_get_n_newlines tries to ensure multiple lines are in \a p->stream.
 *
 * \param[in,out] p Parser needing data to be copied.
 * \param[in] whence Offset at which to start counting.
 * \param[in] n_lines Number of lines to fetch.
 * \returns Number of bytes in p->stream needed to get \a n_lines
 *   newlines, or non-positive rinex_error_t value on failure.
 */
static int rnx_get_n_newlines(
    struct rinex_parser *p,
    uint64_t whence,
    int n_lines
)
{
    int found = 0;
    const char * restrict buffer = p->stream->buffer;
#if defined(__AVX2__)
    const __m256i v_nl = _mm256_broadcastb_epi8(_mm_set1_epi8('\n'));

    for (; whence + 64 < p->stream->size; whence += 64)
    {
        const __m256i v_p_2 = _mm256_loadu_si256((__m256i const *)(buffer + whence + 32));
        const __m256i m_nl_2 = _mm256_cmpeq_epi8(v_nl, v_p_2);
        const __m256i v_p = _mm256_loadu_si256((__m256i const *)(buffer + whence));
        const __m256i m_nl = _mm256_cmpeq_epi8(v_nl, v_p);
        uint64_t kk = ((uint64_t)_mm256_movemask_epi8(m_nl_2) << 32)
            | (uint32_t)_mm256_movemask_epi8(m_nl);

        const int nn = __builtin_popcountll(kk);
        if (found + nn < n_lines)
        {
            found += nn;
            continue;
        }

        while (1)
        {
            int r = __builtin_ctzll(kk);
            kk &= (kk - 1);
            if (++found == n_lines)
            {
                return whence + r + 1;
            }
        }
    }
#elif defined(__ARM_NEON)
    const uint8x16_t v_nl = vdupq_n_u8('\n');
    const uint8x16_t v_m0 = { 0x01, 0x10, 0x01, 0x10, 0x01, 0x10, 0x01, 0x10,
        0x01, 0x10, 0x01, 0x10, 0x01, 0x10, 0x01, 0x10 };
    const uint8x16_t v_m1 = { 0x02, 0x20, 0x02, 0x20, 0x02, 0x20, 0x02, 0x20,
        0x02, 0x20, 0x02, 0x20, 0x02, 0x20, 0x02, 0x20 };
    const uint8x16_t v_m2 = { 0x04, 0x40, 0x04, 0x40, 0x04, 0x40, 0x04, 0x40,
        0x04, 0x40, 0x04, 0x40, 0x04, 0x40, 0x04, 0x40 };
    const uint8x16_t v_m3 = { 0x08, 0x80, 0x08, 0x80, 0x08, 0x80, 0x08, 0x80,
         0x08, 0x80, 0x08, 0x80, 0x08, 0x80, 0x08, 0x80 };

    for (; whence + 128 < p->stream->size; whence += 128)
    {
        uint8x16x4_t v_lo = vld4q_u8((const uint8_t *)(buffer + whence));
        uint8x16x4_t v_hi = vld4q_u8((const uint8_t *)(buffer + whence + 64));
        uint64x2_t v_mask;
        uint64_t m_lo;
        int nn;

        v_lo.val[0] = vceqq_u8(v_lo.val[0], v_nl);
        v_hi.val[0] = vceqq_u8(v_hi.val[0], v_nl);
        v_lo.val[0] = vandq_u8(v_m0, v_lo.val[0]);
        v_hi.val[0] = vandq_u8(v_m0, v_hi.val[0]);

        v_lo.val[1] = vceqq_u8(v_lo.val[1], v_nl);
        v_hi.val[1] = vceqq_u8(v_hi.val[1], v_nl);
        v_lo.val[1] = vbslq_u8(v_m1, v_lo.val[1], v_lo.val[0]);
        v_hi.val[1] = vbslq_u8(v_m1, v_hi.val[1], v_hi.val[0]);

        v_lo.val[2] = vceqq_u8(v_lo.val[2], v_nl);
        v_hi.val[2] = vceqq_u8(v_hi.val[2], v_nl);
        v_lo.val[2] = vbslq_u8(v_m2, v_lo.val[2], v_lo.val[1]);
        v_hi.val[2] = vbslq_u8(v_m2, v_hi.val[2], v_hi.val[1]);

        v_lo.val[3] = vceqq_u8(v_lo.val[3], v_nl);
        v_hi.val[3] = vceqq_u8(v_hi.val[3], v_nl);
        v_lo.val[3] = vbslq_u8(v_m3, v_lo.val[3], v_lo.val[2]);
        v_hi.val[3] = vbslq_u8(v_m3, v_hi.val[3], v_hi.val[2]);

        v_mask = vreinterpretq_u64_u8(vpaddq_u8(v_lo.val[3], v_hi.val[3]));

        m_lo = vgetq_lane_u64(v_mask, 0);
        nn = __builtin_popcountll(m_lo);
        if (found + nn < n_lines)
        {
            found += nn;
        }
        else while (1)
        {
            int r = __builtin_ctzll(m_lo);
            m_lo &= (m_lo - 1);
            if (++found == n_lines)
            {
                return whence + r + 1;
            }
        }

        uint64_t m_hi = vgetq_lane_u64(v_mask, 1);
        nn = __builtin_popcountll(m_hi);
        if (found + nn < n_lines)
        {
            found += nn;
        }
        else while (1)
        {
            int r = __builtin_ctzll(m_hi);
            m_hi &= (m_hi - 1);
            if (++found == n_lines)
            {
                return whence + r + 65;
            }
        }
    }
#endif

    for (; whence < p->stream->size; ++whence)
    {
        /* Was it a newline? */
        if (buffer[whence] == '\n')
        {
            if (++found == n_lines)
            {
                return whence + 1;
            }
        }
    }

    return 0;
}

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
