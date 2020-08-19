/** rinex_parse.c - RINEX parsing utilities.
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

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#ifdef __x86_64__
# include <x86intrin.h>
#endif

#define BLOCK_SIZE (1024 * 1024 - RINEX_EXTRA)

/** rnx_v23_parser is a RINEX v2.xx or v3.xx parser. */
struct rnx_v23_parser
{
    /** base is the standard rinex_parser contents. */
    struct rinex_parser base;

    /** buffer_alloc is the allocated length of #base.buffer. */
    int buffer_alloc;

    /** signal_alloc is the allocated length of #base.signal. */
    int signal_alloc;

    /** system_len is the length of #system and #system_id.
     *
     * In the case of #system_id, this does not count the trailing '\0'.
     */
    int system_len;

    /** obs_len is the length of #obs. */
    int obs_len;

    /** parse_ofs is the current read offset in base.stream->buffer. */
    int parse_ofs;

    /** system_id is a nul-terminated string identifying supported
     * systems.  system_id[ii] is the system identifier of the ii'th
     * satellite system.  system_id[system_len] is '\0'.
     */
    char *system_id;

    /** system identifies the ending indexes of GNSS systems in #obs.
     *
     * The ii'th system starts at obs[ii ? system[ii-1] : 0] and ends at
     * obs[system[ii]].
     */
    int *system;

    /** obs is a vector containing the possible observations for each
     * satellite in the file.  obs[ii].sv[0] may be the satellite system
     * code or '\0'; obs[ii].sv[1..3] are all '\0'.
     */
    rinex_signal_t *obs;
};

/** Searches for \a needle in \a haystack.
 *
 * \param[in] haystack Data to search in.
 * \param[in] h_size Number of bytes in \a haystack.
 * \param[in] needle Data to search for.
 * \param[in] n_size Number of bytes in \a needle.
 * \returns A pointer to the first instance of \a needle within
 *   \a haystack, or NULL if \a haystack does not contain \a needle.
 */
static void *memmem
(
    const char *haystack, size_t h_size,
    const char *needle, size_t n_size
)
{
    size_t ii;

    if (n_size > h_size)
    {
        assert(n_size <= h_size);
        return NULL;
    }

    for (ii = 0; ii < h_size - n_size; ++ii)
    {
        if (!memcmp(haystack + ii, needle, n_size))
        {
            return (void *)haystack + ii;
        }
    }

    return NULL;
}

/** Search for the RINEX "END OF HEADER" line.
 * 
 * \param[in] in Buffer containing the entire plausible header.
 * \param[in] in_size Length of \a in.
 * \returns RINEX error code on failure, length of the header otherwise.
 */
static int find_end_of_header
(
    const char in[],
    size_t in_size
)
{
    static const char end_of_header[] = "END OF HEADER";
    char *pos;
    int ofs = 0, ii;

    while (1)
    {
        pos = memmem(in + ofs, in_size, end_of_header, sizeof end_of_header - 1);
        if (!pos || pos < in + 60)
        {
            return RINEX_ERR_BAD_FORMAT;
        }
        if (pos[-61] != '\n')
        {
            ofs = (pos - in) + 1;
            continue;
        }

        for (ii = sizeof end_of_header - 1; ii < 21; ++ii)
        {
            if (pos[ii] != ' ')
            {
                break;
            }
        }

        if (pos[ii] != '\n')
        {
            return RINEX_ERR_BAD_FORMAT;
        }
        ofs = (pos - in) + 1 + ii;
        return ofs;
    }
}

/** Parses an unsigned integer field of \a width bytes starting at
 * \a start.
 *
 * If the field is all spaces (' '), writes 0 to \a *p_out and returns 0.
 *
 * \param[out] p_out Receives the parsed integer.
 * \param[in] start Start of the text field to parse.
 * \param[in] width Width of field in characters.
 * \returns Zero on success, else EINVAL if the field had any characters
 *   except spaces and digits or if a space followed a digit.
 */
static int parse_uint
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

