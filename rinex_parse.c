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

#include "rinex_p.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#if defined(__AVX2__)

static __m256i rnx_parse_4(
    const __m128i *v_obs
)
{
    const __m256i v_atoi = _mm256_setr_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8,
        9, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0, 0, 0, 0, 0, 0);
    const __m256i mul_1_10 = _mm256_setr_epi8(10, 1, 10, 1, 10, 1, 10,
        1, 10, 1, 0, 1, 10, 1, 0, 0, 10, 1, 10, 1, 10, 1, 10, 1, 10, 1,
        0, 1, 10, 1, 0, 0);
    const __m256i mul_1_100 = _mm256_setr_epi16(100, 1, 100, 1, 10, 1,
        1, 0, 100, 1, 100, 1, 10, 1, 1, 0);
    const __m256i weight_3 = _mm256_setr_epi16(10000, 1, 100, 1, 10000,
        1, 100, 1, 10000, 1, 100, 1, 10000, 1, 100, 1);
    const __m256i weight_4 = _mm256_setr_epi32(100000, 1, 100000, 1,
        100000, 1, 100000, 1);
    const __m256i v_minus = _mm256_setr_epi8('-', '-', '-', '-', '-',
        '-', '-', '-', '-', '-', 0, 0, 0, 0, 0, 0, '-', '-', '-', '-',
        '-', '-', '-', '-', '-', '-', 0, 0, 0, 0, 0, 0);
    const __m256i p_0 = _mm256_loadu_si256((const __m256i *)(v_obs + 0));
    const __m256i p_1 = _mm256_loadu_si256((const __m256i *)(v_obs + 2));

    /* Convert digits to their values. */
    const __m256i t0_0 = _mm256_shuffle_epi8(v_atoi, p_0);
    const __m256i t0_1 = _mm256_shuffle_epi8(v_atoi, p_1);
    /* Accumulate adjacent digits into two-digit int16_t's. */
    const __m256i t1_0 = _mm256_maddubs_epi16(t0_0, mul_1_10);
    const __m256i t1_1 = _mm256_maddubs_epi16(t0_1, mul_1_10);
    /* Accumulate adjacent (two-digit) int16_t's into int32_t's. */
    const __m256i t2_0 = _mm256_madd_epi16(t1_0, mul_1_100);
    const __m256i t2_1 = _mm256_madd_epi16(t1_1, mul_1_100);
    /* Our int32_t's are only in the range 0..9999, so pack down. */
    const __m256i t3 = _mm256_packus_epi32(t2_0, t2_1);
    /* Combine adjacent (four-digit) int16_t's into int32_t's. */
    const __m256i t4 = _mm256_madd_epi16(t3, weight_3);
    /* Scale the high-order 32-bit values by 1e5. */
    const __m256i t5 = _mm256_mul_epu32(t4, weight_4);
    /* Shift the low-order 32-bit values "down". */
    const __m256i t6 = _mm256_srli_epi64(t4, 32);
    /* Add the low- and high-order 32-bit values (extended to 64 bits). */
    const __m256i t7 = _mm256_add_epi64(t5, t6);

    /* There is no _mm256_sign_epi64, unfortunately... */
    const __m256i v_zero = _mm256_setzero_si256();
    const __m256i v_ones = _mm256_cmpeq_epi64(v_zero, v_zero);
    __m256i mask = v_zero;
    int neg_0 = _mm256_movemask_epi8(_mm256_cmpeq_epi8(v_minus, p_0));
    if (neg_0 >> 16)
    {
        mask = _mm256_blend_epi32(mask, v_ones, 0x03);
    }
    if (neg_0 & 65535)
    {
        mask = _mm256_blend_epi32(mask, v_ones, 0x0c);
    }
    int neg_1 = _mm256_movemask_epi8(_mm256_cmpeq_epi8(v_minus, p_1));
    if (neg_1 >> 16)
    {
        mask = _mm256_blend_epi32(mask, v_ones, 0x30);
    }
    if (neg_1 & 65535)
    {
        mask = _mm256_blend_epi32(mask, v_ones, 0xc0);
    }
    const __m256i t8 = _mm256_sub_epi64(v_zero, t7);
    return _mm256_blendv_epi8(t7, t8, mask);
}

