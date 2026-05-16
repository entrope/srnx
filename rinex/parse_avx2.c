#include "rinex/rnx_priv.h"
#include <x86intrin.h>

static const char shuffle_8[31] __attribute__((aligned(16))) = {
    15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

static void transpose_8_avx2(int64_t out[8], const char in[], int nbits)
    __attribute__((target("avx2")));

/* SSE and AVX don't have native forms of these instructions, because
 * the bit-cast between int and float is supposedly a noop.
 */

static inline int _mm_movemask_epi32(__m128i x)
    __attribute__((target("avx2")));
static inline int _mm_movemask_epi32(__m128i x)
{
    return _mm_movemask_ps(_mm_castsi128_ps(x));
}

static inline int _mm256_movemask_epi32(__m256i x)
    __attribute__((target("avx2")));
static inline int _mm256_movemask_epi32(__m256i x)
{
    return _mm256_movemask_ps(_mm256_castsi256_ps(x));
}

/* Transpose an 8-by-nbits (each 8 bits adjacent) matrix to \a out[0..7].
 * The nbit-bit values are sign-extended.
 */
void transpose_8_avx2(int64_t out[8], const char in[], int nbits)
{
    int ii;

    /* Hat tip to Mischa Sandberg (mischasan) for these inner loops. */
    if (nbits <= 16)
    {
        /* Use only 128-bit operations for power efficiency. */
        __m128i perm = _mm_loadu_si128((const __m128i *)(shuffle_8 + 16 - nbits));
        __m128i v_in = _mm_loadu_si128((const __m128i *)in);
        v_in = _mm_shuffle_epi8(v_in, perm);
        for (ii = 0; ii < 8; ++ii)
        {
            out[ii] = (int16_t)_mm_movemask_epi8(v_in);
            v_in = _mm_slli_epi64(v_in, 1);
        }
    }
    else /* nbits <= 32, use an AVX2 register */
    {
        __m256i perm = _mm256_loadu2_m128i(
            (const __m128i *)(shuffle_8 + 32 - nbits),
            (const __m128i *)shuffle_8);
        __m256i v_in = _mm256_loadu2_m128i((const __m128i *)in,
            (const __m128i *)(in + nbits - 16));
        v_in = _mm256_shuffle_epi8(v_in, perm);
        for (ii = 0; ii < 8; ++ii)
        {
            out[ii] = _mm256_movemask_epi8(v_in);
            v_in = _mm256_slli_epi64(v_in, 1);
        }
    }
}

/** Decode a row-major K-rows × (count/8)-bytes bit matrix as count/8
 * consecutive 8-wide transpositions, one per column group of 8 values.
 */
void rnx_avx2_transpose(int64_t *out, const char *in, int bits, int count)
{
    int n_groups = count >> 3;
    int gg, rr;
    char tmp[32] = {0};         /* bits <= 32; zero-pad for safe AVX2 loads */

    for (gg = 0; gg < n_groups; ++gg)
    {
        for (rr = 0; rr < bits; ++rr)
            tmp[rr] = in[gg + rr * n_groups];
        transpose_8_avx2(out + gg * 8, tmp, bits);
    }
}

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
        mask = _mm256_blend_epi32(mask, v_ones, 0x30);
    }
    if (neg_0 & 65535)
    {
        mask = _mm256_blend_epi32(mask, v_ones, 0x03);
    }
    int neg_1 = _mm256_movemask_epi8(_mm256_cmpeq_epi8(v_minus, p_1));
    if (neg_1 >> 16)
    {
        mask = _mm256_blend_epi32(mask, v_ones, 0xc0);
    }
    if (neg_1 & 65535)
    {
        mask = _mm256_blend_epi32(mask, v_ones, 0x0c);
    }
    const __m256i t8 = _mm256_sub_epi64(v_zero, t7);
    const __m256i t9 = _mm256_blendv_epi8(t7, t8, mask);
    return t9;
}