/** Parses a fixed-point decimal field.
 *
 * A valid field consists of \a width - \a frac - 1 characters as a
 * signed integer, a decimal point ('.'), and \a frac characters as a
 * fractional part.
 *
 * The signed integer is zero or more spaces, an optional minus sign
 * ('-'), and zero or more digits. The fractional part has zero or more
 * digits followed by zero or more spaces (or a newline).
 *
 * Some examples of valid fixed-point decimals from the RINEX 2.11 and
 * 3.04 specifications are (between the double quotes):
 * "  4375274.   " and "         -.120".
 *
 * \param[out] p_out Receives the parsed value, times \a 10**point.
 * \param[in] start Start of the text field to parse.
 * \param[in] width Width of the total field in characters.
 * \param[in] frac Width of the fractional part in characters.
 * \returns Zero on success, else EINVAL if the field was invalid.
 */
static int parse_fixed
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

/** rnx_get_newline tries to find the first newline in \a s->buffer.
 *
 * \param[in,out] s Stream needing a newline.
 * \param[in] p_whence Offset at which to start counting.
 * \returns Byte offset of the first newline, or 0 for EOF, or
 *   (negative) rinex_error_t value on failure.
 */
static int rnx_get_newline(
    struct rinex_stream *s,
    int *p_whence
)
{
    int ii, res;

    /* Scanning for the first EOL is easy. */
    for (ii = *p_whence; ii < (int)s->size; ++ii)
    {
        if (s->buffer[ii] == '\n')
        {
            return ii + 1;
        }
    }

    /* We should advance the stream (reading more data) and try again,
     * but if there is no old data to discard, we must have hit EOF.
     */
    if (*p_whence == 0)
    {
        return 0;
    }

    res = s->advance(s, BLOCK_SIZE, *p_whence);
    if (res)
    {
        errno = res;
        return RINEX_ERR_SYSTEM;
    }
    *p_whence = 0;

    return rnx_get_newline(s, p_whence);
}

/** rnx_get_newlines tries to ensure multiple lines are in \a p->stream.
 *
 * \param[in,out] p Parser needing data to be copied.
 * \param[in] p_whence Offset at which to start counting.
 * \param[out] p_body_ofs Receives offset of body data within
 *   \a p->stream->buffer.
 * \param[in] n_header Number of "header" lines to fetch.
 * \param[in] n_body Number of "body" lines to fetch.
 * \returns Number of bytes in p->stream needed to get \a n_header +
 *   \a n_body newlines, or 0 for EOF, or rinex_error_t value on failure.
 */
static int rnx_get_newlines(
    struct rinex_parser *p,
    int *p_whence,
    int *p_body_ofs,
    int n_header,
    int n_body
)
{
    const char * restrict buffer = p->stream->buffer;
    int ii, jj, res, n_total;
#if defined(__AVX2__)
    const __m256i v_nl = _mm256_broadcastb_epi8(_mm_set1_epi8('\n'));
#endif

    ii = *p_whence;
    jj = 0;
    n_total = n_header + n_body;

#if defined(__AVX2__)
    for (; ii + 64 < (int)p->stream->size; ii += 64)
    {
        __m256i v_p   = _mm256_loadu_si256((__m256i const *)(buffer + ii));
        __m256i v_p_2 = _mm256_loadu_si256((__m256i const *)(buffer + ii + 32));
        __m256i m_nl   = _mm256_cmpeq_epi8(v_nl, v_p);
        __m256i m_nl_2 = _mm256_cmpeq_epi8(v_nl, v_p_2);
        uint64_t kk = (uint32_t)_mm256_movemask_epi8(m_nl)
            | ((uint64_t)_mm256_movemask_epi8(m_nl_2) << 32);
        while (kk)
        {
            int r = __builtin_ctzll(kk);
            kk &= (kk - 1);
            ++jj;
            if (jj == n_header)
            {
                *p_body_ofs = ii + r + 1;
            }
            if (jj == n_total)
            {
                return ii + r + 1;
            }
        }
    }
#endif

    for (; ii < (int)p->stream->size; ++ii)
    {
        /* Was it a newline? */
        if (buffer[ii] == '\n')
        {
            ++jj;
            if (jj == n_header)
            {
                *p_body_ofs = ii + 1;
            }
            if (jj == n_total)
            {
                return ii + 1;
            }
        }
    }

    /* We should advance the stream (reading more data) and try again,
     * but if there is no old data to discard, we must have hit EOF.
     */
    if (*p_whence == 0)
    {
        return 0;
    }

    res = p->stream->advance(p->stream, BLOCK_SIZE, *p_whence);
    if (res)
    {
        errno = res;
        return RINEX_ERR_SYSTEM;
    }
    *p_whence = 0;

    return rnx_get_newlines(p, p_whence, p_body_ofs, n_header, n_body);
}