static const char *rnx_buffer_and_parse_obs
(
    const char *obs,
    __m128i *p_out
)
{
    const __m128i v_nl = _mm_set1_epi8('\n');
    const __m128i v_sp = _mm_set1_epi8(' ');

    __m128i v_obs = _mm_loadu_si128((const __m128i *)obs);
    __m128i m_nl = _mm_cmpeq_epi8(v_obs, v_nl);
    const int mask = _mm_movemask_epi8(m_nl);
    const int idx = __builtin_ctz(mask | 0x10000);
    __m128i m_nl_1 = _mm_or_si128(m_nl,   _mm_bslli_si128(m_nl, 1));
    __m128i m_nl_2 = _mm_or_si128(m_nl_1, _mm_bslli_si128(m_nl_1, 2));
    __m128i m_nl_3 = _mm_or_si128(m_nl_2, _mm_bslli_si128(m_nl_2, 4));
    __m128i m_sp   = _mm_or_si128(m_nl_3, _mm_bslli_si128(m_nl_3, 8));
    *p_out = _mm_blendv_epi8(v_obs, v_sp, m_sp);
    return obs + idx;
}

static void rnx_parse_final_avx2
(
    const __m128i v_obs[8],
    int64_t *base,
    char *lli,
    char *ssi,
    int count
)
{
    __m256i res_lo = rnx_parse_4(v_obs + 0);
    __m256i res_hi = rnx_parse_4(v_obs + 4);

    switch (count)
    {
    case 7:
        lli[6] = _mm_extract_epi8(v_obs[6], 14);
        ssi[6] = _mm_extract_epi8(v_obs[6], 15);
        base[6] = _mm256_extract_epi64(res_hi, 2);
        /* fall through */
    case 6:
        lli[5] = _mm_extract_epi8(v_obs[5], 14);
        ssi[5] = _mm_extract_epi8(v_obs[5], 15);
        base[5] = _mm256_extract_epi64(res_hi, 1);
        /* fall through */
    case 5:
        lli[4] = _mm_extract_epi8(v_obs[4], 14);
        ssi[4] = _mm_extract_epi8(v_obs[4], 15);
        base[4] = _mm256_extract_epi64(res_hi, 0);
        /* fall through */
    case 4:
        lli[3] = _mm_extract_epi8(v_obs[3], 14);
        ssi[3] = _mm_extract_epi8(v_obs[3], 15);
        base[3] = _mm256_extract_epi64(res_lo, 3);
        /* fall through */
    case 3:
        lli[2] = _mm_extract_epi8(v_obs[2], 14);
        ssi[2] = _mm_extract_epi8(v_obs[2], 15);
        base[2] = _mm256_extract_epi64(res_lo, 2);
        /* fall through */
    case 2:
        lli[1] = _mm_extract_epi8(v_obs[1], 14);
        ssi[1] = _mm_extract_epi8(v_obs[1], 15);
        base[1] = _mm256_extract_epi64(res_lo, 1);
        /* fall through */
    case 1:
        lli[0] = _mm_extract_epi8(v_obs[0], 14);
        ssi[0] = _mm_extract_epi8(v_obs[0], 15);
        base[0] = _mm256_extract_epi64(res_lo, 0);
    }
}

#elif defined(__ARM_NEON)

static const char *rnx_parse_obs_neon
(
    const char *obs,
    int64_t *p_obs,
    uint8x16_t *p_lli,
    uint8x16_t *p_ssi
)
{
    const uint8x16_t v_zero = vdupq_n_u8(0);

    /* This interleaves two paths: if there is a newline, replace it and
     * everything after it with spaces (for LLI and SSI); and converting
     * digits in the observation part to their values.  The observation
     * part can normally contain digits or any of [-.\r\n]; we subtract
     * '0' and use VTBL to index the value.
     *
     * These are interleaved because a typical ARMv8 core has at least
     * two Advanced SIMD (Neon) execution units (and I don't trust
     * compilers to look far enough ahead to exploit the interleaving).
     */
    const uint8x16_t v_orig = vld1q_u8((const uint8_t *)obs);
    const uint8x16_t m_nl_0 = vceqq_u8(v_orig, vdupq_n_u8('\n'));
    uint8x16_t m_digits = vsubq_u8(v_orig, vdupq_n_u8('0'));
    const uint8x16_t m_nl_1 = vorrq_u8(m_nl_0, vextq_u8(m_nl_0, v_zero, 1));
    const uint8x16_t v_digits = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0, 0, 0, 0, 0, 0 };
    const uint8x16_t m_nl_2 = vorrq_u8(m_nl_1, vextq_u8(m_nl_1, v_zero, 2));
    m_digits = vqtbl1q_u8(m_digits, v_digits);
    const uint8x16_t m_nl_3 = vorrq_u8(m_nl_2, vextq_u8(m_nl_2, v_zero, 4));
    const uint8x16_t m_mask = vorrq_u8(m_mask, vextq_u8(m_nl_3, v_zero, 8));
    m_digits = vbicq_u8(m_digits, m_mask);
    const int idx = 256 - vaddvq_u8(m_mask);
    const uint8x16_t v_1_10 = { 10, 1, 10, 1, 10, 1, 10, 1, 10, 1, 0, 1, 10, 1, 0, 0 };
    m_digits = vmulq_u8(m_digits, v_1_10);
    const uint8x16_t m_obs = vbslq_u8(m_mask, vdupq_n_u8(' '), v_orig);
    uint16x8_t m_dig_u16 = vpaddq_u8(m_digits);
    const uint16x8_t v_1_100 = { 100, 1, 100, 1, 10, 1, 1, 0 };
    m_dig_u16 = vmulq_u16(m_dig_u16, v_1_100);
    uint32x4_t m_dig_u32 = vpaddq_u16(m_dig_u16);
    const uint32x4_t v_

    *p_lli = vextq_u8(vdupq_n_u8(m_obs[14]), *p_lli, 1);
    *p_ssi = vextq_u8(m_obs, *p_ssi, 1);

    /* TODO: convert observation value to int64_t */

    *p_obs = 0;
    return obs + idx;
}

