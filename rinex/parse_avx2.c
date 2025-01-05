#include "rinex/rnx_priv.h"
#include <x86intrin.h>

static const char shuffle_8[31] __attribute__((aligned(16))) = {
    15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

static const char shuffle_16[32] __attribute__((aligned(16))) = {
    0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
};

static const char shuffle_32[80] __attribute__((aligned(16))) = {
    15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0,
    3, 2, 1, 0, 3, 2, 1, 0, 3, 2, 1, 0, 3, 2, 1, 0,
    7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12, 0, 0, 0, 0,
    0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3,
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
};

static void transpose_8_avx2(int64_t out[8], const char in[], int nbits)
    __attribute__((target("avx2")));
static void transpose_16_avx2(int64_t out[16], const char in[], int nbits)
    __attribute__((target("avx2")));
static void transpose_32_avx2(int64_t out[32], const char in[], int nbits)
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

/* Transpose a 16-by-nbits (each 16 bits adjacent) matrix to \a out[0..15].
 * The nbit-bit values are sign-extended.
 */
void transpose_16_avx2(int64_t out[16], const char in[], int nbits)
{
    int ii;

    /* AVX and SSE have nothing like _mm_movemask_epi16(), so we use a
     * different approach for transposing blocks of 16 numbers.
     *
     * This approach described in "Hacker's Delight" by Henry S. Warren, Jr.,
     * (section 7-3), based on an approach credited to Guy Steele.  We
     * need to byte-swap the input and output to make the 64-bit
     * arithmetic work properly for the SRNX bit ordering.
     */
    if (nbits <= 8)
    {
        /* 16*nbits <= 128, so we only need 128-bit ops initially... */
        const __m128i c1 = _mm_set1_epi64x(0xAA55AA55AA55AA55);
        const __m128i c2 = _mm_set1_epi64x(0x00AA00AA00AA00AA);
        const __m128i c3 = _mm_set1_epi64x(0xCCCC3333CCCC3333);
        const __m128i c4 = _mm_set1_epi64x(0x0000CCCC0000CCCC);
        const __m128i c5 = _mm_set1_epi64x(0xF0F0F0F00F0F0F0F);
        const __m128i c6 = _mm_set1_epi64x(0x00000000F0F0F0F0);
        const __m128i p1 = _mm_setr_epi8(14, 12, 10, 8, 6, 4, 2, 0,
            15, 13, 11, 9, 7, 5, 3, 1);
        const __m128i p2 = _mm_setr_epi8(7, 6, 5, 4, 3, 2, 1, 0,
            15, 14, 13, 12, 11, 10, 9, 8);
        const __m128i perm = _mm_loadu_si128((const __m128i *)(shuffle_16 + nbits * 2));

        __m128i v_in = _mm_loadu_si128((const __m128i *)in);
        v_in = _mm_shuffle_epi8(v_in, perm);
        v_in = _mm_shuffle_epi8(v_in, p1);

        const __m128i x1 = _mm_or_si128(_mm_and_si128(v_in, c1),
            _mm_or_si128(_mm_slli_epi64(_mm_and_si128(v_in, c2), 7),
            _mm_and_si128(_mm_srli_epi64(v_in, 7), c2)));
        const __m128i x2 = _mm_or_si128(_mm_and_si128(x1, c3),
            _mm_or_si128(_mm_slli_epi64(_mm_and_si128(x1, c4), 14),
            _mm_and_si128(_mm_srli_epi64(x1, 14), c4)));
        const __m128i x3 = _mm_or_si128(_mm_and_si128(x2, c5),
            _mm_or_si128(_mm_slli_epi64(_mm_and_si128(x2, c6), 28),
            _mm_and_si128(_mm_srli_epi64(x2, 28), c6)));
        const __m128i t1 = _mm_shuffle_epi8(x3, p2);

        /* ... but once we get to this point, AVX2 is easier. */
        _mm256_storeu_si256((__m256i *)out, _mm256_cvtepi8_epi64(t1));
        const __m128i t2 = _mm_shuffle_epi32(t1, 0x55);
        _mm256_storeu_si256((__m256i *)(out + 4), _mm256_cvtepi8_epi64(t2));
        const __m128i t3 = _mm_shuffle_epi32(t1, 0xAA);
        _mm256_storeu_si256((__m256i *)(out + 8), _mm256_cvtepi8_epi64(t3));
        const __m128i t4 = _mm_shuffle_epi32(t1, 0xFF);
        _mm256_storeu_si256((__m256i *)(out + 12), _mm256_cvtepi8_epi64(t4));

        return;
    }

    const __m256i p1 = _mm256_setr_epi8(
        14, 12, 10, 8, 6, 4, 2, 0, 15, 13, 11, 9, 7, 5, 3, 1,
        14, 12, 10, 8, 6, 4, 2, 0, 15, 13, 11, 9, 7, 5, 3, 1);
    if (nbits <= 16) /* fits within one AVX2 register */
    {
        const __m256i c1 = _mm256_set1_epi64x(0xAA55AA55AA55AA55);
        const __m256i c2 = _mm256_set1_epi64x(0x00AA00AA00AA00AA);
        const __m256i c3 = _mm256_set1_epi64x(0xCCCC3333CCCC3333);
        const __m256i c4 = _mm256_set1_epi64x(0x0000CCCC0000CCCC);
        const __m256i c5 = _mm256_set1_epi64x(0xF0F0F0F00F0F0F0F);
        const __m256i c6 = _mm256_set1_epi64x(0x00000000F0F0F0F0);
        const __m256i p2 = _mm256_setr_epi8(
            7, 15, 6, 14, 5, 13, 4, 12, 3, 11, 2, 10, 1, 9, 0, 8,
            7, 15, 6, 14, 5, 13, 4, 12, 3, 11, 2, 10, 1, 9, 0, 8);
        const __m256i perm = _mm256_loadu2_m128i(
            (const __m128i *)(shuffle_16 + nbits * 2 - 16),
            (const __m128i *)(shuffle_16 + 16));

        __m256i v_in = _mm256_loadu2_m128i(
            (const __m128i *)in,
            (const __m128i *)(in + nbits * 2 - 16));
        v_in = _mm256_shuffle_epi8(v_in, perm);
        v_in = _mm256_shuffle_epi8(v_in, p1);

        const __m256i x1 = _mm256_or_si256(_mm256_and_si256(v_in, c1),
            _mm256_or_si256(_mm256_slli_epi64(_mm256_and_si256(v_in, c2), 7),
            _mm256_and_si256(_mm256_srli_epi64(v_in, 7), c2)));
        const __m256i x2 = _mm256_or_si256(_mm256_and_si256(x1, c3),
            _mm256_or_si256(_mm256_slli_epi64(_mm256_and_si256(x1, c4), 14),
            _mm256_and_si256(_mm256_srli_epi64(x1, 14), c4)));
        const __m256i x3 = _mm256_or_si256(_mm256_and_si256(x2, c5),
            _mm256_or_si256(_mm256_slli_epi64(_mm256_and_si256(x2, c6), 28),
            _mm256_and_si256(_mm256_srli_epi64(x2, 28), c6)));
        const __m256i x4 = _mm256_permute4x64_epi64(x3, 0xd8);
        const __m256i t_w = _mm256_shuffle_epi8(x4, p2);

        const __m128i t1 = _mm256_castsi256_si128(t_w);
        _mm256_storeu_si256((__m256i *)out, _mm256_cvtepi16_epi64(t1));
        const __m128i t2 = _mm_shuffle_epi32(t1, 0xEE);
        _mm256_storeu_si256((__m256i *)(out + 4), _mm256_cvtepi16_epi64(t2));
        const __m128i t3 = _mm256_extracti128_si256(t_w, 1);
        _mm256_storeu_si256((__m256i *)(out + 8), _mm256_cvtepi16_epi64(t3));
        const __m128i t4 = _mm_shuffle_epi32(t3, 0xEE);
        _mm256_storeu_si256((__m256i *)(out + 12), _mm256_cvtepi16_epi64(t4));

        return;
    }

    /* needs two AVX2 registers; how to get the MSBs depends on nbits,
     * but it's always a win to use _mm256_movemask_epi8().
     *
     * FIXME: This ends up being slightly slower than transpose_32_avx2(..., nbits).
     */
    __m256i perm, v_hi;
    if (nbits <= 24)
    {
        perm = _mm256_loadu2_m128i(
            (const __m128i *)(shuffle_16 + nbits * 2 - 32),
            (const __m128i *)shuffle_16);
        v_hi = _mm256_broadcastsi128_si256(
            _mm_loadu_si128((const __m128i *)in));
    }
    else /* 24 < nbits <= 32 */
    {
        perm = _mm256_loadu2_m128i(
            (const __m128i *)(shuffle_16 + 16),
            (const __m128i *)(shuffle_16 + nbits * 2 - 48));
        v_hi = _mm256_loadu2_m128i((const __m128i *)(in + nbits * 2 - 48),
            (const __m128i *)in);
    }
    const __m256i v_lo = _mm256_lddqu_si256((const __m256i *)(in + nbits * 2 - 32));
    const __m256i v_th = _mm256_shuffle_epi8(v_hi, perm);
    const __m256i v_pl = _mm256_shuffle_epi8(v_lo, p1);
    const __m256i v_ph = _mm256_shuffle_epi8(v_th, p1);
    const __m256i u_lo = _mm256_unpacklo_epi64(v_ph, v_pl);
    const __m256i u_hi = _mm256_unpackhi_epi64(v_ph, v_pl);
    __m256i u_pl = _mm256_permute4x64_epi64(u_lo, 0x27);
    __m256i u_ph = _mm256_permute4x64_epi64(u_hi, 0x27);
    for (ii = 0; ii < 8; ++ii)
    {
        out[ii+0] = _mm256_movemask_epi8(u_pl);
        out[ii+8] = _mm256_movemask_epi8(u_ph);
        u_pl = _mm256_slli_epi64(u_pl, 1);
        u_ph = _mm256_slli_epi64(u_ph, 1);
    }
}

/* Transpose a 32-by-nbits (each 32 bits adjacent) matrix to \a out[0..31].
 * The nbit-bit values are sign-extended.
 */
void transpose_32_avx2(int64_t out[32], const char in[], int nbits)
{
    int ii;

    /* The "128-bit lane" nature of AVX really hurts here.  Only 4 rows
     * of 32 bits fit into a 128-bit lane, so the branching goes down to
     * nbits/4.  nbits < 9 looks mostly like transpose_8_avx2(), and
     * nbits > 8 looks more like transpose_16_avx2().
     */

    if (nbits < 1)
    {
        /* noop */
    }
    else if (nbits < 17)
    {
        /* FIXME: These are slower than 16 < nbits <= 24, or even <= 32. */
        if (nbits < 9)
        {
            /* Up to 8 bits looks like transpose_8_avx2(). */
            if (nbits < 5) /* 1 to 4 bits: one SSE register */
            {
                __m128i perm = _mm_loadu_si128((const __m128i *)(shuffle_32 + 16 - nbits * 4));
                __m128i v_in = _mm_loadu_si128((const __m128i *)in);
                v_in = _mm_shuffle_epi8(v_in, perm);
                for (ii = 0; ii < 32; ++ii)
                {
                    out[ii] = _mm_movemask_epi32(v_in) << 28 >> 28;
                    v_in = _mm_slli_epi64(v_in, 1);
                }
            }
            else /* 5 to 8 bits: one AVX register */
            {
                __m256i perm = _mm256_loadu2_m128i(
                    (const __m128i *)(shuffle_32 + 32 - nbits * 4),
                    (const __m128i *)shuffle_32);
                __m256i v_in = _mm256_loadu2_m128i(
                    (const __m128i *)in,
                    (const __m128i *)(in + nbits * 4 - 16));
                v_in = _mm256_shuffle_epi8(v_in, perm);
                for (ii = 0; ii < 32; ++ii)
                {
                    out[ii] = (char)_mm256_movemask_epi32(v_in);
                    v_in = _mm256_slli_epi64(v_in, 1);
                }
            }
        }
        else /* 8 < nbits < 17 */
        {
            /* nbits > 8 looks more like transpose_16_avx2() */
            const __m256i c1 = _mm256_set1_epi64x(0xAA55AA55AA55AA55);
            const __m256i c2 = _mm256_set1_epi64x(0x00AA00AA00AA00AA);
            const __m256i c3 = _mm256_set1_epi64x(0xCCCC3333CCCC3333);
            const __m256i c4 = _mm256_set1_epi64x(0x0000CCCC0000CCCC);
            const __m256i c5 = _mm256_set1_epi64x(0xF0F0F0F00F0F0F0F);
            const __m256i c6 = _mm256_set1_epi64x(0x00000000F0F0F0F0);

            /* 9 to 16 bits: up to two AVX registers; we save no
             * logic by trying to use one SSE and one AVX, so just
             * deinterleave into two YMMs and then expand the results.
             */
            __m256i perm, v_hi, v_lo;
            if (nbits < 13)
            {
                perm = _mm256_loadu2_m128i(
                    (const __m128i *)(shuffle_32 + nbits * 4 + 16),
                    (const __m128i *)(shuffle_32 + 48));
                v_hi = _mm256_broadcastsi128_si256(
                    _mm_loadu_si128((const __m128i *)in));
            }
            else /* 12 < nbits < 17 */
            {
                perm = _mm256_loadu2_m128i(
                    (const __m128i *)(shuffle_32 + 64),
                    (const __m128i *)(shuffle_32 + nbits * 4));
                v_hi = _mm256_loadu2_m128i(
                    (const __m128i *)(in + nbits * 4 - 48),
                    (const __m128i *)in);
            }
            v_lo = _mm256_lddqu_si256((const __m256i *)(in + nbits * 4 - 32));
            v_hi = _mm256_shuffle_epi8(v_hi, perm);

            const __m256i p1 = _mm256_setr_epi8(
                12, 8, 4, 0, 13, 9, 5, 1, 14, 10, 6, 2, 15, 11, 7, 3,
                12, 8, 4, 0, 13, 9, 5, 1, 14, 10, 6, 2, 15, 11, 7, 3);
            v_hi = _mm256_shuffle_epi8(v_hi, p1);
            v_lo = _mm256_shuffle_epi8(v_lo, p1);

            const __m256i p2 = _mm256_setr_epi32(4, 0, 5, 1, 6, 2, 7, 3);
            v_hi = _mm256_permutevar8x32_epi32(v_hi, p2);
            v_lo = _mm256_permutevar8x32_epi32(v_lo, p2);

            const __m256i x1_hi = _mm256_or_si256(_mm256_and_si256(v_hi, c1),
                _mm256_or_si256(_mm256_slli_epi64(_mm256_and_si256(v_hi, c2), 7),
                _mm256_and_si256(_mm256_srli_epi64(v_hi, 7), c2)));
            const __m256i x1_lo = _mm256_or_si256(_mm256_and_si256(v_lo, c1),
                _mm256_or_si256(_mm256_slli_epi64(_mm256_and_si256(v_lo, c2), 7),
                _mm256_and_si256(_mm256_srli_epi64(v_lo, 7), c2)));

            const __m256i x2_hi = _mm256_or_si256(_mm256_and_si256(x1_hi, c3),
                _mm256_or_si256(_mm256_slli_epi64(_mm256_and_si256(x1_hi, c4), 14),
                _mm256_and_si256(_mm256_srli_epi64(x1_hi, 14), c4)));
            const __m256i x2_lo = _mm256_or_si256(_mm256_and_si256(x1_lo, c3),
                _mm256_or_si256(_mm256_slli_epi64(_mm256_and_si256(x1_lo, c4), 14),
                _mm256_and_si256(_mm256_srli_epi64(x1_lo, 14), c4)));

            const __m256i x3_hi = _mm256_or_si256(_mm256_and_si256(x2_hi, c5),
                _mm256_or_si256(_mm256_slli_epi64(_mm256_and_si256(x2_hi, c6), 28),
                _mm256_and_si256(_mm256_srli_epi64(x2_hi, 28), c6)));
            const __m256i x3_lo = _mm256_or_si256(_mm256_and_si256(x2_lo, c5),
                _mm256_or_si256(_mm256_slli_epi64(_mm256_and_si256(x2_lo, c6), 28),
                _mm256_and_si256(_mm256_srli_epi64(x2_lo, 28), c6)));

            const __m256i p3 = _mm256_setr_epi8(
                7, 6, 5, 4, 3, 2, 1, 0, 15, 14, 13, 12, 11, 10, 9, 8,
                7, 6, 5, 4, 3, 2, 1, 0, 15, 14, 13, 12, 11, 10, 9, 8);
            const __m256i x4_hi = _mm256_shuffle_epi8(x3_hi, p3);
            const __m256i x4_lo = _mm256_shuffle_epi8(x3_lo, p3);

            const __m256i x5_hi = _mm256_unpacklo_epi8(x4_lo, x4_hi);
            const __m256i x5_lo = _mm256_unpackhi_epi8(x4_lo, x4_hi);

            const __m128i t1 = _mm256_castsi256_si128(x5_hi);
            _mm256_storeu_si256((__m256i *)out, _mm256_cvtepi16_epi64(t1));
            const __m128i t2 = _mm_shuffle_epi32(t1, 0xEE);
            _mm256_storeu_si256((__m256i *)(out + 4), _mm256_cvtepi16_epi64(t2));
            const __m128i t3 = _mm256_castsi256_si128(x5_lo);
            _mm256_storeu_si256((__m256i *)(out + 8), _mm256_cvtepi16_epi64(t3));
            const __m128i t4 = _mm_shuffle_epi32(t3, 0xEE);
            _mm256_storeu_si256((__m256i *)(out + 12), _mm256_cvtepi16_epi64(t4));
            const __m128i t5 = _mm256_extracti128_si256(x5_hi, 1);
            _mm256_storeu_si256((__m256i *)(out + 16), _mm256_cvtepi16_epi64(t5));
            const __m128i t6 = _mm_shuffle_epi32(t5, 0xEE);
            _mm256_storeu_si256((__m256i *)(out + 20), _mm256_cvtepi16_epi64(t6));
            const __m128i t7 = _mm256_extracti128_si256(x5_lo, 1);
            _mm256_storeu_si256((__m256i *)(out + 24), _mm256_cvtepi16_epi64(t7));
            const __m128i t8 = _mm_shuffle_epi32(t7, 0xEE);
            _mm256_storeu_si256((__m256i *)(out + 28), _mm256_cvtepi16_epi64(t8));
        }
    }
    else /* 16 < nbits <= 32 */
    {
        /* 17 to 32 bits: up to four AVX registers; it is hard to
         * deinterleave correctly with three, and that would add more
         * total instructions, so always extend to a 32x32 transpose.
         */
        __m256i perm, v_0, v_1;
        if (nbits < 25)
        {
            v_0 = _mm256_broadcastsi128_si256(_mm_loadu_si128((const __m128i *)in));
            if (nbits < 21)
            {
                /* 17 to 20 bits: first 128 bits repeat in both */
                v_1 = v_0;
                perm = _mm256_loadu2_m128i(
                    (const __m128i *)(shuffle_32 + nbits * 4 - 16),
                    (const __m128i *)(shuffle_32 + 48));
            }
            else /* 20 < nbits < 25 */
            {
                /* 21 to 24 bits: fourth 128 bits from different spot */
                v_1 = _mm256_inserti128_si256(v_0,
                    _mm_loadu_si128((const __m128i *)(in + nbits * 4 - 80)), 1);
                perm = _mm256_loadu2_m128i(
                    (const __m128i *)(shuffle_32 + 64),
                    (const __m128i *)(shuffle_32 + nbits * 4 - 32));
            }
            v_1 = _mm256_shuffle_epi8(v_1, perm);
            perm = _mm256_broadcastsi128_si256(_mm_loadu_si128(
                (const __m128i *)(shuffle_32 + 48)));
        }
        else
        {
            if (nbits < 29)
            {
                v_0 = _mm256_broadcastsi128_si256(_mm_loadu_si128((const __m128i *)in));
                perm = _mm256_loadu2_m128i(
                    (const __m128i *)(shuffle_32 + nbits * 4 - 48),
                    (const __m128i *)(shuffle_32 + 48));
            }
            else /* 28 < nbits < 33 */
            {
                v_0 = _mm256_loadu2_m128i((const __m128i *)(in + nbits * 4 - 112),
                    (const __m128i *)in);
                perm = _mm256_loadu2_m128i(
                    (const __m128i *)(shuffle_32 + 64),
                    (const __m128i *)(shuffle_32 + nbits * 4 - 64));
            }
            v_1 = _mm256_lddqu_si256((const __m256i *)(in + nbits * 4 - 96));
        }
        v_0 = _mm256_shuffle_epi8(v_0, perm);
        __m256i v_2 = _mm256_lddqu_si256((const __m256i *)(in + nbits * 4 - 64));
        __m256i v_3 = _mm256_lddqu_si256((const __m256i *)(in + nbits * 4 - 32));

        perm = _mm256_setr_epi8(
            12, 8, 4, 0, 13, 9, 5, 1, 14, 10, 6, 2, 15, 11, 7, 3,
            12, 8, 4, 0, 13, 9, 5, 1, 14, 10, 6, 2, 15, 11, 7, 3);
        v_0 = _mm256_shuffle_epi8(v_0, perm);
        v_1 = _mm256_shuffle_epi8(v_1, perm);
        v_2 = _mm256_shuffle_epi8(v_2, perm);
        v_3 = _mm256_shuffle_epi8(v_3, perm);

        perm = _mm256_setr_epi32(4, 0, 5, 1, 6, 2, 7, 3);
        v_0 = _mm256_permutevar8x32_epi32(v_0, perm);
        v_1 = _mm256_permutevar8x32_epi32(v_1, perm);
        v_2 = _mm256_permutevar8x32_epi32(v_2, perm);
        v_3 = _mm256_permutevar8x32_epi32(v_3, perm);

        const __m256i c1 = _mm256_set1_epi64x(0xAA55AA55AA55AA55);
        const __m256i c2 = _mm256_set1_epi64x(0x00AA00AA00AA00AA);
        const __m256i x1_0 = _mm256_or_si256(_mm256_and_si256(v_0, c1),
            _mm256_or_si256(_mm256_slli_epi64(_mm256_and_si256(v_0, c2), 7),
            _mm256_and_si256(_mm256_srli_epi64(v_0, 7), c2)));
        const __m256i x1_1 = _mm256_or_si256(_mm256_and_si256(v_1, c1),
            _mm256_or_si256(_mm256_slli_epi64(_mm256_and_si256(v_1, c2), 7),
            _mm256_and_si256(_mm256_srli_epi64(v_1, 7), c2)));
        const __m256i x1_2 = _mm256_or_si256(_mm256_and_si256(v_2, c1),
            _mm256_or_si256(_mm256_slli_epi64(_mm256_and_si256(v_2, c2), 7),
            _mm256_and_si256(_mm256_srli_epi64(v_2, 7), c2)));
        const __m256i x1_3 = _mm256_or_si256(_mm256_and_si256(v_3, c1),
            _mm256_or_si256(_mm256_slli_epi64(_mm256_and_si256(v_3, c2), 7),
            _mm256_and_si256(_mm256_srli_epi64(v_3, 7), c2)));

        const __m256i c3 = _mm256_set1_epi64x(0xCCCC3333CCCC3333);
        const __m256i c4 = _mm256_set1_epi64x(0x0000CCCC0000CCCC);
        const __m256i x2_0 = _mm256_or_si256(_mm256_and_si256(x1_0, c3),
            _mm256_or_si256(_mm256_slli_epi64(_mm256_and_si256(x1_0, c4), 14),
            _mm256_and_si256(_mm256_srli_epi64(x1_0, 14), c4)));
        const __m256i x2_1 = _mm256_or_si256(_mm256_and_si256(x1_1, c3),
            _mm256_or_si256(_mm256_slli_epi64(_mm256_and_si256(x1_1, c4), 14),
            _mm256_and_si256(_mm256_srli_epi64(x1_1, 14), c4)));
        const __m256i x2_2 = _mm256_or_si256(_mm256_and_si256(x1_2, c3),
            _mm256_or_si256(_mm256_slli_epi64(_mm256_and_si256(x1_2, c4), 14),
            _mm256_and_si256(_mm256_srli_epi64(x1_2, 14), c4)));
        const __m256i x2_3 = _mm256_or_si256(_mm256_and_si256(x1_3, c3),
            _mm256_or_si256(_mm256_slli_epi64(_mm256_and_si256(x1_3, c4), 14),
            _mm256_and_si256(_mm256_srli_epi64(x1_3, 14), c4)));

        const __m256i c5 = _mm256_set1_epi64x(0xF0F0F0F00F0F0F0F);
        const __m256i c6 = _mm256_set1_epi64x(0x00000000F0F0F0F0);
        const __m256i x3_0 = _mm256_or_si256(_mm256_and_si256(x2_0, c5),
            _mm256_or_si256(_mm256_slli_epi64(_mm256_and_si256(x2_0, c6), 28),
            _mm256_and_si256(_mm256_srli_epi64(x2_0, 28), c6)));
        const __m256i x3_1 = _mm256_or_si256(_mm256_and_si256(x2_1, c5),
            _mm256_or_si256(_mm256_slli_epi64(_mm256_and_si256(x2_1, c6), 28),
            _mm256_and_si256(_mm256_srli_epi64(x2_1, 28), c6)));
        const __m256i x3_2 = _mm256_or_si256(_mm256_and_si256(x2_2, c5),
            _mm256_or_si256(_mm256_slli_epi64(_mm256_and_si256(x2_2, c6), 28),
            _mm256_and_si256(_mm256_srli_epi64(x2_2, 28), c6)));
        const __m256i x3_3 = _mm256_or_si256(_mm256_and_si256(x2_3, c5),
            _mm256_or_si256(_mm256_slli_epi64(_mm256_and_si256(x2_3, c6), 28),
            _mm256_and_si256(_mm256_srli_epi64(x2_3, 28), c6)));

        perm = _mm256_setr_epi8(
            7, 6, 5, 4, 3, 2, 1, 0, 15, 14, 13, 12, 11, 10, 9, 8,
            7, 6, 5, 4, 3, 2, 1, 0, 15, 14, 13, 12, 11, 10, 9, 8);
        const __m256i x4_0 = _mm256_shuffle_epi8(x3_0, perm);
        const __m256i x4_1 = _mm256_shuffle_epi8(x3_1, perm);
        const __m256i x4_2 = _mm256_shuffle_epi8(x3_2, perm);
        const __m256i x4_3 = _mm256_shuffle_epi8(x3_3, perm);

        const __m256i x5_0 = _mm256_unpacklo_epi8(x4_1, x4_0);
        const __m256i x5_1 = _mm256_unpackhi_epi8(x4_1, x4_0);
        const __m256i x5_2 = _mm256_unpacklo_epi8(x4_3, x4_2);
        const __m256i x5_3 = _mm256_unpackhi_epi8(x4_3, x4_2);

        const __m256i x6_0 = _mm256_unpacklo_epi16(x5_2, x5_0);
        const __m256i x6_1 = _mm256_unpackhi_epi16(x5_2, x5_0);
        const __m256i x6_2 = _mm256_unpacklo_epi16(x5_3, x5_1);
        const __m256i x6_3 = _mm256_unpackhi_epi16(x5_3, x5_1);

        const __m128i t1 = _mm256_castsi256_si128(x6_0);
        _mm256_storeu_si256((__m256i *)(out + 0), _mm256_cvtepi32_epi64(t1));
        const __m128i t2 = _mm256_castsi256_si128(x6_1);
        _mm256_storeu_si256((__m256i *)(out + 4), _mm256_cvtepi32_epi64(t2));
        const __m128i t3 = _mm256_castsi256_si128(x6_2);
        _mm256_storeu_si256((__m256i *)(out + 8), _mm256_cvtepi32_epi64(t3));
        const __m128i t4 = _mm256_castsi256_si128(x6_3);
        _mm256_storeu_si256((__m256i *)(out + 12), _mm256_cvtepi32_epi64(t4));
        const __m128i t5 = _mm256_extracti128_si256(x6_0, 1);
        _mm256_storeu_si256((__m256i *)(out + 16), _mm256_cvtepi32_epi64(t5));
        const __m128i t6 = _mm256_extracti128_si256(x6_1, 1);
        _mm256_storeu_si256((__m256i *)(out + 20), _mm256_cvtepi32_epi64(t6));
        const __m128i t7 = _mm256_extracti128_si256(x6_2, 1);
        _mm256_storeu_si256((__m256i *)(out + 24), _mm256_cvtepi32_epi64(t7));
        const __m128i t8 = _mm256_extracti128_si256(x6_3, 1);
        _mm256_storeu_si256((__m256i *)(out + 28), _mm256_cvtepi32_epi64(t8));
    }
}

void rnx_avx2_transpose(int64_t *out, const char *in, int bits, int count)
{
    switch (count)
    {
    case 8:  transpose_8_avx2 (out, in, bits); break;
    case 16: transpose_16_avx2(out, in, bits); break;
    case 32: transpose_32_avx2(out, in, bits); break;
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