static const char blank[] = "                ";

/** rnx_read_v2_observations reads observations from \a p. */
static rinex_error_t rnx_read_v2_observations(
    struct rnx_v23_parser *p,
    const char *epoch,
    const char *obs
)
{
    rinex_signal_t *signal;
    int ii, jj, kk, nn;
#if defined(__SSE4_1__)
    const __m128i v_nl = _mm_set1_epi8('\n');
    const __m128i v_sp = _mm_set1_epi8(' ');
    static const char v_mask[] = {
        255, 255, 255, 255, 255, 255, 255, 255,
        255, 255, 255, 255, 255, 255, 255, 255,
        0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0
    };
#endif

    /* Read observations for each satellite. */
    for (ii = nn = 0; ii < p->base.epoch.n_sats; ++ii)
    {
        /* Read each observation for this satellite. */
        for (jj = 0; jj < p->obs_len; ++jj)
        {
            /* If at EOL or a blank, skip this observation. */
            if (*obs == '\n')
            {
                goto eol;
            }
            if (!memcmp(obs, blank, 16))
            {
                obs += 16;
                goto eol;
            }

            /* Do we need more buffer space? */
            if ((nn + 1) * 16 >= p->buffer_alloc)
            {
                p->buffer_alloc *= 2;
                p->base.buffer = realloc(p->base.buffer, p->buffer_alloc);
                if (!p->base.buffer)
                {
                    return RINEX_ERR_SYSTEM;
                }
            }
            if (nn >= p->signal_alloc)
            {
                p->signal_alloc *= 2;
                p->base.signal = realloc(p->base.signal,
                    p->signal_alloc * sizeof p->base.signal[0]);
                if (!p->base.signal)
                {
                    return RINEX_ERR_SYSTEM;
                }
            }

            /* Record the signal name. */
            signal = &p->base.signal[nn];
            memcpy(signal, &p->obs[jj], sizeof *signal);
            memcpy(signal->id.sv, epoch + 32 + 3 * (ii % 12), 3);

            /* Copy the observation data. */
#if defined(__SSE4_1__)
            {
                __m128i v_obs = _mm_loadu_si128((const __m128i *)obs);
                __m128i m_nl = _mm_cmpeq_epi8(v_nl, v_obs);
                int mask = _mm_movemask_epi8(m_nl);
                int idx = __builtin_ctz(mask | 0x10000);
                __m128i m_sp = _mm_loadu_si128((const __m128i *)(v_mask + 16 - idx));
                __m128i res = _mm_blendv_epi8(v_sp, v_obs, m_sp);
                _mm_storeu_si128((__m128i *)(p->base.buffer + nn * 16), res);
                obs += idx;
            }
            (void)kk;
#else
            for (kk = 0; kk < 16; ++kk)
            {
                /* XXX: Check format? */
                p->base.buffer[nn * 16 + kk] = (*obs == '\n') ? ' ' : *obs++;
            }
#endif

            /* Bump our count of signals recorded. */
            nn++;

eol:
            /* There are up to five observations per line. */
            if ((jj % 5 == 4) || (jj + 1 == p->obs_len))
            {
                if (*obs != '\n')
                {
                    return RINEX_ERR_BAD_FORMAT;
                }
                obs++;
            }
        }

        /* There are 12 satellite names per header line. */
        if (ii % 12 == 11)
        {
            epoch = strchr(epoch, '\n') + 1;
        }
    }

    p->base.buffer_len = nn * 16;
    p->base.signal_len = nn;

    return 1;
}