static void rnx_movemask_neon
(
    char *out,
    uint8_t n_bytes,
    uint8x16_t v_data
)
{
    const uint8x16_t v_pos = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
    const uint8x16_t v_in = vld1q_u8((const uint8_t *)out);
    const uint8x16_t v_n_bytes = vdupq_n_u8(n_bytes);
    uint8x16_t v_mask  = vcleq_u8(v_pos, v_n_bytes);
    v_mask = vbslq_u8(v_mask, v_data, v_in);
    vst1q_u8((uint8_t *)out, v_mask);
}

#else

static const char *rnx_parse_obs
(
    struct rnx_v23_parser *p,
    const char *obs,
    int nn
)
{
    char buf[16];
    int kk;

    /* The first 11 characters must be present: space, minus, digit or dot. */
    for (kk = 0; kk < 10; ++kk)
    {
        if (*obs != ' ') break;
        buf[kk] = *obs++;
    }
    if (kk < 10 && *obs == '-')
    {
        buf[kk++] = *obs++;
    }
    for (; kk < 10; ++kk)
    {
        if (*obs < '0' || *obs > '9') break;
        buf[kk] = *obs++;
    }
    if (kk != 10 || *obs++ != '.')
    {
        return NULL;
    }
    buf[kk++] = '.';
    for (; kk < 16; ++kk)
    {
        buf[kk] = (*obs == '\n') ? ' ' : *obs++;
    }

    /* Save the LLI and SSI. */
    p->base.lli[nn] = buf[14];
    p->base.ssi[nn] = buf[15];

    /* Parse the observation value. */
#define DVAL(X) ((X == ' ') ? 0 : (X - '0'))
    p->base.obs[nn] = DVAL(buf[13])
        + 10 * DVAL(buf[12])
        + 100 * DVAL(buf[11])
        /* buf[10] == '.', at least in theory */
        + 1000 * DVAL(buf[9])
        + 10000 * DVAL(buf[8])
        + 100000 * DVAL(buf[7])
        + 1000000 * DVAL(buf[6])
        + 10000000 * DVAL(buf[5])
        + 100000000 * DVAL(buf[4])
        + INT64_C(1000000000) * DVAL(buf[3])
        + INT64_C(10000000000) * DVAL(buf[2])
        + INT64_C(100000000000) * DVAL(buf[1])
        + INT64_C(1000000000000) * DVAL(buf[0]);
#undef DVAL

    return obs;
}

#endif /* SIMD instruction set */

static const char blank[] = "                ";