static inline int rnx_avx2_parse_obs
(
    struct rinex_parser *p,
    const char *obs,
    __m128i v_obs[],
    const int idx[],
    int kk
)
{
    const __m128i v_nl = _mm_set1_epi8('\n');
    const __m128i v_sp = _mm_set1_epi8(' ');
    __m128i v_obs_2 = _mm_loadu_si128((const __m128i *)obs);
    __m128i m_nl = _mm_cmpeq_epi8(v_obs_2, v_nl);
    const int mask = _mm_movemask_epi8(m_nl);
    const int cnt = __builtin_ctz(mask | 0x10000);
    __m128i m_nl_1 = _mm_or_si128(m_nl,   _mm_bslli_si128(m_nl, 1));
    __m128i m_nl_2 = _mm_or_si128(m_nl_1, _mm_bslli_si128(m_nl_1, 2));
    __m128i m_nl_3 = _mm_or_si128(m_nl_2, _mm_bslli_si128(m_nl_2, 4));
    __m128i m_sp   = _mm_or_si128(m_nl_3, _mm_bslli_si128(m_nl_3, 8));
    v_obs[kk] = _mm_blendv_epi8(v_obs_2, v_sp, m_sp);
    if (kk == 7)
    {
        __m128i lli_ssi_01 = _mm_unpackhi_epi8(v_obs[0], v_obs[1]);
        __m128i lli_ssi_23 = _mm_unpackhi_epi8(v_obs[2], v_obs[3]);
        __m128i lli_ssi_45 = _mm_unpackhi_epi8(v_obs[4], v_obs[5]);
        __m128i lli_ssi_67 = _mm_unpackhi_epi8(v_obs[6], v_obs[7]);
        __m128i lli_ssi_03 = _mm_unpackhi_epi16(lli_ssi_01, lli_ssi_23);
        __m128i lli_ssi_47 = _mm_unpackhi_epi16(lli_ssi_45, lli_ssi_67);
        __m128i lli_ssi = _mm_unpackhi_epi32(lli_ssi_03, lli_ssi_47);
        __m256i obs_03 = rnx_parse_4(v_obs);
        __m256i obs_47 = rnx_parse_4(v_obs + 4);
        p->lli[idx[0]] = _mm_extract_epi8(lli_ssi, 0);
        p->lli[idx[1]] = _mm_extract_epi8(lli_ssi, 1);
        p->lli[idx[2]] = _mm_extract_epi8(lli_ssi, 2);
        p->lli[idx[3]] = _mm_extract_epi8(lli_ssi, 3);
        p->lli[idx[4]] = _mm_extract_epi8(lli_ssi, 4);
        p->lli[idx[5]] = _mm_extract_epi8(lli_ssi, 5);
        p->lli[idx[6]] = _mm_extract_epi8(lli_ssi, 6);
        p->lli[idx[7]] = _mm_extract_epi8(lli_ssi, 7);
        p->ssi[idx[0]] = _mm_extract_epi8(lli_ssi,  8);
        p->ssi[idx[1]] = _mm_extract_epi8(lli_ssi,  9);
        p->ssi[idx[2]] = _mm_extract_epi8(lli_ssi, 10);
        p->ssi[idx[3]] = _mm_extract_epi8(lli_ssi, 11);
        p->ssi[idx[4]] = _mm_extract_epi8(lli_ssi, 12);
        p->ssi[idx[5]] = _mm_extract_epi8(lli_ssi, 13);
        p->ssi[idx[6]] = _mm_extract_epi8(lli_ssi, 14);
        p->ssi[idx[7]] = _mm_extract_epi8(lli_ssi, 15);
        /* Because of AVX's "lane" arrangement, the indexes are
         * a little mixed up in the middle of each register.
         */
        p->obs[idx[0]] = _mm256_extract_epi64(obs_03, 0);
        p->obs[idx[2]] = _mm256_extract_epi64(obs_03, 1);
        p->obs[idx[1]] = _mm256_extract_epi64(obs_03, 2);
        p->obs[idx[3]] = _mm256_extract_epi64(obs_03, 3);
        p->obs[idx[4]] = _mm256_extract_epi64(obs_47, 0);
        p->obs[idx[6]] = _mm256_extract_epi64(obs_47, 1);
        p->obs[idx[5]] = _mm256_extract_epi64(obs_47, 2);
        p->obs[idx[7]] = _mm256_extract_epi64(obs_47, 3);
    }
    return cnt;
}