/** rnx_read_v2 reads an observation data record from \a p_. */
static int rnx_read_v2(struct rinex_parser *p_)
{
    struct rnx_v23_parser *p = (struct rnx_v23_parser *)p_;
    const char *line;
    int64_t i64;
    rinex_error_t err;
    int res, yy, mm, dd, hh, min, n_sats, body_ofs, line_len;

    /* Make sure we have an epoch to parse. */
    res = rnx_get_newline(p->base.stream, &p->parse_ofs);
    if (res <= 0)
    {
        return res;
    }
    if (res < 33)
    {
        return RINEX_ERR_BAD_FORMAT;
    }
    line_len = res - 1 - p->parse_ofs;
    line = p->base.stream->buffer + p->parse_ofs;

    /* Parse the timestamp, epoch flag and "number of satellites" field. */
    if ((line[28] < '0' || line[28] > '6')
        || parse_uint(&yy, line+1, 2) || parse_uint(&mm, line+4, 2)
        || parse_uint(&dd, line+7, 2) || parse_uint(&hh, line+10, 2)
        || parse_uint(&min, line+13, 2) || parse_uint(&n_sats, line+29, 3)
        || parse_fixed(&i64, line+15, 11, 7))
    {
        return RINEX_ERR_BAD_FORMAT;
    }
    yy += (yy < 80) ? 2000 : 1900;
    p->base.epoch.yyyy_mm_dd = (yy * 100 + mm) * 100 + dd;
    p->base.epoch.hh_mm = hh * 100 + min;
    p->base.epoch.sec_e7 = i64;
    p->base.epoch.flag = line[28];
    p->base.epoch.n_sats = n_sats;

    /* Is there a receiver clock offset? */
    if (line_len <= 68)
    {
        p->base.epoch.clock_offset = 0;
    }
    else if (line_len == 80)
    {
        if (parse_fixed(&p->base.epoch.clock_offset, line+68, 12, 9))
        {
            return RINEX_ERR_BAD_FORMAT;
        }
    }
    else
    {
        return RINEX_ERR_BAD_FORMAT;
    }

    /* Is it a set of observations or a special event? */
    switch (p->base.epoch.flag)
    {
    case '0': case '1': case '6':
        /* Get enough data. */
        mm = (p->obs_len + 4) / 5; /* How many lines per satellite? */
        body_ofs = 0;
        res = rnx_get_newlines(p_, &p->parse_ofs, &body_ofs,
            (n_sats + 11) / 12, n_sats * mm);
        if (res < 0)
        {
            return res;
        }
        else if (res == 0)
        {
            return RINEX_ERR_BAD_FORMAT;
        }
        line = p->base.stream->buffer + p->parse_ofs;
        p->parse_ofs = res;

        return rnx_read_v2_observations(p, line,
            p->base.stream->buffer + body_ofs);

    case '2': case '3': case '4': case '5':
        /* Get the data. */
        p->parse_ofs = res;
        res = rnx_get_newlines(p_, &p->parse_ofs, NULL, 0, n_sats);
        if (res < 0)
        {
            err = res;
        }
        else if (res == 0)
        {
            return RINEX_ERR_BAD_FORMAT;
        }
        else
        {
            p->base.buffer_len = res - p->parse_ofs;
            memcpy(p->base.buffer, p->base.stream->buffer + p->parse_ofs,
                p->base.buffer_len);
            p->parse_ofs = res;
            err = 1;
        }
        p->base.signal_len = 0;

        /* and we are done */
        return err;
    }

    assert(0 && "logic failure in rnx_read_v2");
    return RINEX_ERR_BAD_FORMAT;
}