/** rnx_read_v2_observations reads observations from \a p. */
static rinex_error_t rnx_read_v2_observations(
    struct rnx_v23_parser *p,
    const char *epoch,
    const char *obs
)
{
#if defined(__AVX2__)
    __m128i v_obs[8];
#elif defined(__ARM_NEON)
    uint8x16_t v_lli = vdupq_n_u8(0);
    uint8x16_t v_ssi = vdupq_n_u8(0);
#endif
    char *buffer;
    int ii, jj, nn;

    /* Read observations for each satellite. */
    p->base.buffer_len = 0;
    buffer = p->base.buffer + p->base.buffer_len;
    for (ii = nn = 0; ii < p->base.epoch.n_sats; ++ii)
    {
        /* Determine the satellite identifier. */
        const char *sv_name = epoch + 32 + 3 * (ii % 12);
        int n_obs = p->base.n_obs[sv_name[0] & 31];
        char svn = (sv_name[1] - '0') * 10 + sv_name[2] - '0';
        uint8_t obs_mask = 0; /* presence bitmask for observations */

        /* There are 12 satellite names per header line. */
        if (ii % 12 == 11)
        {
            epoch = strchr(epoch, '\n') + 1;
        }

        /* Grow buffer if needed. */
        if (p->buffer_alloc < p->base.buffer_len + 2 + (n_obs + 7) / 8)
        {
            /* n_obs <= 25, so we need to add at most 6 bytes; but
             * buffer_alloc was already big enough for the file header.
             */
            p->buffer_alloc <<= 1;
            p->base.buffer = realloc(p->base.buffer, p->buffer_alloc);
            if (!p->base.buffer)
            {
                p->base.error_line = __LINE__;
                return RINEX_ERR_SYSTEM;
            }
            buffer = p->base.buffer + p->base.buffer_len;
        }

        /* Save satellite identifier. */
        *buffer++ = sv_name[0];
        *buffer++ = svn;

        /* Read each observation for this satellite. */
        for (jj = 0; jj < n_obs; ++jj)
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
            if (nn >= p->obs_alloc)
            {
                p->obs_alloc *= 2;
                p->base.lli = realloc(p->base.lli, p->obs_alloc);
                p->base.ssi = realloc(p->base.ssi, p->obs_alloc);
                p->base.obs = realloc(p->base.obs,
                    p->obs_alloc * sizeof p->base.obs[0]);
                if (!p->base.lli || !p->base.ssi || !p->base.obs)
                {
                    p->base.error_line = __LINE__;
                    return RINEX_ERR_SYSTEM;
                }
            }

            /* Copy the observation data. */
#if defined(__AVX2__)
            obs = rnx_buffer_and_parse_obs(obs, v_obs + (nn & 7));
            if ((nn & 7) == 7)
            {
                __m128i lli_ssi_01 = _mm_unpackhi_epi8(v_obs[0], v_obs[1]);
                __m128i lli_ssi_23 = _mm_unpackhi_epi8(v_obs[2], v_obs[3]);
                __m128i lli_ssi_45 = _mm_unpackhi_epi8(v_obs[4], v_obs[5]);
                __m128i lli_ssi_67 = _mm_unpackhi_epi8(v_obs[6], v_obs[7]);
                __m128i lli_ssi_03 = _mm_unpackhi_epi16(lli_ssi_01, lli_ssi_23);
                __m128i lli_ssi_47 = _mm_unpackhi_epi16(lli_ssi_45, lli_ssi_67);
                __m128i lli_ssi = _mm_unpackhi_epi32(lli_ssi_03, lli_ssi_47);
                _mm_storel_epi64((__m128i *)(p->base.lli + nn - 7), lli_ssi);
                _mm_storel_epi64((__m128i *)(p->base.ssi + nn - 7),
                    _mm_shuffle_epi32(lli_ssi, 0xB1));
                _mm256_storeu_si256((__m256i *)(p->base.obs + nn - 7),
                    rnx_parse_4(v_obs));
                _mm256_storeu_si256((__m256i *)(p->base.obs + nn - 3),
                    rnx_parse_4(v_obs + 4));
            }
#elif defined(__ARM_NEON)
            obs = rnx_parse_obs_neon(obs, p->base.obs + nn, &v_lli, &v_ssi);
            if (nn & 15 == 15)
            {
                vst1q_u8((uint8_t *)(p->base.lli + nn - 15), v_lli);
                vst1q_u8((uint8_t *)(p->base.ssi + nn - 15), v_ssi);
            }
#else
            obs = rnx_parse_obs(p, obs, nn);
#endif
            if (!obs)
            {
                p->base.error_line = __LINE__;
                return RINEX_ERR_BAD_FORMAT;
            }

            /* Remember that we saw this signal. */
            obs_mask |= 1 << (jj & 7);
            nn++;

eol:
            /* Update presence bitmasks. */
            if (((jj + 1) == n_obs) || ((jj & 7) == 7))
            {
                *buffer++ = obs_mask;
                obs_mask = 0;
            }

            /* There are up to five observations per line. */
            if (((jj + 1) == n_obs) || ((jj % 5) == 4))
            {
                if (*obs != '\n')
                {
                    p->base.error_line = __LINE__;
                    return RINEX_ERR_BAD_FORMAT;
                }
                obs++;
            }
        }

        /* Make sure buffer_len is updated. */
        p->base.buffer_len = buffer - p->base.buffer;
    }