static inline void rnx_avx2_parse_outro(
    struct rinex_parser *p,
    __m128i v_obs[],
    const int idx[],
    int kk
)
{
    __m256i res_lo = rnx_parse_4(v_obs + 0);
    __m256i res_hi = rnx_parse_4(v_obs + 4);

    switch (kk)
    {
    case 7:
        /* Indexing is wonky because of AVX's "lane" arrangement. */
        p->lli[idx[6]] = _mm_extract_epi8(v_obs[6], 14);
        p->ssi[idx[6]] = _mm_extract_epi8(v_obs[6], 15);
        p->obs[idx[5]] = _mm256_extract_epi64(res_hi, 2);
        /* fall through */
    case 6:
        p->lli[idx[5]] = _mm_extract_epi8(v_obs[5], 14);
        p->ssi[idx[5]] = _mm_extract_epi8(v_obs[5], 15);
        p->obs[idx[6]] = _mm256_extract_epi64(res_hi, 1);
        /* fall through */
    case 5:
        p->lli[idx[4]] = _mm_extract_epi8(v_obs[4], 14);
        p->ssi[idx[4]] = _mm_extract_epi8(v_obs[4], 15);
        p->obs[idx[4]] = _mm256_extract_epi64(res_hi, 0);
        /* fall through */
    case 4:
        p->lli[idx[3]] = _mm_extract_epi8(v_obs[3], 14);
        p->ssi[idx[3]] = _mm_extract_epi8(v_obs[3], 15);
        p->obs[idx[3]] = _mm256_extract_epi64(res_lo, 3);
        /* fall through */
    case 3:
        p->lli[idx[2]] = _mm_extract_epi8(v_obs[2], 14);
        p->ssi[idx[2]] = _mm_extract_epi8(v_obs[2], 15);
        p->obs[idx[1]] = _mm256_extract_epi64(res_lo, 2);
        /* fall through */
    case 2:
        p->lli[idx[1]] = _mm_extract_epi8(v_obs[1], 14);
        p->ssi[idx[1]] = _mm_extract_epi8(v_obs[1], 15);
        p->obs[idx[2]] = _mm256_extract_epi64(res_lo, 1);
        /* fall through */
    case 1:
        p->lli[idx[0]] = _mm_extract_epi8(v_obs[0], 14);
        p->ssi[idx[0]] = _mm_extract_epi8(v_obs[0], 15);
        p->obs[idx[0]] = _mm256_extract_epi64(res_lo, 0);
    }
}

#define SIMD_PARSE_INTRO \
    __m128i v_obs[8]; \
    int idx[8]; \
    int kk = 0;

#define SIMD_PARSE_OBS \
    idx[kk] = nn; \
    obs += rnx_avx2_parse_obs(&p->base, obs, v_obs, idx, kk); \
    kk = (kk + 1) & 7

#define SIMD_PARSE_OUTRO \
    if (kk) rnx_avx2_parse_outro(&p->base, v_obs, idx, kk);

static int rnx_avx2_get_n_newlines_inner(
    const struct rinex_parser *p,
    uint64_t *p_whence,
    int n_lines,
    int *found
)
{
    const __m256i v_nl = _mm256_broadcastb_epi8(_mm_set1_epi8('\n'));
    const char * restrict buffer = p->stream->buffer;
    uint64_t whence = *p_whence;

    for (; whence + 64 < p->stream->size; whence += 64)
    {
        const __m256i v_p_2 = _mm256_loadu_si256((__m256i const *)(buffer + whence + 32));
        const __m256i m_nl_2 = _mm256_cmpeq_epi8(v_nl, v_p_2);
        const __m256i v_p = _mm256_loadu_si256((__m256i const *)(buffer + whence));
        const __m256i m_nl = _mm256_cmpeq_epi8(v_nl, v_p);
        uint64_t kk = ((uint64_t)_mm256_movemask_epi8(m_nl_2) << 32)
            | (uint32_t)_mm256_movemask_epi8(m_nl);

        const int nn = __builtin_popcountll(kk);
        if (*found + nn < n_lines)
        {
            *found += nn;
            continue;
        }

        while (1)
        {
            int r = __builtin_ctzll(kk);
            kk &= (kk - 1);
            if (++*found == n_lines)
            {
                *p_whence = whence + r + 1;
                return 1;
            }
        }
    }

    *p_whence = whence;
    return 0;
}

#define SIMD_GET_N_NEWLINES \
    if (rnx_avx2_get_n_newlines_inner(p, &whence, n_lines, &found)) return whence

#define SIMD_TYPE avx2
#include "rinex/parse_simd.ii"