/** rnx_read_v3_observations reads observations from \a obs. */
static rinex_error_t rnx_read_v3_observations(
    struct rnx_v23_parser *p,
    const char obs[]
)
{
    rinex_signal_t *signal;
    const char *sv_id;
    int ii, jj, kk, nn, prev_obs, last_obs;
#if defined(__SSE4_1__)
    const __m128i v_nl = _mm_set1_epi8('\n');
    const __m128i v_sp = _mm_set1_epi8(' ');
    static const char v_mask[] = {
        255, 255, 255, 255, 255, 255, 255, 255,
        255, 255, 255, 255, 255, 255, 255, 255,
        0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0
    };
#endif

    /* Read observations for each satellite. */
    for (ii = nn = 0; ii < p->base.epoch.n_sats; ++ii)
    {
        /* Figure out which satellite system this satellite belongs to. */
        sv_id = obs;
        for (kk = 0; kk < p->system_len; ++kk)
        {
            if (sv_id[0] == p->system_id[kk])
            {
                break;
            }
        }
        if (kk == p->system_len)
        {
            return RINEX_ERR_BAD_FORMAT;
        }
        obs += 3;

        /* Find the per-system observation list and count. */
        prev_obs = kk ? p->system[kk-1] : 0;
        last_obs = p->system[kk];

        /* Read each observation for this satellite. */
        for (jj = prev_obs; jj < last_obs; ++jj)
        {
            /* If at EOL, we are done for this satellite. */
            if (*obs == '\n')
            {
                break;
            }

            /* If at a blank, skip this observation. */
            if (!memcmp(obs, blank, 16))
            {
                obs += 16;
                continue;
            }

            /* Do we need more buffer space? */
            if ((nn + 1) * 16 >= p->buffer_alloc)
            {
                p->buffer_alloc *= 2;
                p->base.buffer = realloc(p->base.buffer, p->buffer_alloc);
                if (!p->base.buffer)
                {
                    return RINEX_ERR_SYSTEM;
                }
            }
            if (nn >= p->signal_alloc)
            {
                p->signal_alloc *= 2;
                p->base.signal = realloc(p->base.signal,
                    p->signal_alloc * sizeof p->base.signal[0]);
                if (!p->base.signal)
                {
                    return RINEX_ERR_SYSTEM;
                }
            }

            /* Record the signal name. */
            signal = &p->base.signal[nn];
            memcpy(signal, &p->obs[jj], sizeof *signal);
            memcpy(signal->id.sv, sv_id, 3);

            /* Copy the observation data. */
#if defined(__SSE4_1__)
            {
                __m128i v_obs = _mm_loadu_si128((const __m128i *)obs);
                __m128i m_nl = _mm_cmpeq_epi8(v_nl, v_obs);
                int mask = _mm_movemask_epi8(m_nl);
                int idx = __builtin_ctz(mask | 0x10000);
                __m128i m_sp = _mm_loadu_si128((const __m128i *)(v_mask + 16 - idx));
                __m128i res = _mm_blendv_epi8(v_sp, v_obs, m_sp);
                _mm_storeu_si128((__m128i *)(p->base.buffer + nn * 16), res);
                obs += idx;
            }
            (void)kk;
#else
            for (kk = 0; kk < 16; ++kk)
            {
                /* XXX: Check format? */
                p->base.buffer[nn * 16 + kk] = (*obs == '\n') ? ' ' : *obs++;
            }
#endif

            /* Bump our count of signals recorded. */
            nn++;
        }

        if (*obs != '\n')
        {
            return RINEX_ERR_BAD_FORMAT;
        }
        obs++;
    }

    p->base.buffer_len = nn * 16;
    p->base.signal_len = nn;

    return 1;
}

/** rnx_read_v3 reads an observation data record from \a p_. */
static int rnx_read_v3(struct rinex_parser *p_)
{
    struct rnx_v23_parser *p = (struct rnx_v23_parser *)p_;
    const char *line;
    int64_t i64;
    int res, yy, mm, dd, hh, min, n_sats, line_len;

    /* Make sure we have an epoch to parse. */
    res = rnx_get_newline(p->base.stream, &p->parse_ofs);
    if (res <= 0)
    {
        return res;
    }
    if (res < 35)
    {
        return RINEX_ERR_BAD_FORMAT;
    }
    line_len = res - p->parse_ofs;
    line = p->base.stream->buffer + p->parse_ofs;
    p->parse_ofs = res;

    /* Parse the timestamp, epoch flag and "number of satellites" field. */
    if ((line[0] != '>' || line[31] < '0' || line[31] > '6')
        || parse_uint(&yy, line+2, 4) || parse_uint(&mm, line+7, 2)
        || parse_uint(&dd, line+10, 2) || parse_uint(&hh, line+13, 2)
        || parse_uint(&min, line+16, 2) || parse_uint(&n_sats, line+32, 3)
        || parse_fixed(&i64, line+18, 11, 7))
    {
        return RINEX_ERR_BAD_FORMAT;
    }
    p->base.epoch.yyyy_mm_dd = (yy * 100 + mm) * 100 + dd;
    p->base.epoch.hh_mm = hh * 100 + min;
    p->base.epoch.sec_e7 = i64;
    p->base.epoch.flag = line[28];
    p->base.epoch.n_sats = n_sats;

    /* Is there a receiver clock offset? */
    if (line_len <= 44)
    {
        p->base.epoch.clock_offset = 0;
    }
    else if (line_len == 59)
    {
        if (parse_fixed(&p->base.epoch.clock_offset, line+44, 15, 12))
        {
            return RINEX_ERR_BAD_FORMAT;
        }
    }
    else
    {
        return RINEX_ERR_BAD_FORMAT;
    }

    /* Get enough data. */
    res = rnx_get_newlines(p_, &p->parse_ofs, NULL, 0, n_sats);
    if (res < 0)
    {
        return res;
    }
    else if (res == 0)
    {
        return RINEX_ERR_BAD_FORMAT;
    }
    line_len = res - p->parse_ofs;
    line = p->base.stream->buffer + p->parse_ofs;
    p->parse_ofs = res;

    /* Is it a set of observations or a special event? */
    switch (p->base.epoch.flag)
    {
    case '0': case '1': case '6':
        return rnx_read_v3_observations(p, line);

    case '2': case '3': case '4': case '5':
        /* We already did most of the work. */
        memcpy(p->base.buffer, line, line_len);
        p->base.buffer_len = line_len;
        p->base.signal_len = 0;
        return 1;
    }

    assert(0 && "logic failure in rnx_read_v3");
    return RINEX_ERR_BAD_FORMAT;
}