#if defined(__AVX2__)
    if (nn & 7)
    {
        rnx_parse_final_avx2(v_obs, p->base.obs + (nn & ~7),
            p->base.lli + (nn & ~7), p->base.ssi + (nn & ~7), nn & 7);
    }
#elif defined(__ARM_NEON)
    if (nn & 15)
    {
        rnx_movemask_neon(p->base.lli + nn & ~15, nn & 15, v_lli);
        rnx_movemask_neon(p->base.ssi + nn & ~15, nn & 15, v_ssi);
    }
#endif

    return RINEX_SUCCESS;
}

/** rnx_read_v2 reads an observation data record from \a p_. */
static rinex_error_t rnx_read_v2(struct rinex_parser *p_)
{
    struct rnx_v23_parser *p = (struct rnx_v23_parser *)p_;
    const char *line;
    int64_t i64;
    rinex_error_t err;
    int res, yy, mm, dd, hh, min, n_sats, body_ofs, line_len;

    /* Make sure we have an epoch to parse. */
    res = rnx_get_newlines(p_, &p->parse_ofs, NULL, 0, 1);
    if (res <= RINEX_EOF)
    {
        return res;
    }
    if (res < 33)
    {
        p_->error_line = __LINE__;
        return RINEX_ERR_BAD_FORMAT;
    }
    line_len = res - 1 - p->parse_ofs;
    line = p->base.stream->buffer + p->parse_ofs;

    /* Parse the timestamp, epoch flag and "number of satellites" field. */
    i64 = 0;
    yy = mm = dd = hh = min = n_sats = 0;
    if ((line[28] < '0' || line[28] > '6')
        || parse_uint(&yy, line+1, 2) || parse_uint(&mm, line+4, 2)
        || parse_uint(&dd, line+7, 2) || parse_uint(&hh, line+10, 2)
        || parse_uint(&min, line+13, 2) || parse_uint(&n_sats, line+29, 3)
        || parse_fixed(&i64, line+15, 11, 7))
    {
        if (line[28] < '2' || line[28] == '6')
        {
            p_->error_line = __LINE__;
            return RINEX_ERR_BAD_FORMAT;
        }
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
            p_->error_line = __LINE__;
            return RINEX_ERR_BAD_FORMAT;
        }
    }
    else
    {
        p_->error_line = __LINE__;
        return RINEX_ERR_BAD_FORMAT;
    }

    /* Is it a set of observations or a special event? */
    switch (p->base.epoch.flag)
    {
    case '0': case '1': case '6':
        /* Get enough data. */
        mm = (p->base.n_obs[0] + 4) / 5; /* How many lines per satellite? */
        body_ofs = 0;
        res = rnx_get_newlines(p_, &p->parse_ofs, &body_ofs,
            (n_sats + 11) / 12, n_sats * mm);
        if (res <= RINEX_EOF)
        {
            if (res == RINEX_EOF)
            {
                res = RINEX_ERR_BAD_FORMAT;
            }
            p_->error_line = __LINE__;
            return res;
        }
        line = p->base.stream->buffer + p->parse_ofs;
        p->parse_ofs = res;

        return rnx_read_v2_observations(p, line,
            p->base.stream->buffer + body_ofs);

    case '2': case '3': case '4': case '5':
        /* Get the data. */
        res = rnx_get_newlines(p_, &p->parse_ofs, NULL, 0, n_sats + 1);
        if (res <= 0)
        {
            if (res == RINEX_EOF)
            {
                res = RINEX_ERR_BAD_FORMAT;
            }
            p_->error_line = __LINE__;
            err = res;
        }
        else
        {
            p->base.buffer_len = res - p->parse_ofs;

            while (p->buffer_alloc < p->base.buffer_len)
            {
                p->buffer_alloc <<= 1;
            }
            p->base.buffer = realloc(p->base.buffer, p->buffer_alloc);
            if (!p->base.buffer)
            {
                p_->error_line = __LINE__;
                return RINEX_ERR_SYSTEM;
            }
            memcpy(p->base.buffer, p->base.stream->buffer + p->parse_ofs,
                p->base.buffer_len);
            p->parse_ofs = res;
            err = RINEX_SUCCESS;
        }

        /* and we are done */
        return err;
    }

    p_->error_line = __LINE__;
    assert(0 && "logic failure in rnx_read_v2");
    return RINEX_ERR_BAD_FORMAT;
}

/** rnx_read_v3_observations reads observations from \a obs. */
static rinex_error_t rnx_read_v3_observations(
    struct rnx_v23_parser *p,
    const char obs[]
)
{
#if defined(__AVX2__)
    __m128i v_obs[8];
#elif defined(__ARM_NEON)
    uint8x16_t v_lli = vdupq_n_u8(0);
    uint8x16_t v_ssi = vdupq_n_u8(0);
#endif
    char *buffer;
    int ii, jj, nn;

    /* Read observations for each satellite. */
    p->base.buffer_len = 0;
    buffer = p->base.buffer + p->base.buffer_len;
    for (ii = nn = 0; ii < p->base.epoch.n_sats; ++ii)
    {
        /* Look up the satellite system's observation count. */
        const char *sv_name = obs;
        short n_obs = p->base.n_obs[sv_name[0] & 31];
        char svn = (sv_name[1] - '0') * 10 + sv_name[2] - '0';
        uint8_t obs_mask = 0; /* presence bitmask for observations */
        obs += 3;

        /* Grow buffer if needed. */
        if (p->buffer_alloc < p->base.buffer_len + 2 + (n_obs + 7) / 8)
        {
            /* n_obs <= 999, so we need to add at most 128 bytes, and
             * we know buffer_alloc will be at least that big to have
             * held the file header.
             */
            p->buffer_alloc <<= 1;
            p->base.buffer = realloc(p->base.buffer, p->buffer_alloc);
            if (!p->base.buffer)
            {
                p->base.error_line = __LINE__;
                return RINEX_ERR_SYSTEM;
            }
            buffer = p->base.buffer + p->base.buffer_len;
        }

        /* Save satellite identifier. */
        *buffer++ = sv_name[0];
        *buffer++ = svn;

        /* Read each observation for this satellite. */
        for (jj = 0; jj < n_obs; ++jj)
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
            if (nn >= p->obs_alloc)
            {
                p->obs_alloc *= 2;
                p->base.lli = realloc(p->base.lli, p->obs_alloc);
                p->base.ssi = realloc(p->base.ssi, p->obs_alloc);
                p->base.obs = realloc(p->base.obs,
                    p->obs_alloc * sizeof p->base.obs[0]);
                if (!p->base.lli || !p->base.ssi || !p->base.obs)
                {
                    p->base.error_line = __LINE__;
                    return RINEX_ERR_SYSTEM;
                }
            }

            /* Copy the observation data. */
#if defined(__AVX2__)
            obs = rnx_buffer_and_parse_obs(obs, v_obs + (nn & 7));
            if ((nn & 7) == 7)
            {
                __m128i lli_ssi_01 = _mm_unpackhi_epi8(v_obs[0], v_obs[1]);
                __m128i lli_ssi_23 = _mm_unpackhi_epi8(v_obs[2], v_obs[3]);
                __m128i lli_ssi_45 = _mm_unpackhi_epi8(v_obs[4], v_obs[5]);
                __m128i lli_ssi_67 = _mm_unpackhi_epi8(v_obs[6], v_obs[7]);
                __m128i lli_ssi_03 = _mm_unpackhi_epi16(lli_ssi_01, lli_ssi_23);
                __m128i lli_ssi_47 = _mm_unpackhi_epi16(lli_ssi_45, lli_ssi_67);
                __m128i lli_ssi = _mm_unpackhi_epi32(lli_ssi_03, lli_ssi_47);
                _mm_storel_epi64((__m128i *)(p->base.lli + nn - 7), lli_ssi);
                _mm_storel_epi64((__m128i *)(p->base.ssi + nn - 7),
                    _mm_shuffle_epi32(lli_ssi, 0xB1));
                _mm256_storeu_si256((__m256i *)(p->base.obs + nn - 7),
                    rnx_parse_4(v_obs));
                _mm256_storeu_si256((__m256i *)(p->base.obs + nn - 3),
                    rnx_parse_4(v_obs + 4));
            }
#elif defined(__ARM_NEON)
            obs = rnx_parse_obs_neon(obs, p->base.obs + nn, &v_lli, &v_ssi);
            if (nn & 15 == 15)
            {
                vst1q_u8((uint8_t *)(p->base.lli + nn - 15), v_lli);
                vst1q_u8((uint8_t *)(p->base.ssi + nn - 15), v_ssi);
            }
#else
            obs = rnx_parse_obs(p, obs, nn);
#endif
            if (!obs)
            {
                p->base.error_line = __LINE__;
                return RINEX_ERR_BAD_FORMAT;
            }

            /* Remember that we saw this signal. */
            obs_mask |= 1 << (jj & 7);
            nn++;

            /* Update presence bitmasks. */
            if (((jj & 7) == 7) || ((jj + 1) == n_obs))
            {
                *buffer++ = obs_mask;
                obs_mask = 0;
            }
        }

        /* Finish writing presence bitmasks for a short line. */
        for (; jj < n_obs; jj += 8)
        {
            *buffer++ = obs_mask;
            obs_mask = 0;
        }

        /* Make sure buffer_len is updated. */
        p->base.buffer_len = buffer - p->base.buffer;

        if (*obs != '\n')
        {
            p->base.error_line = __LINE__;
            return RINEX_ERR_BAD_FORMAT;
        }
        obs++;
    }