/** rnx_free_v23 deallocates \a p_, which must be a rnx_v23_parser. */
static void rnx_free_v23(struct rinex_parser *p_)
{
    struct rnx_v23_parser *p = (struct rnx_v23_parser *)p_;

    free(p->base.buffer);
    free(p->base.signal);
    free(p->system_id);
    free(p->system);
    free(p->obs);
    free(p);
}

/** Finds the start of the first line with the given header label.
 *
 * \warning Has undefined behavior for the first header.  This should
 *   not be a problem because the first header should be at p->buffer
 *   anyway.
 * \param[in] p RINEX parser with header in \a p->buffer.
 * \param[in] label Header label to search for.
 * \param[in] sizeof_label Size of \a label, including nul terminator.
 * \returns A pointer into \a rnx->header such that \a !strcmp(ptr+60,label),
 *   or NULL if there is no header with the requested label.
 */
static const char *rnx_find_header
(
    const struct rinex_parser *p,
    const char label[],
    size_t sizeof_label
)
{
    char *pos;

    pos = memmem(p->buffer + 61, p->buffer_len - 61, label, sizeof_label - 1);
    if (!pos || pos[-61] != '\n')
    {
        return NULL;
    }

    return pos - 60;
}

/** rnx_open_v2 reads the observation codes in \a p->base.header. */
static rinex_error_t rnx_open_v2(struct rnx_v23_parser *p)
{
    static const char n_obs[] = "# / TYPES OF OBSERV";
    const char *line;
    int res, ii, jj;

    /* Find the (first?) PRN / # OF OBS line. */
    line = rnx_find_header(&p->base, n_obs, sizeof n_obs);
    if (!line)
    {
        return RINEX_ERR_BAD_FORMAT;
    }
    res = parse_uint(&p->obs_len, line, 6);
    if (res || (p->obs_len < 1))
    {
        return RINEX_ERR_BAD_FORMAT;
    }
    p->obs = calloc(p->obs_len, sizeof p->obs[0]);
    if (!p->obs)
    {
        return RINEX_ERR_SYSTEM;
    }

    /* Copy observation codes. */
    for (ii = 0; ii < p->obs_len; )
    {
        /* After the first line, find the next header and check it. */
        if (ii)
        {
            /* Find this line's EOL. */
            line = strchr(line, '\n');
            if (!line)
            {
                return RINEX_ERR_BAD_FORMAT;
            }

            /* Check the type of the next header line. */
            if (memcmp(++line + 60, n_obs, sizeof(n_obs) - 1))
            {
                return RINEX_ERR_BAD_FORMAT;
            }
        }

        /* Copy observation codes from this line. */
        for (jj = 0; (ii < p->obs_len) && (jj < 9); ++ii, ++jj)
        {
            p->obs[ii].id.obs[0] = line[10 + 6*jj];
            p->obs[ii].id.obs[1] = line[11 + 6*jj];
        }
    }

    /* Initially assume 20 satellites is enough space. */
    p->signal_alloc = 20 * p->obs_len;
    p->base.signal = calloc(p->signal_alloc, sizeof p->base.signal[0]);
    if (!p->base.signal)
    {
        return RINEX_ERR_SYSTEM;
    }

    return RINEX_SUCCESS;
}