#if defined(__AVX2__)
    if (nn & 7)
    {
        rnx_parse_final_avx2(v_obs, p->base.obs + (nn & ~7),
            p->base.lli + (nn & ~7), p->base.ssi + (nn & ~7), nn & 7);
    }
#elif defined(__ARM_NEON)
    if (nn & 15)
    {
        rnx_movemask_neon(p->base.lli + nn & ~15, nn & 15, v_lli);
        rnx_movemask_neon(p->base.ssi + nn & ~15, nn & 15, v_ssi);
    }
#endif

    return RINEX_SUCCESS;
}

/** rnx_read_v3 reads an observation data record from \a p_. */
static rinex_error_t rnx_read_v3(struct rinex_parser *p_)
{
    struct rnx_v23_parser *p = (struct rnx_v23_parser *)p_;
    const char *line;
    int64_t i64;
    int res, yy, mm, dd, hh, min, n_sats, line_len;

    /* Make sure we have an epoch to parse. */
    res = rnx_get_newlines(p_, &p->parse_ofs, NULL, 0, 1);
    if (res <= RINEX_EOF)
    {
        return res;
    }
    if (res < 35)
    {
        p_->error_line = __LINE__;
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
        p_->error_line = __LINE__;
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
            p_->error_line = __LINE__;
            return RINEX_ERR_BAD_FORMAT;
        }
    }
    else
    {
        p_->error_line = __LINE__;
        return RINEX_ERR_BAD_FORMAT;
    }

    /* Get enough data. */
    res = rnx_get_newlines(p_, &p->parse_ofs, NULL, 0, n_sats);
    if (res <= RINEX_EOF)
    {
        return res;
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
        return RINEX_SUCCESS;
    }

    p_->error_line = __LINE__;
    assert(0 && "logic failure in rnx_read_v3");
    return RINEX_ERR_BAD_FORMAT;
}

/** rnx_free_v23 deallocates \a p_, which must be a rnx_v23_parser. */
static void rnx_free_v23(struct rinex_parser *p_)
{
    struct rnx_v23_parser *p = (struct rnx_v23_parser *)p_;

    free(p->base.buffer);
    free(p->base.lli);
    free(p->base.ssi);
    free(p->base.obs);
    free(p);
}

/* Doc comment in rinex.h */
const char *rinex_find_header
(
    const struct rinex_parser *p,
    const char label[],
    unsigned int sizeof_label
)
{
    int ofs;

    ofs = rnx_find_header(p->buffer, p->buffer_len, label, sizeof_label);
    if (ofs < 0)
    {
        return NULL;
    }

    return p->buffer + ofs;
}

/** rnx_open_v2 reads the observation codes in \a p->base.header. */
static const char *rnx_open_v2(struct rnx_v23_parser *p)
{
    static const char n_obs[] = "# / TYPES OF OBSERV";
    const char *line;
    int res, value;
    char obs_type;

    /* What type of observations are in this file? */
    obs_type = p->base.buffer[40];
    if (!strchr(" GRSEM", obs_type))
    {
        return "Invalid satellite system for file";
    }

    /* Find the (first?) PRN / # OF OBS line. */
    line = rinex_find_header(&p->base, n_obs, sizeof n_obs);
    if (!line)
    {
        return "Could not find PRN / # OF OBS line";
    }
    res = parse_uint(&value, line, 6);
    if (res || (value < 1))
    {
        return "Invalid number of observations";
    }
    p->base.n_obs[' ' & 31] = value;
    if (obs_type == 'M')
    {
        p->base.n_obs['E' & 31] = value;
        p->base.n_obs['G' & 31] = value;
        p->base.n_obs['R' & 31] = value;
        p->base.n_obs['S' & 31] = value;
    }
    else
    {
        p->base.n_obs[obs_type & 31] = value;
        if (obs_type == ' ')
        {
            p->base.n_obs['G' & 31] = value;
        }
    }

    /* Initially assume 500 observations/epoch is enough space. */
    p->obs_alloc = 500;
    p->base.lli = calloc(p->obs_alloc, 1);
    p->base.ssi = calloc(p->obs_alloc, 1);
    p->base.obs = calloc(p->obs_alloc, sizeof p->base.obs[0]);
    if (!p->base.lli || !p->base.ssi || !p->base.obs)
    {
        return "Memory allocation failed";
    }

    return NULL;
}

/** rnx_open_v3 reads the observation codes in \a p->base.header. */
static const char *rnx_open_v3(struct rnx_v23_parser *p)
{
    static const char sys_n_obs[] = "SYS / # / OBS TYPES";
    const char *line;
    int res, ii, n_obs;
    char sys_id;

    /* Find the (first) SYS / # / OBS TYPES line. */
    line = rinex_find_header(&p->base, sys_n_obs, sizeof sys_n_obs);
    if (!line)
    {
        return "Could not find SYS / # / OBS TYPES line";
    }

    /* Keep going until we find a different header label. */
    while (!memcmp(line + 60, sys_n_obs, sizeof(sys_n_obs) - 1))
    {
        /* How many observations for this system? */
        sys_id = line[0];
        res = parse_uint(&n_obs, line + 3, 3);
        if (res || (n_obs < 1))
        {
            return "Invalid number of observations";
        }
        p->base.n_obs[sys_id & 31] = n_obs;

        /* Scan past following lines, 13 observation codes per line. */
        for (ii = 13; ii < n_obs; ii += 13)
        {
            line = strchr(line, '\n') + 1;
            if ((line[0] != ' ')
                || memcmp(line + 60, sys_n_obs, sizeof(sys_n_obs) - 1))
            {
                return "Expected a successor SYS / # / OBS TYPES line";
            }
        }

        /* Skip to the next line. */
        line = strchr(line, '\n') + 1;
    }

    /* Initially assume 500 observations is enough. */
    p->obs_alloc = 500;
    p->base.lli = calloc(p->obs_alloc, 1);
    p->base.ssi = calloc(p->obs_alloc, 1);
    p->base.obs = calloc(p->obs_alloc, sizeof p->base.obs[0]);
    if (!p->base.lli || !p->base.ssi || !p->base.obs)
    {
        return "Memory allocation failed";
    }

    return NULL;
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

    return jj;
}

/* Doc comment is in rinex.h. */
const char *rinex_open
(
    struct rinex_parser **p_parser,
    struct rinex_stream *stream
)
{
    static const char end_of_header[] = "END OF HEADER";
    struct rnx_v23_parser *p;
    const char *err;
    int res;

    if (*p_parser)
    {
        (*p_parser)->destroy(*p_parser);
        *p_parser = NULL;
    }

    res = stream->advance(stream, BLOCK_SIZE, 0);
    if (res || stream->size < 80)
    {
        return strerror(errno);
    }

    /* Is it an uncompressed RINEX file? */
    p = NULL;
    if (!memcmp("RINEX VERSION / TYPE", stream->buffer + 60, 20))
    {
        /* Check that it's an observation file. */
        if (stream->buffer[20] != 'O')
        {
            return "Not an observation RINEX file";
        }

        /* Check for END OF HEADER. */
        res = rnx_find_header(stream->buffer, stream->size, end_of_header,
            sizeof end_of_header);
        if (res < 1)
        {
            return "Could not find end of header";
        }
        res = strchr(stream->buffer + res, '\n') - stream->buffer + 1;

        /* Allocate the parser structure. */
        if (memcmp("     2.", stream->buffer, 7)
            && memcmp("     3.", stream->buffer, 7))
        {
            return "Unsupported RINEX version number";
        }
        p = calloc(1, sizeof(struct rnx_v23_parser));
        if (!p)
        {
            return "Memory allocation failed";
        }
        p->base.stream = stream;
        p->base.destroy = rnx_free_v23;

        /* Copy the header. */
        p->parse_ofs = res;
        p->buffer_alloc = res;
        p->base.buffer = calloc(p->buffer_alloc, 1);
        if (!p->base.buffer)
        {
            free(p);
            return "Memory allocation failed";
        }

        /* Copy the header for the caller's use. */
        res = rnx_copy_header(p->base.buffer, stream->buffer, res);
        if (res < 0)
        {
            free(p);
            return "Invalid header line detected";
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
        if (err != NULL)
        {
            rnx_free_v23(&p->base);
            return err;
        }

        *p_parser = &p->base;
        return NULL;
    }

    /* TODO: support files using Hatanaka compression:
    if (!memcmp("CRX VERS   / TYPE", stream->buffer + 60, 20)) { .. }
     */

    return "Unrecognized file format";
}