/** rnx_open_v3 reads the observation codes in \a p->base.header. */
static rinex_error_t rnx_open_v3(struct rnx_v23_parser *p)
{
    static const char sys_n_obs[] = "SYS / # / OBS TYPES";
    const char *line;
    int res, ii, jj, kk, nn, n_obs, system_alloc, obs_alloc;

    /* Find the (first) SYS / # / OBS TYPES line. */
    line = rnx_find_header(&p->base, sys_n_obs, sizeof sys_n_obs);
    if (!line)
    {
        return RINEX_ERR_BAD_FORMAT;
    }

    /* Initially assume no more than eight satellite systems. */
    system_alloc = 8;
    p->system_id = calloc(system_alloc + 1, 1);
    if (!p->system_id)
    {
        return RINEX_ERR_SYSTEM;
    }
    p->system = calloc(system_alloc, sizeof p->system[0]);
    if (!p->system)
    {
        return RINEX_ERR_SYSTEM;
    }

    /* Initially (conservatively!) assume 20 observations per system. */
    obs_alloc = 20 * system_alloc;
    p->obs = calloc(obs_alloc, sizeof p->obs[0]);
    if (!p->obs)
    {
        return RINEX_ERR_SYSTEM;
    }

    /* Keep going until we find a different header label. */
    for (kk = nn = 0; !memcmp(line + 60, sys_n_obs, sizeof(sys_n_obs) - 1); )
    {
        /* How many observations for this system? */
        res = parse_uint(&n_obs, line + 3, 3);
        if (res || (n_obs < 1))
        {
            return RINEX_ERR_BAD_FORMAT;
        }

        /* Do we need to increase system_alloc? */
        if (kk >= system_alloc)
        {
            system_alloc *= 2;
            p->system_id = realloc(p->system_id, system_alloc + 1);
            p->system = realloc(p->system, system_alloc * sizeof(p->system[0]));
            if (!p->system_id || !p->system)
            {
                return RINEX_ERR_SYSTEM;
            }
        }

        /* Update the per-system info in \a *p. */
        p->system_id[kk] = line[0];
        p->system[kk] = n_obs + (kk ? p->system[kk-1] : 0);

        /* If necessary, increase the size of p->obs. */
        if (p->system[kk] >= obs_alloc)
        {
            /* Increase obs_alloc as needed. */
            while (p->system[kk] >= obs_alloc)
            {
                obs_alloc *= 2;
            }
            p->obs = realloc(p->obs, obs_alloc * sizeof(p->obs[0]));
            if (!p->obs)
            {
                return RINEX_ERR_SYSTEM;
            }
        }

        /* Increment our system count. */
        kk++;

        /* Copy observation codes. */
        for (ii = 0; ii < n_obs; )
        {
            /* After the first line, check that the next one is right. */
            if (ii)
            {
                line = strchr(line, '\n') + 1;
                if ((line[0] != ' ')
                    || memcmp(line + 60, sys_n_obs, sizeof(sys_n_obs) - 1))
                {
                    return RINEX_ERR_BAD_FORMAT;
                }
            }

            /* Copy observation codes from this line. */
            for (jj = 0; (ii < n_obs) && (jj < 13); ++ii, ++jj, ++nn)
            {
                /* Zero-init because realloc() does not zero new areas. */
                p->obs[nn].u64 = 0;
                p->obs[nn].id.sv[0] = line[0];
                p->obs[nn].id.obs[0] = line[7 + 4*jj];
                p->obs[nn].id.obs[1] = line[8 + 4*jj];
                p->obs[nn].id.obs[2] = line[9 + 4*jj];
            }
        }
        line = strchr(line, '\n') + 1;
    }

    /* Shrink the arrays we allocated to fit their contents. */
    p->system_len = kk;
    p->system_id = realloc(p->system_id, p->system_len + 1);
    p->system = realloc(p->system, p->system_len * sizeof(p->system[0]));

    p->obs_len = nn;
    p->obs = realloc(p->obs, p->obs_len * sizeof(p->obs[0]));

    /* Initially assume 500 signals is enough space.
     * (This is slightly under 2 KiB, which allows some overhead for a
     * buddy-type allocator, without much internal fragmentation.)
     */
    p->signal_alloc = 500;
    p->base.signal = calloc(p->signal_alloc, sizeof p->base.signal[0]);
    if (!p->base.signal)
    {
        return RINEX_ERR_SYSTEM;
    }

    return RINEX_SUCCESS;
}

/** Copy \a in_len bytes of header from \a in to \a out.
 *
 * Replace each newline sequences (CR, LF, CRLF) with a single '\n'.
 * Remove trailing spaces from each header line.
 *
 * \param[out] out Receives cleaned-up header.
 * \param[in] in Input header to copy from.
 * \param[in] in_len Length of header to copy, including the final
 *   newline sequence.
 * \returns Length of output on success, RINEX_ERR_BAD_FORMAT on illegal
 *   line length.
 */
static rinex_error_t rnx_copy_header
(
    char out[],
    const char in[],
    int in_len
)
{
    int ii, jj, last_eol, line_len;

    for (ii = jj = last_eol = 0; ii < in_len; ++ii)
    {
        /* Convert CRLF to '\n'. */
        out[jj] = in[ii];

        /* Are we at an EOL? */
        if (out[jj] == '\n')
        {
            /* Check total line length. */
            line_len = jj - last_eol;
            if (line_len < 61 || line_len > 80)
            {
                return RINEX_ERR_BAD_FORMAT;
            }

            /* Trim trailing whitespace. */
            while (out[jj-1] == ' ')
            {
                jj--;
            }
            out[jj] = '\n';
            last_eol = jj+1;
        }
        ++jj;
    }

    return 0;
}

/* Doc comment is in rinex.h. */
rinex_error_t rinex_open
(
    struct rinex_parser **p_parser,
    struct rinex_stream *stream
)
{
    struct rnx_v23_parser *p;
    rinex_error_t err;
    int res;

    if (*p_parser)
    {
        (*p_parser)->destroy(*p_parser);
        *p_parser = NULL;
    }

    res = stream->advance(stream, BLOCK_SIZE, 0);
    if (res || stream->size < 80)
    {
        errno = res;
        return RINEX_ERR_SYSTEM;
    }

    /* Is it an uncompressed RINEX file? */
    p = NULL;
    if (!memcmp("RINEX VERSION / TYPE", stream->buffer + 60, 20))
    {
        /* Check that it's an observation file. */
        if (stream->buffer[20] != 'O')
        {
            return RINEX_ERR_NOT_OBSERVATION;
        }

        /* Check for END OF HEADER. */
        res = find_end_of_header(stream->buffer, stream->size);
        if (res < 1)
        {
            return RINEX_ERR_BAD_FORMAT;
        }

        /* Allocate the parser structure. */
        if (memcmp("     2.", stream->buffer, 7)
            && memcmp("     3.", stream->buffer, 7))
        {
            return RINEX_ERR_UNKNOWN_VERSION;
        }
        p = calloc(1, sizeof(struct rnx_v23_parser));
        if (!p)
        {
            errno = ENOMEM;
            return RINEX_ERR_SYSTEM;
        }
        p->base.signal = NULL;
        p->base.stream = stream;
        p->base.destroy = rnx_free_v23;

        /* Copy the header. */
        p->parse_ofs = res;
        p->buffer_alloc = res;
        p->base.buffer = calloc(p->buffer_alloc, 1);
        if (!p->base.buffer)
        {
            errno = ENOMEM;
            free(p);
            return RINEX_ERR_SYSTEM;
        }

        /* Copy the header for the caller's use. */
        res = rnx_copy_header(p->base.buffer, stream->buffer, res);
        if (res < 0)
        {
            free(p);
            return (rinex_error_t)res;
        }

        p->base.buffer_len = res;
        if (!memcmp("     2.", stream->buffer, 7))
        {
            p->base.read = rnx_read_v2;
            err = rnx_open_v2(p);
        }
        else
        {
            p->base.read = rnx_read_v3;
            err = rnx_open_v3(p);
        }
        if (err != RINEX_SUCCESS)
        {
            rnx_free_v23(&p->base);
            return err;
        }

        *p_parser = &p->base;
        return RINEX_SUCCESS;
    }

    /* TODO: support files using Hatanaka compression:
    if (!memcmp("CRX VERS   / TYPE", stream->buffer + 60, 20)) { .. }
     */

    return RINEX_ERR_BAD_FORMAT;
}

/* Doc comment is in rinex.h. */
int64_t rinex_parse_obs(const char c[])
{
    int64_t value;

    if (parse_fixed(&value, c, 14, 3))
    {
        value = INT64_MIN;
    }

    return value;
}
