/** rnx2srnx.c - Convert RINEX files to Succinct RINEX format.
 * Copyright 2021 Michael Poole.
 * SPDX-License-Identifier: MIT-Modern-Variant
 *
 * Built twice from this one source file (see CMakeLists.txt):
 *
 *   rnx2srnx        - the real converter.  Writes an actual .srnx file.
 *   rnx2srnx_search - built with -DRNX2SRNX_SEARCH.  A clone that amortizes
 *     RINEX parsing across multiple candidate block-size tuples.  For each
 *     input file and each requested tuple, reports the total encoded byte
 *     count to stdout.  No SRNX bytes are written; only the byte cost of an
 *     encoding under the given tuple is computed.  The DP that picks block
 *     boundaries is parameterized over a runtime tuple of block sizes
 *     (each a positive multiple of 8, up to --max-block) instead of the
 *     rnx2srnx build's fixed spec table.  The transpose step is skipped
 *     entirely: we only need byte counts, not decodable output.
 *     Output: one line per (file, tuple), tab-separated:
 *         <file>\t<tuple>\t<bytes>
 *     where <tuple> is dash-separated, e.g. "8-16-32-64-128".
 *     With --soc2, observation chunks are costed as SOC2 (cross-signal
 *     prediction, SSI references, split-Rice blocks) instead of SOCD.
 *
 * The two builds share the LEB128 helpers, the delta-matrix computation,
 * and the block-choice types; everything else (in particular the DP core,
 * sparse table, and scratch-buffer types) is duplicated under the
 * RNX2SRNX_SEARCH guard below because the two variants need incompatible
 * candidate-list representations (rnx2srnx uses the fixed spec table from
 * rnx_priv.h; rnx2srnx_search uses a runtime tuple bounded by --max-block).
 */

#include "rinex/digest.h"
#include "rinex/rinex_load.h"
#include "rinex/rnx_priv.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#ifdef __ARM_NEON
#include <arm_neon.h>
#elif defined(__AVX2__)
#include <immintrin.h>
#endif
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ---- Configuration globals (shared) ---- */

/* Digest ids match between builds so the regression check (spec tuple
 * + spec defaults, in rnx2srnx_search) reproduces an unmodified rnx2srnx
 * run's file size. */
static int g_chunk_digest_id = 2;    /* CRC32C */
static int g_file_digest_id  = 5;    /* BLAKE3-256 */

/* Upper bound on SLEB128-run length considered by dp_core (--max-sleb-run). */
static int g_max_sleb_run = 16;

/* ---- LEB128 length helpers (shared) ---- */

static int uleb128_len(uint64_t val)
{
    int n = 1;
    while (val >= 128) { val >>= 7; n++; }
    return n;
}

static int sleb128_len(int64_t val)
{
    uint64_t uv = ((uint64_t)val << 1) ^ (uint64_t)(val >> 63);
    return uleb128_len(uv);
}

/* SLEB128 byte-length from two's-complement bit-width.
 *
 * For any val with twos_comp_bw(val) == bw, sleb128_len(val) is constant.
 * Derived as ceil(bw / 7), with bw == 0 mapped to 1 (unused sentinel).
 * Covers bw 0..64; bw > 64 is clamped to 255 before use so this never
 * goes out of bounds. */
static const uint8_t sleb128_len_from_bw[65] =
{
    [0]   = 1,
    [1 ... 7]   = 1,
    [8 ... 14]  = 2,
    [15 ... 21] = 3,
    [22 ... 28] = 4,
    [29 ... 35] = 5,
    [36 ... 42] = 6,
    [43 ... 49] = 7,
    [50 ... 56] = 8,
    [57 ... 63] = 9,
    [64]        = 10,
};

/* ---- Zigzag bit-width (shared) ---- */

/** Return minimum bits for two's-complement representation of \a val. */
static uint8_t twos_comp_bw(int64_t val)
{
    uint64_t v = (val >= 0) ? (uint64_t)val : (uint64_t)(~val);
    return v == 0 ? 1 : (uint8_t)(65 - __builtin_clzll(v));
}

/* ---- GCD computation (shared) ---- */

static int64_t gcd(int64_t a, int64_t b)
{
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    while (b) { int64_t t = b; b = a % b; a = t; }
    return a;
}

/* ---- Delta matrix computation (shared) ---- */

/** Compute 8 * count delta values into \a d (caller-owned, 8 * count elements).
 * d[ord * count + i] = ord-th order difference at index i,
 * where order 0 is the raw scaled values (with d[ord][-1] = 0 implied).
 */
static void compute_delta_matrix(int64_t *d, const int64_t *scaled, int count)
{
    int ord, ii;

    for (ii = 0; ii < count; ++ii)
        d[ii] = scaled[ii];

    for (ord = 1; ord <= 7; ++ord)
    {
        const int64_t *prev = d + (ord - 1) * count;
        int64_t *curr = d + ord * count;
        curr[0] = prev[0];
        for (ii = 1; ii < count; ++ii)
            curr[ii] = prev[ii] - prev[ii - 1];
    }
}

/* ---- Block-choice types (shared) ----
 *
 * The greedy "lowest cost-per-value" selector is suboptimal whenever a
 * candidate block size's max_bw exceeds the max_bw of just the leading
 * half — most visibly at m=128 on noisy (e.g. 30 s-decimated) data, where
 * a single high-magnitude delta forces 64 unrelated values to pay the
 * outlier's bit-width.  We replace it with a 1-D dynamic program: each
 * position is a node, each candidate block (plus zero-run and SLEB128
 * fallback) is an edge, and we walk dp[count..0] choosing the edge that
 * minimises total bytes.
 */

enum block_kind
{
    BK_BLOCK,   /**< m × bw transposed bit matrix */
    BK_ZERO,    /**< zero-run (>= 16 zeros) */
    BK_SLEB,    /**< SLEB128 run (1..16 values) */
    BK_RICE     /**< SOC2 split-Rice block; bw holds the Rice parameter */
};

struct block_choice
{
    short kind;     /**< enum block_kind */
    short bw;       /**< BK_BLOCK: 1..32; BK_RICE: Rice parameter 0..31 */
    int   len;      /**< values consumed by this edge */
};

/* ---- Indicator alphabet index (shared) ---- */

/* Returns the alphabet index of ch, or -1 if not in alphabet. */
static int indicator_idx(unsigned char ch)
{
    if (ch == ' ') return 0;
    if (ch >= '0' && ch <= '9') return 1 + (ch - '0');
    return -1;
}

/* ---- SRNX header payload size (shared) ---- */

#define SRNX_PAYLOAD_SIZE 16

/* ---- Epoch span helper (shared) ---- */

/** Check if two epochs are in the same span (same interval, no minute
 * rollover boundary).
 */
static int same_span(const struct rinex_epoch *prev,
    const struct rinex_epoch *cur, int64_t interval_e7)
{
    int64_t expected_sec;

    if (cur->yyyy_mm_dd != prev->yyyy_mm_dd)
        return 0;
    expected_sec = prev->sec_e7 + interval_e7;
    if (expected_sec >= 600000000 && prev->sec_e7 < 600000000)
    {
        /* Minute rollover expected. */
        expected_sec -= 600000000;
        if (cur->hh_mm != prev->hh_mm + 1
            && !(prev->hh_mm % 100 == 59
                 && cur->hh_mm == prev->hh_mm + 41))
            return 0;
        return cur->sec_e7 == expected_sec;
    }
    if (cur->hh_mm != prev->hh_mm)
        return 0;
    return cur->sec_e7 == expected_sec;
}

/* ---- SDIR entry (shared) ---- */

struct sdir_entry
{
    char name[4];
    size_t sate_offset;
};

/* ---- Digest flag parser (shared) ---- */

static int parse_digest_flag(const char *arg, const char *prefix, int *out)
{
    size_t plen = strlen(prefix);
    const char *val;
    char *endp;
    long id;

    if (strncmp(arg, prefix, plen) != 0)
        return 0;
    val = arg + plen;
    id = strtol(val, &endp, 10);
    if (*val == '\0' || *endp != '\0' || id < 0 || id > INT_MAX)
    {
        fprintf(stderr, "Invalid digest id in '%s'\n", arg);
        exit(EXIT_FAILURE);
    }
    if (rnx_digest_length((int)id) < 0)
    {
        fprintf(stderr, "Unsupported digest id %ld\n", id);
        exit(EXIT_FAILURE);
    }
    *out = (int)id;
    return 1;
}

/* ---- SLEB128-run search (shared) ---- */

/** Best SLEB128-run edge starting at \a ii.
 *
 * Minimises `2 + (psum[ii + L] - psum[ii]) + dp[ii + L]` over lengths
 * L in [1, max_L], storing the best L in \a *out_L.  Costs fit in int32
 * (dp < 30 MB; psum delta <= 160 bytes for 16 vals).  Processes 16-wide
 * SIMD chunks updating the running argmin; a scalar tail (starting at
 * chunk_start) handles any remainder. */
static inline int64_t sleb_run_best(const int64_t *dp, const uint32_t *psum,
    int ii, int max_L, int *out_L)
{
    uint32_t pii = psum[ii];
    int64_t sleb_min = INT64_MAX;
    int sleb_min_L = 0;
    int chunk_start = 1, L;
#if defined(__ARM_NEON)
    for (; chunk_start + 15 <= max_L; chunk_start += 16)
    {
        const int64_t *dpv = dp + ii + chunk_start;
        int32x4_t dp0 = vcombine_s32(vmovn_s64(vld1q_s64(dpv+ 0)), vmovn_s64(vld1q_s64(dpv+ 2)));
        int32x4_t dp1 = vcombine_s32(vmovn_s64(vld1q_s64(dpv+ 4)), vmovn_s64(vld1q_s64(dpv+ 6)));
        int32x4_t dp2 = vcombine_s32(vmovn_s64(vld1q_s64(dpv+ 8)), vmovn_s64(vld1q_s64(dpv+10)));
        int32x4_t dp3 = vcombine_s32(vmovn_s64(vld1q_s64(dpv+12)), vmovn_s64(vld1q_s64(dpv+14)));
        uint32x4_t pii_v = vdupq_n_u32(pii);
        const uint32_t *psv = psum + ii + chunk_start;
        int32x4_t d0 = vreinterpretq_s32_u32(vsubq_u32(vld1q_u32(psv+ 0), pii_v));
        int32x4_t d1 = vreinterpretq_s32_u32(vsubq_u32(vld1q_u32(psv+ 4), pii_v));
        int32x4_t d2 = vreinterpretq_s32_u32(vsubq_u32(vld1q_u32(psv+ 8), pii_v));
        int32x4_t d3 = vreinterpretq_s32_u32(vsubq_u32(vld1q_u32(psv+12), pii_v));
        int32x4_t two = vdupq_n_s32(2);
        int32x4_t c0 = vaddq_s32(vaddq_s32(d0, dp0), two);
        int32x4_t c1 = vaddq_s32(vaddq_s32(d1, dp1), two);
        int32x4_t c2 = vaddq_s32(vaddq_s32(d2, dp2), two);
        int32x4_t c3 = vaddq_s32(vaddq_s32(d3, dp3), two);
        int b = chunk_start;
        int32x4_t i0 = (int32x4_t){b+ 0, b+ 1, b+ 2, b+ 3};
        int32x4_t i1 = (int32x4_t){b+ 4, b+ 5, b+ 6, b+ 7};
        int32x4_t i2 = (int32x4_t){b+ 8, b+ 9, b+10, b+11};
        int32x4_t i3 = (int32x4_t){b+12, b+13, b+14, b+15};
        uint32x4_t gt;
        gt = vcgtq_s32(c0, c1); c0 = vbslq_s32(gt, c1, c0); i0 = vbslq_s32(gt, i1, i0);
        gt = vcgtq_s32(c2, c3); c2 = vbslq_s32(gt, c3, c2); i2 = vbslq_s32(gt, i3, i2);
        gt = vcgtq_s32(c0, c2); c0 = vbslq_s32(gt, c2, c0); i0 = vbslq_s32(gt, i2, i0);
        int32x2_t c_lo = vget_low_s32(c0), c_hi = vget_high_s32(c0);
        int32x2_t i_lo = vget_low_s32(i0), i_hi = vget_high_s32(i0);
        uint32x2_t gt2 = vcgt_s32(c_lo, c_hi);
        c_lo = vbsl_s32(gt2, c_hi, c_lo);
        i_lo = vbsl_s32(gt2, i_hi, i_lo);
        int32_t ca = vget_lane_s32(c_lo, 0), ia = vget_lane_s32(i_lo, 0);
        int32_t cb = vget_lane_s32(c_lo, 1), ib = vget_lane_s32(i_lo, 1);
        int32_t chunk_min = (ca <= cb) ? ca : cb;
        int chunk_min_L = (ca <= cb) ? ia : ib;
        if (chunk_min < sleb_min) { sleb_min = chunk_min; sleb_min_L = chunk_min_L; }
    }
#elif defined(__AVX2__)
    for (; chunk_start + 15 <= max_L; chunk_start += 16)
    {
        const int64_t *dpv = dp + ii + chunk_start;
        __m128i n03 = _mm_unpacklo_epi64(
            _mm_shuffle_epi32(_mm_loadu_si128((__m128i*)(dpv+ 0)), 0x08),
            _mm_shuffle_epi32(_mm_loadu_si128((__m128i*)(dpv+ 2)), 0x08));
        __m128i n47 = _mm_unpacklo_epi64(
            _mm_shuffle_epi32(_mm_loadu_si128((__m128i*)(dpv+ 4)), 0x08),
            _mm_shuffle_epi32(_mm_loadu_si128((__m128i*)(dpv+ 6)), 0x08));
        __m128i n8b = _mm_unpacklo_epi64(
            _mm_shuffle_epi32(_mm_loadu_si128((__m128i*)(dpv+ 8)), 0x08),
            _mm_shuffle_epi32(_mm_loadu_si128((__m128i*)(dpv+10)), 0x08));
        __m128i ncf = _mm_unpacklo_epi64(
            _mm_shuffle_epi32(_mm_loadu_si128((__m128i*)(dpv+12)), 0x08),
            _mm_shuffle_epi32(_mm_loadu_si128((__m128i*)(dpv+14)), 0x08));
        __m256i dp0 = _mm256_set_m128i(n47, n03);
        __m256i dp1 = _mm256_set_m128i(ncf, n8b);
        __m256i pii_v = _mm256_set1_epi32((int)pii);
        const uint32_t *psv = psum + ii + chunk_start;
        __m256i d0 = _mm256_sub_epi32(_mm256_loadu_si256((__m256i*)(psv+ 0)), pii_v);
        __m256i d1 = _mm256_sub_epi32(_mm256_loadu_si256((__m256i*)(psv+ 8)), pii_v);
        __m256i two = _mm256_set1_epi32(2);
        __m256i c0 = _mm256_add_epi32(_mm256_add_epi32(d0, dp0), two);
        __m256i c1 = _mm256_add_epi32(_mm256_add_epi32(d1, dp1), two);
        int b = chunk_start;
        __m256i i0 = _mm256_set_epi32(b+7, b+6, b+5, b+4, b+3, b+2, b+1, b+0);
        __m256i i1 = _mm256_set_epi32(b+15, b+14, b+13, b+12, b+11, b+10, b+9, b+8);
        __m256i gt = _mm256_cmpgt_epi32(c0, c1);
        __m256i cv = _mm256_blendv_epi8(c0, c1, gt);
        __m256i iv = _mm256_blendv_epi8(i0, i1, gt);
        __m128i cv_lo = _mm256_castsi256_si128(cv);
        __m128i cv_hi = _mm256_extracti128_si256(cv, 1);
        __m128i iv_lo = _mm256_castsi256_si128(iv);
        __m128i iv_hi = _mm256_extracti128_si256(iv, 1);
        __m128i gt2 = _mm_cmpgt_epi32(cv_lo, cv_hi);
        cv_lo = _mm_blendv_epi8(cv_lo, cv_hi, gt2);
        iv_lo = _mm_blendv_epi8(iv_lo, iv_hi, gt2);
        __m128i cv_r = _mm_shuffle_epi32(cv_lo, 0x4E);
        __m128i iv_r = _mm_shuffle_epi32(iv_lo, 0x4E);
        __m128i gt3 = _mm_cmpgt_epi32(cv_lo, cv_r);
        cv_lo = _mm_blendv_epi8(cv_lo, cv_r, gt3);
        iv_lo = _mm_blendv_epi8(iv_lo, iv_r, gt3);
        cv_r = _mm_shuffle_epi32(cv_lo, 0xB1);
        iv_r = _mm_shuffle_epi32(iv_lo, 0xB1);
        __m128i gt4 = _mm_cmpgt_epi32(cv_lo, cv_r);
        cv_lo = _mm_blendv_epi8(cv_lo, cv_r, gt4);
        iv_lo = _mm_blendv_epi8(iv_lo, iv_r, gt4);
        int32_t chunk_min = _mm_cvtsi128_si32(cv_lo);
        int chunk_min_L = _mm_cvtsi128_si32(iv_lo);
        if (chunk_min < sleb_min) { sleb_min = chunk_min; sleb_min_L = chunk_min_L; }
    }
#endif /* __ARM_NEON || __AVX2__ */
    for (L = chunk_start; L <= max_L; ++L)
    {
        int64_t c = 2 + (int64_t)(psum[ii + L] - pii) + dp[ii + L];
        if (c < sleb_min) { sleb_min = c; sleb_min_L = L; }
    }
    *out_L = sleb_min_L;
    return sleb_min;
}

/* ---- Split-Rice block DP (shared; SOC2) ---- */

/* Quotients above this are capped when summing (fewer for very long
 * sequences, so 32-bit sums cannot overflow).  A 4096-bit quotient costs
 * far more than coding its value any other way, so capping does not
 * change the DP's choices in practice. */
#define RICE_Q_CAP 4095u

/* Zigzag values above this are capped when estimating a block's mean. */
#define RICE_U_CAP ((uint64_t)1 << 40)

/** Scratch arrays for rice_dp(), each with room for at least count + 1
 * entries; rice_q has (RNX_RICE_MAX_R + 1) * (count + 1) entries. */
struct rice_scratch
{
    int64_t  *dp;
    uint32_t *psum;
    uint64_t *usum;
    uint32_t *rice_q;
};

/** SOC2 split-Rice DP: the same shortest-path search as the SOCD DP, but
 * each candidate block of m values (m from \a cand) is a split-Rice block
 * costing 1 + r*m/8 + ceil(sum(q + 1) / 8) bytes for its best Rice
 * parameter r.  Zero-run and SLEB128-run edges are as in SOCD.
 *
 * The best r lies in [0, bitlen(mean u)], searched downward: below the
 * top, a block's bit count is at least 8 + r*m + (sum u >> r), which grows
 * as r shrinks, so the search stops once that bound cannot beat the best
 * block so far.  If \a choices is non-NULL it receives the optimal edge
 * at each position.  Returns the minimal byte count. */
static int64_t rice_dp(const struct rice_scratch *rs, const int64_t *vals,
    int count, const int *cand, int n_cand, struct block_choice *choices)
{
    uint32_t *rice_q = rs->rice_q;
    uint64_t *usum = rs->usum;
    uint32_t *psum = rs->psum;
    int64_t *dp = rs->dp;
    uint64_t u_max = 0;
    uint32_t q_cap;
    int ii, rr, ci, zlen_next, r_max, width, r_start;

    /* The search never exceeds r = bitlen(mean u) <= bitlen(max u), so
     * only those prefix sums are needed.  They are stored position-major:
     * rice_q[ii * width + rr] = sum over the first ii values of q + 1 for
     * Rice parameter rr, so one position's sums share cache lines. */
    for (ii = 0; ii < count; ++ii)
    {
        uint64_t uu = ((uint64_t)vals[ii] << 1) ^ (uint64_t)(vals[ii] >> 63);
        if (uu > u_max)
            u_max = uu;
    }
    if (u_max > RICE_U_CAP)
        u_max = RICE_U_CAP;
    r_max = u_max ? 64 - __builtin_clzll(u_max) : 1;
    if (r_max > RNX_RICE_MAX_R)
        r_max = RNX_RICE_MAX_R;
    width = r_max + 1;
    q_cap = UINT32_MAX / (uint32_t)(count + 1) - 1;
    if (q_cap > RICE_Q_CAP)
        q_cap = RICE_Q_CAP;

    usum[0] = 0;
    psum[0] = 0;
    for (rr = 0; rr < width; ++rr)
        rice_q[rr] = 0;
    for (ii = 0; ii < count; ++ii)
    {
        uint64_t uu = ((uint64_t)vals[ii] << 1) ^ (uint64_t)(vals[ii] >> 63);
        uint8_t bw = twos_comp_bw(vals[ii]);
        const uint32_t *prev = rice_q + (size_t)ii * width;
        uint32_t *next = rice_q + (size_t)(ii + 1) * width;
        usum[ii + 1] = usum[ii] + (uu < RICE_U_CAP ? uu : RICE_U_CAP);
        psum[ii + 1] = psum[ii] + sleb128_len_from_bw[bw > 64 ? 64 : bw];
        for (rr = 0; rr < width; ++rr)
        {
            uint64_t qq = uu >> rr;
            next[rr] = prev[rr] + (qq < q_cap ? (uint32_t)qq : q_cap) + 1;
        }
    }

    dp[count] = 0;
    zlen_next = 0;
    for (ii = count - 1; ii >= 0; --ii)
    {
        int remaining = count - ii, max_L, sleb_L, zlen;
        int64_t best = INT64_MAX, cost;
        struct block_choice best_choice = {BK_SLEB, 0, 1};

        zlen = (vals[ii] == 0) ? zlen_next + 1 : 0;
        zlen_next = zlen;
        if (zlen > 0)
        {
            cost = 1 + uleb128_len((uint64_t)(zlen - 1)) + dp[ii + zlen];
            if (cost < best)
            {
                best = cost;
                best_choice.kind = BK_ZERO;
                best_choice.bw = 0;
                best_choice.len = zlen;
            }
        }

        for (ci = 0, r_start = -1; ci < n_cand; ++ci)
        {
            int m = cand[ci], r_hi, best_r;
            uint64_t usum_m, mean, best_bits, best_block;
            const uint32_t *qa, *qb;

            if (remaining < m)
                break;
            usum_m = usum[ii + m] - usum[ii];
            mean = usum_m / (uint64_t)m;
            r_hi = mean ? 64 - __builtin_clzll(mean) : 1;
            if (r_hi > r_max)
                r_hi = r_max;

            /* A block's bit count, r*m + sum(q + 1), is convex in r, and
             * its byte count is that rounded up, so walk downhill over
             * [0, r_hi] from the previous block size's best r. */
            qa = rice_q + (size_t)ii * width;
            qb = rice_q + (size_t)(ii + m) * width;
            best_r = r_start < 0 ? r_hi : r_start > r_hi ? r_hi : r_start;
            best_bits = (uint64_t)best_r * m + (qb[best_r] - qa[best_r]);
            while (best_r > 0)
            {
                uint64_t bits = (uint64_t)(best_r - 1) * m
                    + (qb[best_r - 1] - qa[best_r - 1]);
                if (bits >= best_bits)
                    break;
                best_bits = bits;
                best_r--;
            }
            if (best_r == (r_start < 0 ? r_hi : r_start > r_hi ? r_hi : r_start))
            {
                while (best_r < r_hi)
                {
                    uint64_t bits = (uint64_t)(best_r + 1) * m
                        + (qb[best_r + 1] - qa[best_r + 1]);
                    if (bits >= best_bits)
                        break;
                    best_bits = bits;
                    best_r++;
                }
            }
            /* Another parameter may tie in bytes; any is equally good. */
            best_block = 1 + (best_bits + 7) / 8;
            r_start = best_r;
            cost = (int64_t)best_block + dp[ii + m];
            if (cost < best)
            {
                best = cost;
                best_choice.kind = BK_RICE;
                best_choice.bw = (short)best_r;
                best_choice.len = m;
            }
        }

        max_L = remaining < g_max_sleb_run ? remaining : g_max_sleb_run;
        cost = sleb_run_best(dp, psum, ii, max_L, &sleb_L);
        if (cost < best)
        {
            best = cost;
            best_choice.kind = BK_SLEB;
            best_choice.bw = 0;
            best_choice.len = sleb_L;
        }

        dp[ii] = best;
        if (choices)
            choices[ii] = best_choice;
    }

    return dp[0];
}

/* ---- SOC2 delta-order search (shared) ---- */

/* How SOC2 coding chooses a delta order (--order-search=).  ORDER_SCAN
 * evaluates orders 0, 1, 2, ... until two consecutive orders fail to
 * improve; ORDER_GUESS starts at the order with the smallest residual
 * bit-length sum and moves to neighbouring orders while that improves. */
enum order_search { ORDER_SCAN, ORDER_GUESS };
static int g_order_search = ORDER_GUESS;

/** Parses an --order-search= option; returns 1 if \a arg was one. */
static int parse_order_search(const char *arg)
{
    if (strncmp(arg, "--order-search=", 15))
        return 0;
    if (!strcmp(arg + 15, "scan"))
        g_order_search = ORDER_SCAN;
    else if (!strcmp(arg + 15, "guess"))
        g_order_search = ORDER_GUESS;
    else
    {
        fprintf(stderr, "Invalid --order-search value '%s' (scan or guess)\n", arg + 15);
        exit(EXIT_FAILURE);
    }
    return 1;
}

/** Cheap estimate of the best delta order for the \a count values whose
 * delta matrix (stride \a count) is \a delta_matrix: the order minimizing
 * the sum of the residuals' ZigZag bit lengths plus its initial state. */
static int rice_order_guess(const int64_t *delta_matrix, int count)
{
    uint64_t best_bits = UINT64_MAX;
    int ord, best = 0;

    for (ord = 0; ord <= 7 && ord < count; ++ord)
    {
        const int64_t *vals = delta_matrix + (size_t)ord * count;
        uint64_t bits = 8 * (uint64_t)ord;
        int ii;
        for (ii = 0; ii < count; ++ii)
        {
            uint64_t uu = ((uint64_t)vals[ii] << 1) ^ (uint64_t)(vals[ii] >> 63);
            bits += uu ? 64 - __builtin_clzll(uu) : 0;
        }
        if (bits < best_bits)
        {
            best_bits = bits;
            best = ord;
        }
    }
    return best;
}

/** Chooses a delta order for \a count values with delta matrix
 * \a delta_matrix, per g_order_search.  \a eval(ctx, ord) returns the
 * byte cost of coding with delta order \a ord, excluding its initial
 * state.  If \a accept is not NULL, it is called right after each
 * evaluation that becomes the best so far.  Stores the chosen order's
 * cost (excluding initial state) in \a *out_cost and returns the order. */
static int rice_select_order(const int64_t *delta_matrix, int count,
    int64_t (*eval)(void *ctx, int ord), void (*accept)(void *ctx),
    void *ctx, int64_t *out_cost)
{
    int64_t best_total, best_cost;
    int best_order, ord, dir, no_improve;

    if (g_order_search == ORDER_SCAN)
    {
        best_order = 0;
        best_cost = best_total = eval(ctx, 0);
        if (accept)
            accept(ctx);
        no_improve = 0;
        for (ord = 1; ord <= 7 && ord < count; ++ord)
        {
            int64_t cost = eval(ctx, ord);
            if (ord + cost < best_total)
            {
                best_total = ord + cost;
                best_cost = cost;
                best_order = ord;
                no_improve = 0;
                if (accept)
                    accept(ctx);
            }
            else if (++no_improve >= 2)
                break;
        }
    }
    else
    {
        int guess = rice_order_guess(delta_matrix, count);
        best_order = guess;
        best_cost = eval(ctx, guess);
        best_total = guess + best_cost;
        if (accept)
            accept(ctx);
        for (dir = -1; dir <= 1; dir += 2)
        {
            for (ord = guess + dir; ord >= 0 && ord <= 7 && ord < count; ord += dir)
            {
                int64_t cost = eval(ctx, ord);
                if (ord + cost >= best_total)
                    break;
                best_total = ord + cost;
                best_cost = cost;
                best_order = ord;
                if (accept)
                    accept(ctx);
            }
        }
    }

    *out_cost = best_cost;
    return best_order;
}

/* ---- SOC2 predictor planning (shared) ----
 *
 * Following the SOC2 encoder guidance in Succinct_RINEX.md: each
 * satellite's reference carrier phase (the one present at the most
 * epochs) is coded without prediction; other carrier phases are
 * predicted from it, and pseudoranges and Doppler from the carrier phase
 * on their own band (or the reference).  Whether a predictor pays off is
 * decided per signal by comparing coded sizes.
 */

/* Markers in a SOC2 value sequence. */
#define SOC2_ABSENT INT64_MIN        /* no observation at this satellite epoch */
#define SOC2_UNPRED (INT64_MIN + 1)  /* observation in an unpredicted-values block */

/* Doppler predictor max_span: 300 seconds, in units of 1e-7 s. */
#define SOC2_DOPPLER_MAX_SPAN 3000000000ULL

/* glo_chan[] entry for a GLONASS slot without a known frequency channel. */
#define GLO_CHAN_UNKNOWN 127

struct soc2_pred
{
    int      kind;     /* SRNX_PRED_NONE, SRNX_PRED_VALUE or SRNX_PRED_RATE */
    int      ref;      /* reference observation index */
    int64_t  num;
    uint64_t den;
    int      stencil;  /* rate: 0 backward, 1 central, 2 forward */
    uint64_t max_span; /* rate: 1e-7 s units, 0 = unlimited */
};

/* The plan for one signal of a satellite. */
struct soc2_plan
{
    struct soc2_pred pred; /* predictor to try; kind 0 if none applies */
    int s_ref;             /* same-band S observation for SSIs, or -1 */
};

/** Carrier frequency in Hz of band \a band of system \a sys, or 0 if
 * unknown.  \a glo_chan is the GLONASS FDMA frequency channel. */
static int64_t soc2_freq_hz(char sys, char band, int glo_chan)
{
    switch (sys)
    {
    case 'G': case 'J': case 'S': case 'I':
        if (band == '1') return 1575420000;
        if (band == '2' && (sys == 'G' || sys == 'J')) return 1227600000;
        if (band == '5') return 1176450000;
        if (band == '6' && sys == 'J') return 1278750000;
        if (band == '9' && sys == 'I') return 2492028000;
        return 0;
    case 'E':
        switch (band)
        {
        case '1': return 1575420000;
        case '5': return 1176450000;
        case '6': return 1278750000;
        case '7': return 1207140000;
        case '8': return 1191795000;
        }
        return 0;
    case 'C':
        switch (band)
        {
        case '1': return 1575420000;
        case '2': return 1561098000;
        case '5': return 1176450000;
        case '6': return 1268520000;
        case '7': return 1207140000;
        case '8': return 1191795000;
        }
        return 0;
    case 'R':
        switch (band)
        {
        case '1': return 1602000000 + (int64_t)glo_chan * 562500;
        case '2': return 1246000000 + (int64_t)glo_chan * 437500;
        case '3': return 1202025000;
        case '4': return 1600995000;
        case '6': return 1248060000;
        }
        return 0;
    }
    return 0;
}

/** Byte count of a ULEB128-length-prefixed RLE of \a sym[] over an
 * alphabet of \a n_sym symbols, omitting trailing runs of symbol 0
 * (space), as SOCD and SOC2 indicator RLEs do. */
static size_t rle_symbols_bytes(const unsigned char *sym, int count, int n_sym)
{
    size_t rle_used = 0;
    int ii = 0;

    while (count > 0 && sym[count - 1] == 0)
        count--;
    while (ii < count)
    {
        int run_start = ii;
        while (ii < count && sym[ii] == sym[run_start])
            ii++;
        rle_used += (size_t)uleb128_len((uint64_t)(ii - run_start - 1) * n_sym
            + sym[run_start]);
    }
    return (size_t)uleb128_len((uint64_t)rle_used) + rle_used;
}

/** Fill \a sym[] with the SOC2 SSI symbols of \a ssi[]: the indicator's
 * alphabet index, or 11 where it equals the SSI predicted from
 * \a svals[] (NULL for no SSI reference).  Returns the index of the first
 * SSI outside the alphabet (mapped to symbol 0), or -1 if none. */
static int soc2_ssi_symbols(unsigned char *sym, const char *ssi,
    const int64_t *svals, int count)
{
    int kk, bad = -1;
    for (kk = 0; kk < count; ++kk)
    {
        int idx = indicator_idx((unsigned char)ssi[kk]);
        if (idx < 0)
        {
            if (bad < 0)
                bad = kk;
            idx = 0;
        }
        if (svals && svals[kk] != INT64_MIN
            && ssi[kk] == rnx_ssi_from_strength(svals[kk]))
            idx = 11;
        sym[kk] = (unsigned char)idx;
    }
    return bad;
}

/** Bytes of the predictor fields (the kind itself is in the mode value). */
static size_t soc2_pred_bytes(const struct soc2_pred *pr)
{
    size_t nn = 0;
    if (pr->kind == SRNX_PRED_NONE)
        return nn;
    nn += (size_t)uleb128_len((uint64_t)pr->ref);
    nn += (size_t)sleb128_len(pr->num);
    nn += (size_t)uleb128_len(pr->den);
    if (pr->kind == SRNX_PRED_RATE)
    {
        nn += (size_t)uleb128_len((uint64_t)pr->stencil);
        nn += (size_t)uleb128_len(pr->max_span);
    }
    return nn;
}

/** Set \a pr's multiplier to num / den in lowest terms. */
static void soc2_set_ratio(struct soc2_pred *pr, int64_t num, int64_t den)
{
    int64_t div = gcd(num, den);
    if (div > 1)
    {
        num /= div;
        den /= div;
    }
    pr->num = num;
    pr->den = (uint64_t)den;
}

/** Parse GLONASS SLOT / FRQ # records from the RINEX header \a hdr into
 * \a glo_chan[slot], leaving GLO_CHAN_UNKNOWN for unlisted slots. */
static void soc2_parse_glo_chan(const char *hdr, int len, signed char glo_chan[100])
{
    const char *end = hdr + len;
    const char *line;

    memset(glo_chan, GLO_CHAN_UNKNOWN, 100);
    for (line = hdr; line < end; )
    {
        const char *eol = memchr(line, '\n', (size_t)(end - line));
        size_t line_len = eol ? (size_t)(eol - line) : (size_t)(end - line);
        if (line_len >= 80 && !memcmp(line + 60, "GLONASS SLOT / FRQ #", 20))
        {
            int ii;
            for (ii = 0; ii + 3 <= 60; ++ii)
            {
                if (line[ii] == 'R' && isdigit((unsigned char)line[ii + 1])
                    && isdigit((unsigned char)line[ii + 2]))
                {
                    char field[8];
                    int slot = (line[ii + 1] - '0') * 10 + (line[ii + 2] - '0');
                    long chan;
                    char *endp;
                    memcpy(field, line + ii + 3, 4);
                    field[4] = '\0';
                    chan = strtol(field, &endp, 10);
                    if (endp != field && chan >= -7 && chan <= 13)
                        glo_chan[slot] = (signed char)chan;
                    ii += 6;
                }
            }
        }
        line += line_len + 1;
    }
}

/** Estimate a GLONASS FDMA channel as the one making the band-1
 * code-minus-carrier sequence smoothest (least total |second difference|). */
static int soc2_estimate_glo_chan(const int64_t *code, const int64_t *phase,
    int count)
{
    int best_chan = 0, chan, kk;
    double best_sum = -1;

    for (chan = -7; chan <= 6; ++chan)
    {
        int64_t freq = soc2_freq_hz('R', '1', chan);
        int64_t prev1 = INT64_MIN, prev2 = INT64_MIN;
        double sum = 0;
        for (kk = 0; kk < count; ++kk)
        {
            int64_t cmc;
            if (code[kk] == INT64_MIN || phase[kk] == INT64_MIN)
            {
                prev1 = prev2 = INT64_MIN;
                continue;
            }
            cmc = code[kk] - rnx_rdiv128((__int128)phase[kk] * 299792458, freq);
            if (prev1 != INT64_MIN && prev2 != INT64_MIN)
            {
                int64_t d2 = cmc - 2 * prev1 + prev2;
                sum += (double)(d2 < 0 ? -d2 : d2);
            }
            prev2 = prev1;
            prev1 = cmc;
        }
        if (best_sum < 0 || sum < best_sum)
        {
            best_sum = sum;
            best_chan = chan;
        }
    }
    return best_chan;
}

/** Index of the observation of type \a type on the band of \a code,
 * preferring the same tracking attribute, else the most-present one;
 * -1 if none. */
static int soc2_same_band(const struct rinex_system_data *p_sys,
    const struct rinex_satellite_data *sv, const int *present, char type,
    const char *code)
{
    int jj, best = -1;
    for (jj = 0; jj < p_sys->n_obs; ++jj)
    {
        const char *oc = p_sys->obs[jj];
        if (oc[0] != type || oc[1] != code[1] || sv->start[jj] < 0)
            continue;
        if (oc[2] == code[2])
            return jj;
        if (best < 0 || present[jj] > present[best])
            best = jj;
    }
    return best;
}

/** Plan SOC2 coding for the signals of satellite \a sv: fills \a t7[]
 * (satellite epoch times, 1e-7 s; obs_used entries) and \a plans[]
 * (p_sys->n_obs entries).  \a glo_chan_tab is from soc2_parse_glo_chan().
 * Returns 0, or -1 if out of memory. */
static int soc2_plan_sv(const struct rinex_data *data,
    const struct rinex_satellite_data *sv,
    const struct rinex_system_data *p_sys, const signed char glo_chan_tab[100],
    int64_t *t7, struct soc2_plan *plans)
{
    int count = sv->obs_used, n_obs = p_sys->n_obs;
    char sys = sv->id[0];
    int *present;
    int jj, kk, ref = -1, glo_chan = 0;

    present = calloc((size_t)n_obs, sizeof *present);
    if (!present)
        return -1;

    kk = 0;
    for (jj = 0; jj < sv->when_used; ++jj)
    {
        int ei;
        for (ei = sv->when[jj].start; ei < sv->when[jj].end; ++ei)
            t7[kk++] = rnx_epoch_time_e7(&data->epoch[ei]);
    }

    for (jj = 0; jj < n_obs; ++jj)
    {
        const int64_t *obs;
        if (sv->start[jj] < 0)
            continue;
        obs = data->obs + sv->start[jj];
        for (kk = 0; kk < count; ++kk)
            present[jj] += obs[kk] != INT64_MIN;
    }

    if (sys == 'R')
    {
        int slot = atoi(sv->id + 1);
        if (slot > 0 && slot < 100 && glo_chan_tab[slot] != GLO_CHAN_UNKNOWN)
            glo_chan = glo_chan_tab[slot];
        else
        {
            int cj = -1, lj = -1;
            for (jj = 0; jj < n_obs; ++jj)
            {
                const char *oc = p_sys->obs[jj];
                if (sv->start[jj] < 0 || oc[1] != '1')
                    continue;
                if ((oc[0] == 'C' || oc[0] == 'P') && cj < 0)
                    cj = jj;
                if (oc[0] == 'L' && lj < 0)
                    lj = jj;
            }
            if (cj >= 0 && lj >= 0)
                glo_chan = soc2_estimate_glo_chan(data->obs + sv->start[cj],
                    data->obs + sv->start[lj], count);
        }
    }

    /* Reference carrier phase: the one present at the most epochs. */
    for (jj = 0; jj < n_obs; ++jj)
    {
        const char *oc = p_sys->obs[jj];
        if (sv->start[jj] < 0 || oc[0] != 'L'
            || !soc2_freq_hz(sys, oc[1], glo_chan))
            continue;
        if (ref < 0 || present[jj] > present[ref])
            ref = jj;
    }

    for (jj = 0; jj < n_obs; ++jj)
    {
        const char *code = p_sys->obs[jj];
        struct soc2_pred *pred = &plans[jj].pred;
        int64_t f_tgt;
        int src;

        memset(pred, 0, sizeof *pred);
        pred->den = 1;
        plans[jj].s_ref = -1;
        if (sv->start[jj] < 0)
            continue;
        plans[jj].s_ref = soc2_same_band(p_sys, sv, present, 'S', code);

        f_tgt = soc2_freq_hz(sys, code[1], glo_chan);
        src = soc2_same_band(p_sys, sv, present, 'L', code);
        if (src < 0 || !soc2_freq_hz(sys, p_sys->obs[src][1], glo_chan))
            src = ref;
        if (ref < 0 || !f_tgt)
            continue;
        if (code[0] == 'L' && jj != ref)
        {
            int64_t f_ref = soc2_freq_hz(sys, p_sys->obs[ref][1], glo_chan);
            pred->kind = SRNX_PRED_VALUE;
            pred->ref = ref;
            soc2_set_ratio(pred, f_tgt, f_ref);
        }
        else if (code[0] == 'C' || code[0] == 'P')
        {
            int64_t f_src = soc2_freq_hz(sys, p_sys->obs[src][1], glo_chan);
            pred->kind = SRNX_PRED_VALUE;
            pred->ref = src;
            soc2_set_ratio(pred, 299792458, f_src);
        }
        else if (code[0] == 'D')
        {
            int64_t f_src = soc2_freq_hz(sys, p_sys->obs[src][1], glo_chan);
            pred->kind = SRNX_PRED_RATE;
            pred->ref = src;
            pred->stencil = 1;
            pred->max_span = SOC2_DOPPLER_MAX_SPAN;
            soc2_set_ratio(pred, -f_tgt, f_src);
        }
    }

    free(present);
    return 0;
}

/** Fill \a seq[] for observations \a obs[] (\a count satellite epochs) at
 * \a scale under predictor \a pr, with reference values \a ref_vals and
 * satellite epoch times \a t7: each entry is the scaled residual
 * x / scale - q, or SOC2_ABSENT, or SOC2_UNPRED.  With kind 0, residuals
 * are just x / scale.  Returns the number of residuals. */
static int soc2_build_seq(int64_t *seq, const int64_t *obs, int count,
    int64_t scale, const struct soc2_pred *pr, const int64_t *ref_vals,
    const int64_t *t7)
{
    int kk, coded = 0;

    for (kk = 0; kk < count; ++kk)
    {
        int64_t pp = 0;
        int avail = 0;

        if (obs[kk] == INT64_MIN)
        {
            seq[kk] = SOC2_ABSENT;
            continue;
        }
        if (pr->kind == SRNX_PRED_VALUE && ref_vals[kk] != INT64_MIN)
        {
            pp = rnx_rdiv128((__int128)ref_vals[kk] * pr->num, (__int128)pr->den);
            avail = 1;
        }
        else if (pr->kind == SRNX_PRED_RATE)
        {
            int aa = pr->stencil == 2 ? kk : kk - 1;
            int bb = pr->stencil == 0 ? kk : kk + 1;
            if (aa >= 0 && bb < count && ref_vals[aa] != INT64_MIN
                && ref_vals[bb] != INT64_MIN)
            {
                int64_t dt = t7[bb] - t7[aa];
                if (dt > 0 && (pr->max_span == 0 || (uint64_t)dt <= pr->max_span))
                {
                    pp = rnx_rdiv128((__int128)(ref_vals[bb] - ref_vals[aa])
                        * 10000000 * pr->num, (__int128)dt * pr->den);
                    avail = 1;
                }
            }
        }

        if (pr->kind == SRNX_PRED_NONE)
        {
            seq[kk] = obs[kk] / scale;
            coded++;
        }
        else if (avail)
        {
            seq[kk] = obs[kk] / scale - rnx_rdiv128(pp, scale);
            coded++;
        }
        else
            seq[kk] = SOC2_UNPRED;
    }
    return coded;
}

/** Bytes of the absent-value (0xFD) and unpredicted-value (0xFC) blocks
 * that \a seq[] needs. */
static size_t soc2_marker_bytes(const int64_t *seq, const int64_t *obs,
    int count, int64_t scale)
{
    size_t bytes = 0;
    int kk, run;

    for (kk = 0; kk < count; kk += run)
    {
        int64_t marker = seq[kk];
        run = 1;
        if (marker != SOC2_ABSENT && marker != SOC2_UNPRED)
            continue;
        while (kk + run < count && seq[kk + run] == marker)
            run++;
        bytes += 1 + (size_t)uleb128_len((uint64_t)(run - 1));
        if (marker == SOC2_UNPRED)
        {
            int jj;
            for (jj = kk; jj < kk + run; ++jj)
                bytes += (size_t)sleb128_len(obs[jj] / scale);
        }
    }
    return bytes;
}

#if !defined(RNX2SRNX_SEARCH)
/* ============================================================
 * rnx2srnx: the real converter.  Writes an actual SRNX file.
 * ============================================================
 */

/* Output filename for cleanup on fatal error. */
static const char *g_output_name;

/* Nonzero (--socd) to write revision 1.0 SOCD chunks instead of SOC2. */
static int g_use_socd;

/* ---- Memory-mapped output buffer ---- */

struct mmbuf
{
    unsigned char *data;
    size_t used;
    size_t cap;
};

/* Report a fatal error, unlink the output file if one was opened, and exit. */
_Noreturn static void fail_and_exit(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
_Noreturn static void fail_and_exit(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    fprintf(stderr, "rnx2srnx: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);

    if (g_output_name)
        unlink(g_output_name);
    exit(EXIT_FAILURE);
}

static void mm_require(struct mmbuf *mm, size_t need)
{
    if (mm->used + need > mm->cap)
    {
        fail_and_exit("Output exceeds mmap capacity "
            "(used=%zu need=%zu cap=%zu)",
            mm->used, need, mm->cap);
    }
}

static void mm_append(struct mmbuf *mm, const void *src, size_t len)
{
    mm_require(mm, len);
    memcpy(mm->data + mm->used, src, len);
    mm->used += len;
}

static void mm_byte(struct mmbuf *mm, unsigned char ch)
{
    mm_require(mm, 1);
    mm->data[mm->used++] = ch;
}

static void mm_uleb128(struct mmbuf *mm, uint64_t val)
{
    do {
        unsigned char ch = val & 127;
        val >>= 7;
        if (val)
            ch |= 128;
        mm_byte(mm, ch);
    } while (val);
}

/* ---- Growable scratch buffer (used for per-chunk payload assembly) ---- */

struct wbuf
{
    char *data;
    size_t used;
    size_t alloc;
};

static void wbuf_init(struct wbuf *wb)
{
    wb->data = NULL;
    wb->used = 0;
    wb->alloc = 0;
}

static void wbuf_free(struct wbuf *wb)
{
    free(wb->data);
    wb->data = NULL;
    wb->used = wb->alloc = 0;
}

static void wbuf_ensure(struct wbuf *wb, size_t need)
{
    if (wb->used + need <= wb->alloc)
        return;
    if (wb->alloc == 0)
        wb->alloc = 4096;
    while (wb->used + need > wb->alloc)
        wb->alloc *= 2;
    wb->data = realloc(wb->data, wb->alloc);
    if (!wb->data)
    {
        fail_and_exit("Out of memory");
    }
}

static void wbuf_append(struct wbuf *wb, const void *src, size_t len)
{
    wbuf_ensure(wb, len);
    memcpy(wb->data + wb->used, src, len);
    wb->used += len;
}

static void wbuf_byte(struct wbuf *wb, unsigned char ch)
{
    wbuf_ensure(wb, 1);
    wb->data[wb->used++] = ch;
}

/* ---- LEB128 encoding ---- */

static void wbuf_uleb128(struct wbuf *wb, uint64_t val)
{
    do {
        unsigned char ch = val & 127;
        val >>= 7;
        if (val)
            ch |= 128;
        wbuf_byte(wb, ch);
    } while (val);
}

static void wbuf_sleb128(struct wbuf *wb, int64_t val)
{
    uint64_t uv = ((uint64_t)val << 1) ^ (uint64_t)(val >> 63);
    wbuf_uleb128(wb, uv);
}

/* ---- Write a chunk to the mmap buffer ---- */

/** Compute and write the trailing chunk digest for the current
 * chunk-digest algorithm.  The chunk's FOURCC + ULEB128(len) header
 * and payload have already been appended to \a mm starting at
 * \a chunk_start.  Returns the number of digest bytes written (0 if
 * the current digest id is null).
 */
static size_t write_chunk_digest(struct mmbuf *mm, size_t chunk_start)
{
    int dig_len;

    dig_len = rnx_digest_length(g_chunk_digest_id);
    if (dig_len <= 0)
        return 0;
    mm_require(mm, (size_t)dig_len);
    if (rnx_digest(g_chunk_digest_id,
            mm->data + chunk_start, mm->used - chunk_start,
            mm->data + mm->used) < 0)
    {
        fail_and_exit("Unsupported chunk digest id=%d", g_chunk_digest_id);
    }
    mm->used += (size_t)dig_len;
    return (size_t)dig_len;
}

static void write_chunk(struct mmbuf *mm, const char fourcc[4],
    const void *payload, size_t payload_len)
{
    size_t chunk_start = mm->used;
    mm_append(mm, fourcc, 4);
    mm_uleb128(mm, payload_len);
    mm_append(mm, payload, payload_len);
    write_chunk_digest(mm, chunk_start);
}

/* ---- RLE-encode indicator array (LLI or SSI) ---- */

/* " 0123456789" — the only 11 symbols permitted in LLI/SSI streams. */
static const char indicator_alphabet[] = " 0123456789";

static void rle_encode_indicators(struct wbuf *wb, const char *ind, int count,
    const char *filename, const char *type, const char *obs_name)
{
    struct wbuf rle;
    int ii, run_start, idx;
    char cur;

    wbuf_init(&rle);

    /* Strip trailing spaces. */
    while (count > 0 && ind[count - 1] == ' ')
        count--;

    ii = 0;
    while (ii < count)
    {
        cur = ind[ii];
        run_start = ii;
        while (ii < count && ind[ii] == cur)
            ii++;
        idx = indicator_idx((unsigned char)cur);
        if (idx < 0)
        {
            fail_and_exit("%s: %s_%s %s byte 0x%02X not in alphabet (%d~%d/%d)",
                filename, obs_name, obs_name+4, type,
                (unsigned char)cur, run_start, ii, count);
        }
        /* Packed encoding: single ULEB128 = (count-1)*11 + idx */
        wbuf_uleb128(&rle, (uint64_t)(ii - run_start - 1) * 11 + (uint64_t)idx);
    }

    wbuf_uleb128(wb, rle.used);
    wbuf_append(wb, rle.data, rle.used);
    wbuf_free(&rle);
}

/* ---- DP-based block selection ---- */

/* ---- Reusable scratch buffers ----
 *
 * One scratch struct is owned by the top-level converter and threaded
 * through to compute_block_dp / encode_delta_run / write_socd_chunk so
 * the per-SOCD allocations (delta matrix, scaled arrays, DP tables,
 * choices) become a single grow-on-demand realloc per signal instead
 * of half a dozen mallocs.
 */
struct rnx2srnx_scratch
{
    size_t   cap;            /**< capacity in elements */
    int64_t *delta_matrix;   /**< 8 * cap */
    int64_t *full_scaled;    /**< cap */
    int64_t *present_scaled; /**< cap */
    int64_t *dp;             /**< cap + 1 */
    uint8_t *spt;            /**< RNX_N_BLOCK_SIZES * cap : sparse table for dp_core window-max */
    uint8_t *all_bw;         /**< 8 * cap : bw for all 8 delta orders */
    uint32_t *psum_arr;      /**< cap + 1 : prefix sums of sleb[] within dp_core */
    struct block_choice *choices_a; /**< cap; "best so far" */
    struct block_choice *choices_b; /**< cap; "candidate" */
    uint32_t *rice_q;        /**< (RNX_RICE_MAX_R + 1) * (cap + 1) : SOC2 prefix sums of q+1 per Rice parameter */
    uint64_t *usum;          /**< cap + 1 : SOC2 prefix sums of capped zigzag values */
    int64_t *seq;            /**< 2 * cap : SOC2 candidate value sequences */
};

static void scratch_init(struct rnx2srnx_scratch *s)
{
    memset(s, 0, sizeof *s);
}

static void scratch_free(struct rnx2srnx_scratch *s)
{
    free(s->delta_matrix);
    free(s->full_scaled);
    free(s->present_scaled);
    free(s->dp);
    free(s->spt);
    free(s->all_bw);
    free(s->psum_arr);
    free(s->choices_a);
    free(s->choices_b);
    free(s->rice_q);
    free(s->usum);
    free(s->seq);
    memset(s, 0, sizeof *s);
}

static void *xrealloc(void *p, size_t bytes)
{
    void *q = realloc(p, bytes);
    if (!q)
    {
        fail_and_exit("Out of memory");
    }
    return q;
}

static void scratch_reserve(struct rnx2srnx_scratch *s, size_t need)
{
    size_t cap;
    if (need <= s->cap)
        return;
    cap = s->cap ? s->cap : 2880;
    while (cap < need)
        cap *= 2;
    s->delta_matrix   = xrealloc(s->delta_matrix, 8 * cap * sizeof *s->delta_matrix);
    s->full_scaled    = xrealloc(s->full_scaled,      cap * sizeof *s->full_scaled);
    s->present_scaled = xrealloc(s->present_scaled,   cap * sizeof *s->present_scaled);
    s->dp             = xrealloc(s->dp,         (cap + 1) * sizeof *s->dp);
    s->spt            = xrealloc(s->spt,          RNX_N_BLOCK_SIZES * cap * sizeof *s->spt);
    s->all_bw         = xrealloc(s->all_bw,       8 * cap * sizeof *s->all_bw);
    s->psum_arr       = xrealloc(s->psum_arr,   (cap + 1) * sizeof *s->psum_arr);
    s->choices_a      = xrealloc(s->choices_a,        cap * sizeof *s->choices_a);
    s->choices_b      = xrealloc(s->choices_b,        cap * sizeof *s->choices_b);
    s->rice_q         = xrealloc(s->rice_q,
        (size_t)(RNX_RICE_MAX_R + 1) * (cap + 1) * sizeof *s->rice_q);
    s->usum           = xrealloc(s->usum,       (cap + 1) * sizeof *s->usum);
    s->seq            = xrealloc(s->seq,          2 * cap * sizeof *s->seq);
    s->cap = cap;
}

/* Block sizes for high-3-bits values 0..6; shared with the decoder and
 * scanner via rnx_block_sizes (rnx_priv.h). */

/* Per-cand[] sparse table: spt[k * cap + ii] = max(bw[ii .. ii + cand[k])).
 * Query is a direct load: max_bw for candidate ci starting at ii is
 * scratch->spt[ci * scratch->cap + ii]. */
static void build_spt(struct rnx2srnx_scratch *scratch,
    const uint8_t *bw, int count)
{
    uint8_t *spt = scratch->spt;
    size_t cap = scratch->cap;
    int ii, jj;

    /* Level 0: window = cand[0] = 8; scan 8 elements directly. */
    for (ii = 0; ii + 8 <= count; ++ii)
    {
        uint8_t mx = bw[ii];
        for (jj = 1; jj < 8; ++jj)
        {
            if (bw[ii + jj] > mx)
                mx = bw[ii + jj];
        }
        spt[ii] = mx;
    }

    /* Levels 1..5: cand[k] = cand[k-1]+8, so spt[k][ii] = max(spt[0][ii], spt[k-1][ii+8]).
     * Back-to-front single pass: when processing ii, ii+8 is already written for this
     * sweep, so all five levels can be updated at each position in one pass. */
    for (ii = count - 8; ii > 0; )
    {
        uint8_t a, b, *s = spt + --ii;

        a = s[0]; b = s[8];  a = a > b ? a : b;  s[1 * cap] = a;
        b = s[1 * cap + 8];  a = a > b ? a : b;  s[2 * cap] = a;
        b = s[2 * cap + 8];  a = a > b ? a : b;  s[3 * cap] = a;
        b = s[3 * cap + 8];  a = a > b ? a : b;  s[4 * cap] = a;
        b = s[4 * cap + 8];  a = a > b ? a : b;  s[5 * cap] = a;
    }

    /* Level 6: cand[6] = 112 = cand[5]+cand[5]+cand[1] = 48+48+16.
     * spt[6][ii] = max(spt[5][ii], spt[5][ii+48], spt[1][ii+96]). */
    if (count < 112)
        return;
    {
        const uint8_t *l1 = spt + 1 * cap;
        const uint8_t *l5 = spt + 5 * cap;
        uint8_t *l6 = spt + 6 * cap;
        for (ii = 0; ii + 112 <= count; ++ii)
        {
            uint8_t a = l5[ii + 0];
            uint8_t b = l5[ii + 48];
            uint8_t c = l1[ii + 96];
            uint8_t mx = a > b ? a : b;
            l6[ii] = mx > c ? mx : c;
        }
    }
}

/** Core DP: sparse-table window-max + right-to-left walk.
 *
 * \a bw is a pre-filled array of \a count bit-widths.  SLEB128 byte lengths
 * are derived from \a bw via the \c sleb128_len_from_bw lookup table.  \a vals
 * is needed only for zero-run detection.
 */
static int64_t dp_core(struct rnx2srnx_scratch *scratch,
    const int64_t *vals, const uint8_t *bw, int count, struct block_choice *choices)
{
    int64_t *dp;
    uint32_t *psum;
    int64_t result, cost;
    int ii, ci, zlen_here, zlen_next, L, max_L;

    dp = scratch->dp;
    psum = scratch->psum_arr;

    /* Build sparse table for O(1) range-max queries. */
    build_spt(scratch, bw, count);

    /* Prefix sums of SLEB128 byte lengths, derived from bit-widths.
     * SLEB128 length is a function of bit-width alone, so we can look
     * it up instead of storing a separate array.
     */
    psum[0] = 0;
    for (ii = 0; ii < count; ++ii)
        psum[ii + 1] = psum[ii] + sleb128_len_from_bw[bw[ii]];

    dp[count] = 0;

    /* zlen_next = length of the maximal zero run starting at ii+1,
     * maintained as we walk right-to-left.  Avoids O(N) re-scanning at
     * every position (which would be O(N^2) on constant signals).
     */
    zlen_next = 0;

    for (ii = count - 1; ii >= 0; --ii)
    {
        int remaining = count - ii, sleb_min_L;
        int64_t best = INT64_MAX, sleb_min;
        uint32_t pii;
        struct block_choice best_choice = {BK_SLEB, 0, 1};

        /* Zero-run transition (only the maximal run; splitting never wins). */
        zlen_here = (vals[ii] == 0) ? zlen_next + 1 : 0;
        zlen_next = zlen_here;
        if (zlen_here > 0)
        {
            cost = 1 + uleb128_len(zlen_here - 1) + dp[ii + zlen_here];
            if (cost < best)
            {
                best = cost;
                best_choice.kind = BK_ZERO;
                best_choice.bw = 0;
                best_choice.len = zlen_here;
            }
        }

        /* Block transitions.  cand[] is ascending so we can stop once
         * remaining < m or the bit width is too large.
         */
        for (ci = 0; ci < RNX_N_BLOCK_SIZES; ++ci)
        {
            int m = rnx_block_sizes[ci];
            int max_bw;

            max_bw = scratch->spt[(size_t)ci * scratch->cap + ii];
            if (remaining < m || max_bw > 32)
                break;
            cost = 1 + (int64_t)max_bw * m / 8 + dp[ii + m];
            if (cost < best)
            {
                best = cost;
                best_choice.kind = BK_BLOCK;
                best_choice.bw = (short)max_bw;
                best_choice.len = m;
            }
        }

        /* SLEB128-run transition: consider every length 1..min(remaining,g_max_sleb_run).
         * Costs fit in int32 (dp < 30 MB; psum delta <= 160 bytes for 16 vals).
         * Process 16-wide SIMD chunks updating the running argmin; a scalar
         * tail (starting at chunk_start) handles any remainder.
         */
        max_L = remaining < g_max_sleb_run ? remaining : g_max_sleb_run;
        pii = psum[ii];
        sleb_min = INT64_MAX;
        sleb_min_L = 0;
        {
            int chunk_start = 1;
#if defined(__ARM_NEON)
            for (; chunk_start + 15 <= max_L; chunk_start += 16)
            {
                const int64_t *dpv = dp + ii + chunk_start;
                int32x4_t dp0 = vcombine_s32(vmovn_s64(vld1q_s64(dpv+ 0)), vmovn_s64(vld1q_s64(dpv+ 2)));
                int32x4_t dp1 = vcombine_s32(vmovn_s64(vld1q_s64(dpv+ 4)), vmovn_s64(vld1q_s64(dpv+ 6)));
                int32x4_t dp2 = vcombine_s32(vmovn_s64(vld1q_s64(dpv+ 8)), vmovn_s64(vld1q_s64(dpv+10)));
                int32x4_t dp3 = vcombine_s32(vmovn_s64(vld1q_s64(dpv+12)), vmovn_s64(vld1q_s64(dpv+14)));
                uint32x4_t pii_v = vdupq_n_u32(pii);
                const uint32_t *psv = psum + ii + chunk_start;
                int32x4_t d0 = vreinterpretq_s32_u32(vsubq_u32(vld1q_u32(psv+ 0), pii_v));
                int32x4_t d1 = vreinterpretq_s32_u32(vsubq_u32(vld1q_u32(psv+ 4), pii_v));
                int32x4_t d2 = vreinterpretq_s32_u32(vsubq_u32(vld1q_u32(psv+ 8), pii_v));
                int32x4_t d3 = vreinterpretq_s32_u32(vsubq_u32(vld1q_u32(psv+12), pii_v));
                int32x4_t two = vdupq_n_s32(2);
                int32x4_t c0 = vaddq_s32(vaddq_s32(d0, dp0), two);
                int32x4_t c1 = vaddq_s32(vaddq_s32(d1, dp1), two);
                int32x4_t c2 = vaddq_s32(vaddq_s32(d2, dp2), two);
                int32x4_t c3 = vaddq_s32(vaddq_s32(d3, dp3), two);
                int b = chunk_start;
                int32x4_t i0 = (int32x4_t){b+ 0, b+ 1, b+ 2, b+ 3};
                int32x4_t i1 = (int32x4_t){b+ 4, b+ 5, b+ 6, b+ 7};
                int32x4_t i2 = (int32x4_t){b+ 8, b+ 9, b+10, b+11};
                int32x4_t i3 = (int32x4_t){b+12, b+13, b+14, b+15};
                uint32x4_t gt;
                gt = vcgtq_s32(c0, c1); c0 = vbslq_s32(gt, c1, c0); i0 = vbslq_s32(gt, i1, i0);
                gt = vcgtq_s32(c2, c3); c2 = vbslq_s32(gt, c3, c2); i2 = vbslq_s32(gt, i3, i2);
                gt = vcgtq_s32(c0, c2); c0 = vbslq_s32(gt, c2, c0); i0 = vbslq_s32(gt, i2, i0);
                int32x2_t c_lo = vget_low_s32(c0), c_hi = vget_high_s32(c0);
                int32x2_t i_lo = vget_low_s32(i0), i_hi = vget_high_s32(i0);
                uint32x2_t gt2 = vcgt_s32(c_lo, c_hi);
                c_lo = vbsl_s32(gt2, c_hi, c_lo);
                i_lo = vbsl_s32(gt2, i_hi, i_lo);
                int32_t ca = vget_lane_s32(c_lo, 0), ia = vget_lane_s32(i_lo, 0);
                int32_t cb = vget_lane_s32(c_lo, 1), ib = vget_lane_s32(i_lo, 1);
                int32_t chunk_min = (ca <= cb) ? ca : cb;
                int chunk_min_L = (ca <= cb) ? ia : ib;
                if (chunk_min < sleb_min) { sleb_min = chunk_min; sleb_min_L = chunk_min_L; }
            }
#elif defined(__AVX2__)
            for (; chunk_start + 15 <= max_L; chunk_start += 16)
            {
                const int64_t *dpv = dp + ii + chunk_start;
                __m128i n03 = _mm_unpacklo_epi64(
                    _mm_shuffle_epi32(_mm_loadu_si128((__m128i*)(dpv+ 0)), 0x08),
                    _mm_shuffle_epi32(_mm_loadu_si128((__m128i*)(dpv+ 2)), 0x08));
                __m128i n47 = _mm_unpacklo_epi64(
                    _mm_shuffle_epi32(_mm_loadu_si128((__m128i*)(dpv+ 4)), 0x08),
                    _mm_shuffle_epi32(_mm_loadu_si128((__m128i*)(dpv+ 6)), 0x08));
                __m128i n8b = _mm_unpacklo_epi64(
                    _mm_shuffle_epi32(_mm_loadu_si128((__m128i*)(dpv+ 8)), 0x08),
                    _mm_shuffle_epi32(_mm_loadu_si128((__m128i*)(dpv+10)), 0x08));
                __m128i ncf = _mm_unpacklo_epi64(
                    _mm_shuffle_epi32(_mm_loadu_si128((__m128i*)(dpv+12)), 0x08),
                    _mm_shuffle_epi32(_mm_loadu_si128((__m128i*)(dpv+14)), 0x08));
                __m256i dp0 = _mm256_set_m128i(n47, n03);
                __m256i dp1 = _mm256_set_m128i(ncf, n8b);
                __m256i pii_v = _mm256_set1_epi32((int)pii);
                const uint32_t *psv = psum + ii + chunk_start;
                __m256i d0 = _mm256_sub_epi32(_mm256_loadu_si256((__m256i*)(psv+ 0)), pii_v);
                __m256i d1 = _mm256_sub_epi32(_mm256_loadu_si256((__m256i*)(psv+ 8)), pii_v);
                __m256i two = _mm256_set1_epi32(2);
                __m256i c0 = _mm256_add_epi32(_mm256_add_epi32(d0, dp0), two);
                __m256i c1 = _mm256_add_epi32(_mm256_add_epi32(d1, dp1), two);
                int b = chunk_start;
                __m256i i0 = _mm256_set_epi32(b+7, b+6, b+5, b+4, b+3, b+2, b+1, b+0);
                __m256i i1 = _mm256_set_epi32(b+15, b+14, b+13, b+12, b+11, b+10, b+9, b+8);
                __m256i gt = _mm256_cmpgt_epi32(c0, c1);
                __m256i cv = _mm256_blendv_epi8(c0, c1, gt);
                __m256i iv = _mm256_blendv_epi8(i0, i1, gt);
                __m128i cv_lo = _mm256_castsi256_si128(cv);
                __m128i cv_hi = _mm256_extracti128_si256(cv, 1);
                __m128i iv_lo = _mm256_castsi256_si128(iv);
                __m128i iv_hi = _mm256_extracti128_si256(iv, 1);
                __m128i gt2 = _mm_cmpgt_epi32(cv_lo, cv_hi);
                cv_lo = _mm_blendv_epi8(cv_lo, cv_hi, gt2);
                iv_lo = _mm_blendv_epi8(iv_lo, iv_hi, gt2);
                __m128i cv_r = _mm_shuffle_epi32(cv_lo, 0x4E);
                __m128i iv_r = _mm_shuffle_epi32(iv_lo, 0x4E);
                __m128i gt3 = _mm_cmpgt_epi32(cv_lo, cv_r);
                cv_lo = _mm_blendv_epi8(cv_lo, cv_r, gt3);
                iv_lo = _mm_blendv_epi8(iv_lo, iv_r, gt3);
                cv_r = _mm_shuffle_epi32(cv_lo, 0xB1);
                iv_r = _mm_shuffle_epi32(iv_lo, 0xB1);
                __m128i gt4 = _mm_cmpgt_epi32(cv_lo, cv_r);
                cv_lo = _mm_blendv_epi8(cv_lo, cv_r, gt4);
                iv_lo = _mm_blendv_epi8(iv_lo, iv_r, gt4);
                int32_t chunk_min = _mm_cvtsi128_si32(cv_lo);
                int chunk_min_L = _mm_cvtsi128_si32(iv_lo);
                if (chunk_min < sleb_min) { sleb_min = chunk_min; sleb_min_L = chunk_min_L; }
            }
#endif /* __ARM_NEON || __AVX2__ */
            for (L = chunk_start; L <= max_L; ++L)
            {
                int64_t c = 2 + (int64_t)(psum[ii + L] - pii) + dp[ii + L];
                if (c < sleb_min) { sleb_min = c; sleb_min_L = L; }
            }
        }
        if (sleb_min < best)
        {
            best = sleb_min;
            best_choice.kind = BK_SLEB;
            best_choice.bw = 0;
            best_choice.len = sleb_min_L;
        }

        dp[ii] = best;
        if (choices)
            choices[ii] = best_choice;
    }

    result = dp[0];
    return result;
}

/** Precompute bit-widths for all 8 delta orders into scratch->all_bw
 * (8 * cap entries, strided by cap).  Call after compute_delta_matrix while
 * the delta values are hot in cache. */
static void precompute_all_bw(struct rnx2srnx_scratch *scratch,
    const int64_t *delta_matrix, int count)
{
    int ord, ii;
    for (ord = 0; ord < 8; ++ord)
    {
        const int64_t *vals = delta_matrix + (size_t)ord * count;
        uint8_t *bw = scratch->all_bw + (size_t)ord * scratch->cap;
        for (ii = 0; ii < count; ++ii)
        {
            bw[ii] = (uint8_t)twos_comp_bw(vals[ii]);
        }
    }
}

/** Compute optimal byte cost to encode \a vals[0..count-1] as SOCD blocks.
 *
 * If \a bw is non-NULL it is used as the precomputed bit-width array;
 * otherwise bit-widths are computed from \a vals.
 *
 * If \a choices is non-NULL, it must have room for \a count entries; on
 * return it is populated such that walking forward from index 0 by
 * \c choices[i].len steps reproduces the optimal block sequence.
 *
 * Runs in O(count) time: bit-widths are computed once, and the per-window
 * max over each candidate block size m ∈ {8,16,24,32,40,48,112} is built
 * by a power-of-2 sparse table (7 levels, O(count) build, O(1) query).
 */
static int64_t compute_block_dp(struct rnx2srnx_scratch *scratch,
    const int64_t *vals, uint8_t *bw, int count, struct block_choice *choices)
{
    if (count <= 0)
        return 0;

    scratch_reserve(scratch, (size_t)count);

    return dp_core(scratch, vals, bw, count, choices);
}

/* ---- Delta order selection ---- */

/** Select the best delta order (0..7) for a pre-computed delta matrix.
 *
 * Each candidate order's choices are computed into the "candidate" buffer
 * (scratch->choices_b); when it beats the running best we swap pointers so
 * scratch->choices_a always holds the best order's choices.  On return,
 * \a *out_choices points to the chosen order's choices (suitable for direct
 * use by the encoder, modulo the gap caveat).
 *
 * Early-stop: break once two consecutive orders fail to improve.
 */
static int select_delta_order(struct rnx2srnx_scratch *scratch,
    const int64_t *delta_matrix, int count, struct block_choice **out_choices)
{
    int best_order = 0;
    int64_t best_cost;
    int ord, no_improve;
    struct block_choice *best_buf = scratch->choices_a;
    struct block_choice *cand_buf = scratch->choices_b;

    precompute_all_bw(scratch, delta_matrix, count);

    best_cost = dp_core(scratch, delta_matrix,
        scratch->all_bw,
        count, best_buf);
    no_improve = 0;

    for (ord = 1; ord <= 7 && ord < count; ++ord)
    {
        const int64_t *ord_vals = delta_matrix + (size_t)ord * count;
        /* Cost of zero-valued SLEB128 init values: 1 byte each. */
        int64_t cost = ord;
        cost += dp_core(scratch, ord_vals,
            scratch->all_bw + (size_t)ord * scratch->cap,
            count, cand_buf);

        if (cost < best_cost)
        {
            struct block_choice *tmp = best_buf;
            best_buf = cand_buf;
            cand_buf = tmp;
            best_cost = cost;
            best_order = ord;
            no_improve = 0;
        }
        else if (++no_improve >= 2)
        {
            break;
        }
    }

    *out_choices = best_buf;
    return best_order;
}

/* ---- Bit-matrix transposition (encode direction) ---- */

/** Write a bit-transposed block of \a count values (a multiple of 8, at
 * most RNX_MAX_RICE_BLOCK), each \a bits wide (at most 32).
 */
static void write_transposed_block(struct wbuf *wb, const int64_t *vals,
    int count, int bits)
{
    int byte_col, row;
    int bytes_per_row = count >> 3;
    char matrix[32 * (RNX_MAX_RICE_BLOCK / 8)];

    /* Process 8 columns at a time (one output byte per row).
     * Each group of 8 vals[] is loaded once and reused across all rows,
     * eliminating the bits-fold reload of the row-major layout. */
    for (byte_col = 0; byte_col < bytes_per_row; ++byte_col)
    {
        const int64_t *v = vals + byte_col * 8;
        uint64_t v0 = (uint64_t)v[0], v1 = (uint64_t)v[1];
        uint64_t v2 = (uint64_t)v[2], v3 = (uint64_t)v[3];
        uint64_t v4 = (uint64_t)v[4], v5 = (uint64_t)v[5];
        uint64_t v6 = (uint64_t)v[6], v7 = (uint64_t)v[7];
        char *out = matrix + byte_col;

        for (row = 0; row < bits; ++row)
        {
            int shift = bits - 1 - row;
            char b = (char)(
                ((v0 >> shift) & 1) << 7 |
                ((v1 >> shift) & 1) << 6 |
                ((v2 >> shift) & 1) << 5 |
                ((v3 >> shift) & 1) << 4 |
                ((v4 >> shift) & 1) << 3 |
                ((v5 >> shift) & 1) << 2 |
                ((v6 >> shift) & 1) << 1 |
                ((v7 >> shift) & 1));
            out[row * bytes_per_row] = b;
        }
    }

    wbuf_append(wb, matrix, bits * bytes_per_row);
}

/* ---- Encode packed observation data ---- */

/** Emit blocks for \a run_len values from \a ord_vals using \a choices. */
static void emit_blocks(struct wbuf *packed, const int64_t *ord_vals,
    int run_len, const struct block_choice *choices)
{
    int ii = 0;
    while (ii < run_len)
    {
        struct block_choice c = choices[ii];

        switch (c.kind)
        {
        case BK_BLOCK:
        {
            unsigned char header;
            int hi3;
            for (hi3 = 0; hi3 < RNX_N_BLOCK_SIZES - 1; ++hi3)
                if (rnx_block_sizes[hi3] == c.len)
                    break;
            header = (unsigned char)(hi3 << 5)
                   | (unsigned char)(c.bw - 1);
            wbuf_byte(packed, header);
            write_transposed_block(packed, ord_vals + ii, c.len, c.bw);
            break;
        }
        case BK_ZERO:
            wbuf_byte(packed, 0xFE);
            wbuf_uleb128(packed, c.len - 1);
            break;
        case BK_SLEB:
        {
            int jj;
            wbuf_byte(packed, 0xFF);
            wbuf_uleb128(packed, c.len - 1);
            for (jj = 0; jj < c.len; ++jj)
                wbuf_sleb128(packed, ord_vals[ii + jj]);
            break;
        }
        case BK_RICE:
            fail_and_exit("split-Rice block in SOCD encoding");
        }

        ii += c.len;
    }
}

/** Encode a run of \a run_len present delta values from \a ord_vals into \a packed.
 *
 * If \a precomputed is non-NULL it is used as-is; otherwise the choices are
 * computed via the DP into scratch->choices_b (overwriting any candidate
 * buffer left by a prior selection pass — caller must not rely on it).
 * \a bw points to the precomputed bit-widths for this run (may be NULL).
 */
static void encode_delta_run(struct wbuf *packed,
    struct rnx2srnx_scratch *scratch,
    const int64_t *ord_vals, uint8_t *bw, int run_len,
    const struct block_choice *precomputed)
{
    const struct block_choice *choices;

    if (run_len <= 0)
        return;

    if (precomputed)
    {
        choices = precomputed;
    }
    else
    {
        compute_block_dp(scratch, ord_vals, bw, run_len, scratch->choices_b);
        choices = scratch->choices_b;
    }

    emit_blocks(packed, ord_vals, run_len, choices);
}

/** Encode packed observation data.
 *
 * \a full_obs is the full observation array (total_count elements); absent
 * observations are marked with INT64_MIN.  \a delta_matrix holds the
 * delta-coded values for the \a present_count non-absent observations only
 * (in the same layout as compute_delta_matrix).  Absent values are emitted
 * as 0xFD blocks; the delta accumulator is not advanced for them.
 */
static void encode_packed_observations(struct wbuf *wb,
    struct rnx2srnx_scratch *scratch,
    const int64_t *full_obs, int total_count,
    const int64_t *delta_matrix, int present_count, int order, int64_t obs_gcd,
    const struct block_choice *cached_choices)
{
    struct wbuf packed;
    const struct block_choice *run_choices;
    const int64_t *ord_vals;
    uint8_t *bw_base;
    int ii, pi, run_end, run_len;

    ord_vals = (delta_matrix && present_count > 0)
        ? delta_matrix + (size_t)order * present_count
        : NULL;
    bw_base = (delta_matrix && present_count > 0)
        ? scratch->all_bw + (size_t)order * scratch->cap
        : NULL;

    wbuf_init(&packed);

    if (obs_gcd > 1)
    {
        wbuf_uleb128(&packed, order + 8);
        wbuf_uleb128(&packed, obs_gcd);
    }
    else
    {
        wbuf_uleb128(&packed, order);
    }

    /* SLEB128 initialization sequence: order zero values (cold start). */
    for (ii = 0; ii < order; ++ii)
        wbuf_sleb128(&packed, 0);

    /* Walk full_obs interleaving absent (0xFD) and present delta blocks. */
    ii = 0;
    pi = 0;
    while (ii < total_count)
    {
        /* Absent run. */
        if (full_obs[ii] == INT64_MIN)
        {
            int absent_len = 0;
            while (ii + absent_len < total_count
                && full_obs[ii + absent_len] == INT64_MIN)
                absent_len++;
            wbuf_byte(&packed, 0xFD);
            wbuf_uleb128(&packed, absent_len - 1);
            ii += absent_len;
            continue;
        }

        /* Present run: find its extent then block-encode. */
        run_end = ii;
        run_choices = NULL;
        while (run_end < total_count && full_obs[run_end] != INT64_MIN)
            run_end++;
        run_len = run_end - ii;
        /* Cached choices are valid only when the present values form a
         * single contiguous run starting at offset 0 of ord_vals — i.e.
         * the SOCD has no absent observations.  Otherwise per-run DP
         * boundaries differ, so recompute. */
        if (cached_choices && pi == 0 && run_len == present_count)
            run_choices = cached_choices;
        encode_delta_run(&packed, scratch, ord_vals + pi, bw_base + pi, run_len, run_choices);
        pi += run_len;
        ii = run_end;
    }

    wbuf_uleb128(wb, packed.used);
    wbuf_append(wb, packed.data, packed.used);
    wbuf_free(&packed);
}

/* ---- Write SOCD chunk ---- */

/** Write a SOCD chunk for one (satellite, signal) pair.
 * Returns the offset (within \a mm) where the SOCD chunk starts.
 */
static size_t write_socd_chunk(struct mmbuf *mm,
    struct rnx2srnx_scratch *scratch,
    const struct rinex_data *data,
    const struct rinex_satellite_data *sv, int sig_idx,
    const struct rinex_system_data *p_sys,
    const char *filename)
{
    struct wbuf payload;
    size_t offset;
    char obs_name[8];
    const int64_t *obs_arr;
    const char *lli_arr, *ssi_arr;
    int obs_count, ii;
    int64_t obs_gcd;

    if (sv->start[sig_idx] < 0)
        return 0; /* signal never observed */

    obs_arr = data->obs + sv->start[sig_idx];
    lli_arr = data->lli + sv->start[sig_idx];
    ssi_arr = data->ssi + sv->start[sig_idx];
    obs_count = sv->obs_used;

    /* Build 8-byte observation name. */
    memset(obs_name, 0, 8);
    memcpy(obs_name, sv->id, 3);
    /* obs_name[3] = '\0' already */
    memcpy(obs_name + 4, p_sys->obs[sig_idx], 3);

    wbuf_init(&payload);
    wbuf_append(&payload, obs_name, 8);

    /* Count-minus-1 of observations. */
    wbuf_uleb128(&payload, obs_count - 1);

    /* RLE-encode LLI and SSI. */
    rle_encode_indicators(&payload, lli_arr, obs_count, filename, "LLI", obs_name);
    rle_encode_indicators(&payload, ssi_arr, obs_count, filename, "SSI", obs_name);

    /* Compute GCD of non-missing observation values (skip INT64_MIN sentinel). */
    obs_gcd = 0;
    for (ii = 0; ii < obs_count; ++ii)
    {
        if (obs_arr[ii] != INT64_MIN)
            obs_gcd = gcd(obs_gcd, obs_arr[ii]);
    }
    if (obs_gcd <= 0 || obs_gcd > 1000000)
        obs_gcd = 1;

    /* Build present-only scaled array for delta order selection and encoding.
     * Absent values (INT64_MIN) are excluded from delta coding and GCD; they
     * will be emitted as 0xFD blocks interleaved with the delta blocks.
     */
    {
        int64_t *full_scaled;
        int64_t *present_scaled;
        int64_t *delta_matrix = NULL;
        struct block_choice *cached_choices = NULL;
        int present_count = 0;
        int order = 0;

        scratch_reserve(scratch, (size_t)obs_count);
        full_scaled    = scratch->full_scaled;
        present_scaled = scratch->present_scaled;

        for (ii = 0; ii < obs_count; ++ii)
        {
            if (obs_arr[ii] == INT64_MIN)
            {
                full_scaled[ii] = INT64_MIN;
            }
            else
            {
                full_scaled[ii] = obs_arr[ii] / obs_gcd;
                present_scaled[present_count++] = full_scaled[ii];
            }
        }

        if (present_count > 0)
        {
            delta_matrix = scratch->delta_matrix;
            compute_delta_matrix(delta_matrix, present_scaled, present_count);
            order = select_delta_order(scratch, delta_matrix, present_count,
                &cached_choices);
        }

        encode_packed_observations(&payload, scratch, full_scaled, obs_count,
            delta_matrix, present_count, order, obs_gcd, cached_choices);
    }

    /* Write the chunk. */
    offset = mm->used;
    write_chunk(mm, "SOCD", payload.data, payload.used);
    wbuf_free(&payload);
    return offset;
}

/* ---- Write SOC2 chunk ---- */

/** Rice-DP scratch view of the converter's scratch buffers. */
static struct rice_scratch enc_rice_scratch(struct rnx2srnx_scratch *scratch)
{
    struct rice_scratch rs;
    rs.dp = scratch->dp;
    rs.psum = scratch->psum_arr;
    rs.usum = scratch->usum;
    rs.rice_q = scratch->rice_q;
    return rs;
}

/* Context for select_delta_order_rice()'s order evaluations.  Each
 * evaluation writes its block choices to #cand; accepting it swaps #cand
 * and #best, as select_delta_order() does for SOCD. */
struct rice_order_ctx
{
    struct rnx2srnx_scratch *scratch;
    const int64_t *delta_matrix;
    int count;
    struct block_choice *best;
    struct block_choice *cand;
};

static int64_t rice_order_eval(void *ctx, int ord)
{
    struct rice_order_ctx *oc = ctx;
    struct rice_scratch rs = enc_rice_scratch(oc->scratch);
    return rice_dp(&rs, oc->delta_matrix + (size_t)ord * oc->count, oc->count,
        rnx_rice_block_sizes, RNX_N_BLOCK_SIZES, oc->cand);
}

static void rice_order_accept(void *ctx)
{
    struct rice_order_ctx *oc = ctx;
    struct block_choice *tmp = oc->best;
    oc->best = oc->cand;
    oc->cand = tmp;
}

/** Select the delta order for the split-Rice coding of the \a count values
 * whose delta matrix is \a delta_matrix, per --order-search;
 * \a *out_choices receives the chosen order's block choices. */
static int select_delta_order_rice(struct rnx2srnx_scratch *scratch,
    const int64_t *delta_matrix, int count, struct block_choice **out_choices)
{
    struct rice_order_ctx oc = { scratch, delta_matrix, count,
        scratch->choices_a, scratch->choices_b };
    int64_t cost;
    int order = rice_select_order(delta_matrix, count, rice_order_eval,
        rice_order_accept, &oc, &cost);

    *out_choices = oc.best;
    return order;
}

/** Write a split-Rice block of \a m values with Rice parameter \a rr:
 * the header byte, the transposed matrix of remainders, then the unary
 * quotients padded to a byte boundary. */
static void write_rice_block(struct wbuf *wb, const int64_t *vals, int m, int rr)
{
    int64_t rem[RNX_MAX_RICE_BLOCK];
    uint64_t mask = rr ? ((uint64_t)1 << rr) - 1 : 0;
    unsigned acc = 0;
    int size_idx, nbits = 0, ii;

    for (size_idx = 0; size_idx < RNX_N_BLOCK_SIZES - 1; ++size_idx)
        if (rnx_rice_block_sizes[size_idx] == m)
            break;
    wbuf_byte(wb, (unsigned char)(size_idx << 5 | rr));

    if (rr > 0)
    {
        for (ii = 0; ii < m; ++ii)
        {
            uint64_t uu = ((uint64_t)vals[ii] << 1) ^ (uint64_t)(vals[ii] >> 63);
            rem[ii] = (int64_t)(uu & mask);
        }
        write_transposed_block(wb, rem, m, rr);
    }

    /* Quotients: q one bits then a zero bit, most significant bit first. */
    for (ii = 0; ii < m; ++ii)
    {
        uint64_t uu = ((uint64_t)vals[ii] << 1) ^ (uint64_t)(vals[ii] >> 63);
        uint64_t qq = uu >> rr;
        for (; qq > 0; --qq)
        {
            acc = acc << 1 | 1;
            if (++nbits == 8)
            {
                wbuf_byte(wb, (unsigned char)acc);
                acc = 0;
                nbits = 0;
            }
        }
        acc <<= 1;
        if (++nbits == 8)
        {
            wbuf_byte(wb, (unsigned char)acc);
            acc = 0;
            nbits = 0;
        }
    }
    if (nbits > 0)
        wbuf_byte(wb, (unsigned char)(acc << (8 - nbits)));
}

/** Emit SOC2 blocks for \a run_len values from \a ord_vals using \a choices. */
static void emit_rice_blocks(struct wbuf *packed, const int64_t *ord_vals,
    int run_len, const struct block_choice *choices)
{
    int ii = 0, jj;
    while (ii < run_len)
    {
        struct block_choice cc = choices[ii];
        switch (cc.kind)
        {
        case BK_RICE:
            write_rice_block(packed, ord_vals + ii, cc.len, cc.bw);
            break;
        case BK_ZERO:
            wbuf_byte(packed, 0xFE);
            wbuf_uleb128(packed, cc.len - 1);
            break;
        case BK_SLEB:
            wbuf_byte(packed, 0xFF);
            wbuf_uleb128(packed, cc.len - 1);
            for (jj = 0; jj < cc.len; ++jj)
                wbuf_sleb128(packed, ord_vals[ii + jj]);
            break;
        case BK_BLOCK:
            fail_and_exit("bit-matrix block in SOC2 encoding");
        }
        ii += cc.len;
    }
}

/** Encode SOC2 packed observation data for \a seq[] (scaled residuals or
 * SOC2_ABSENT / SOC2_UNPRED markers; \a obs[] supplies unpredicted
 * values) into \a out, with its ULEB128 length prefix. */
static void encode_soc2_packed(struct wbuf *out, struct rnx2srnx_scratch *scratch,
    const int64_t *seq, const int64_t *obs, int count, int64_t scale)
{
    struct wbuf packed;
    struct block_choice *cached_choices = NULL;
    const int64_t *ord_vals = NULL;
    int64_t *coded = scratch->present_scaled;
    int ii, pi, n_coded = 0, order = 0;

    for (ii = 0; ii < count; ++ii)
        if (seq[ii] > SOC2_UNPRED)
            coded[n_coded++] = seq[ii];
    if (n_coded > 0)
    {
        compute_delta_matrix(scratch->delta_matrix, coded, n_coded);
        order = select_delta_order_rice(scratch, scratch->delta_matrix,
            n_coded, &cached_choices);
        ord_vals = scratch->delta_matrix + (size_t)order * n_coded;
    }

    wbuf_init(&packed);
    if (scale > 1)
    {
        wbuf_uleb128(&packed, order + 8);
        wbuf_uleb128(&packed, scale);
    }
    else
        wbuf_uleb128(&packed, order);
    for (ii = 0; ii < order; ++ii)
        wbuf_sleb128(&packed, 0);

    for (ii = 0, pi = 0; ii < count; )
    {
        int run = 1, jj;
        if (seq[ii] == SOC2_ABSENT || seq[ii] == SOC2_UNPRED)
        {
            while (ii + run < count && seq[ii + run] == seq[ii])
                run++;
            wbuf_byte(&packed, seq[ii] == SOC2_ABSENT ? 0xFD : SRNX_BLOCK_UNPRED);
            wbuf_uleb128(&packed, run - 1);
            if (seq[ii] == SOC2_UNPRED)
                for (jj = ii; jj < ii + run; ++jj)
                    wbuf_sleb128(&packed, obs[jj] / scale);
        }
        else
        {
            const struct block_choice *choices = cached_choices;
            while (ii + run < count && seq[ii + run] > SOC2_UNPRED)
                run++;
            /* Whole-sequence choices are valid only for a single run. */
            if (!(pi == 0 && run == n_coded))
            {
                struct rice_scratch rs = enc_rice_scratch(scratch);
                rice_dp(&rs, ord_vals + pi, run, rnx_rice_block_sizes,
                    RNX_N_BLOCK_SIZES, scratch->choices_b);
                choices = scratch->choices_b;
            }
            emit_rice_blocks(&packed, ord_vals + pi, run, choices);
            pi += run;
        }
        ii += run;
    }

    wbuf_uleb128(out, packed.used);
    wbuf_append(out, packed.data, packed.used);
    wbuf_free(&packed);
}

/** Write a ULEB128-length-prefixed RLE of \a sym[] over \a n_sym symbols. */
static void wbuf_rle_symbols(struct wbuf *wb, const unsigned char *sym,
    int count, int n_sym)
{
    struct wbuf rle;
    int ii = 0;

    wbuf_init(&rle);
    while (count > 0 && sym[count - 1] == 0)
        count--;
    while (ii < count)
    {
        int run_start = ii;
        while (ii < count && sym[ii] == sym[run_start])
            ii++;
        wbuf_uleb128(&rle, (uint64_t)(ii - run_start - 1) * n_sym + sym[run_start]);
    }
    wbuf_uleb128(wb, rle.used);
    wbuf_append(wb, rle.data, rle.used);
    wbuf_free(&rle);
}

/** Write a SOC2 chunk for one (satellite, signal) pair, using the planned
 * predictor and SSI reference only where they make the chunk smaller.
 * Returns the offset (within \a mm) where the chunk starts.
 */
static size_t write_soc2_chunk(struct mmbuf *mm,
    struct rnx2srnx_scratch *scratch,
    const struct rinex_data *data,
    const struct rinex_satellite_data *sv, int sig_idx,
    const struct rinex_system_data *p_sys,
    const struct soc2_plan *plan, const int64_t *t7,
    const char *filename)
{
    static const struct soc2_pred none = {0, 0, 0, 1, 0, 0};
    struct wbuf payload, packed[2];
    const struct soc2_pred *pred[2];
    char obs_name[8];
    const int64_t *obs_arr;
    const char *lli_arr, *ssi_arr;
    unsigned char *sym;
    size_t offset, ssi_plain, ssi_pred = SIZE_MAX, cost[2];
    int obs_count, ii, bad, n_cand, best, ssi_ref = 0;
    int64_t obs_gcd;

    if (sv->start[sig_idx] < 0)
        return 0;
    obs_arr = data->obs + sv->start[sig_idx];
    lli_arr = data->lli + sv->start[sig_idx];
    ssi_arr = data->ssi + sv->start[sig_idx];
    obs_count = sv->obs_used;
    scratch_reserve(scratch, (size_t)obs_count);

    memset(obs_name, 0, 8);
    memcpy(obs_name, sv->id, 3);
    memcpy(obs_name + 4, p_sys->obs[sig_idx], 3);

    obs_gcd = 0;
    for (ii = 0; ii < obs_count; ++ii)
        if (obs_arr[ii] != INT64_MIN)
            obs_gcd = gcd(obs_gcd, obs_arr[ii]);
    if (obs_gcd <= 0 || obs_gcd > 1000000)
        obs_gcd = 1;

    /* SSI coding: plain, or against the planned S observation. */
    sym = (unsigned char *)xrealloc(NULL, (size_t)obs_count);
    bad = soc2_ssi_symbols(sym, ssi_arr, NULL, obs_count);
    if (bad >= 0)
    {
        fail_and_exit("%s: %s_%s SSI byte 0x%02X not in alphabet (%d/%d)",
            filename, obs_name, obs_name + 4, (unsigned char)ssi_arr[bad],
            bad, obs_count);
    }
    ssi_plain = 1 + rle_symbols_bytes(sym, obs_count, 11);
    if (plan->s_ref >= 0)
    {
        soc2_ssi_symbols(sym, ssi_arr, data->obs + sv->start[plan->s_ref], obs_count);
        ssi_pred = (size_t)uleb128_len(((uint64_t)plan->s_ref + 1) << 2)
            + rle_symbols_bytes(sym, obs_count, 12);
        if (ssi_pred < ssi_plain)
            ssi_ref = plan->s_ref + 1;
    }

    /* Code the values without and (if planned) with the predictor, and
     * keep the smaller. */
    pred[0] = &none;
    pred[1] = &plan->pred;
    n_cand = plan->pred.kind != SRNX_PRED_NONE ? 2 : 1;
    best = 0;
    for (ii = 0; ii < n_cand; ++ii)
    {
        int64_t *seq = scratch->seq + (size_t)ii * scratch->cap;
        soc2_build_seq(seq, obs_arr, obs_count, obs_gcd, pred[ii],
            ii ? data->obs + sv->start[pred[ii]->ref] : NULL, t7);
        wbuf_init(&packed[ii]);
        encode_soc2_packed(&packed[ii], scratch, seq, obs_arr, obs_count, obs_gcd);
        cost[ii] = soc2_pred_bytes(pred[ii]) + packed[ii].used;
        if (cost[ii] < cost[best])
            best = ii;
    }

    wbuf_init(&payload);
    wbuf_append(&payload, obs_name, 8);
    wbuf_uleb128(&payload, obs_count - 1);
    rle_encode_indicators(&payload, lli_arr, obs_count, filename, "LLI", obs_name);
    wbuf_uleb128(&payload, (uint64_t)ssi_ref << 2 | (uint64_t)pred[best]->kind);
    if (ssi_ref)
        wbuf_rle_symbols(&payload, sym, obs_count, 12);
    else
    {
        soc2_ssi_symbols(sym, ssi_arr, NULL, obs_count);
        wbuf_rle_symbols(&payload, sym, obs_count, 11);
    }
    if (pred[best]->kind != SRNX_PRED_NONE)
    {
        wbuf_uleb128(&payload, (uint64_t)pred[best]->ref);
        wbuf_sleb128(&payload, pred[best]->num);
        wbuf_uleb128(&payload, pred[best]->den);
        if (pred[best]->kind == SRNX_PRED_RATE)
        {
            wbuf_uleb128(&payload, (uint64_t)pred[best]->stencil);
            wbuf_uleb128(&payload, pred[best]->max_span);
        }
    }
    wbuf_append(&payload, packed[best].data, packed[best].used);

    offset = mm->used;
    write_chunk(mm, "SOC2", payload.data, payload.used);
    wbuf_free(&payload);
    for (ii = 0; ii < n_cand; ++ii)
        wbuf_free(&packed[ii]);
    free(sym);
    return offset;
}

/* ---- Write SATE chunk ---- */

static size_t write_sate_chunk(struct mmbuf *mm,
    const struct rinex_satellite_data *sv,
    int n_obs, const size_t *socd_offsets, size_t sate_file_pos)
{
    struct wbuf payload;
    size_t offset;
    int ii, prev_end;

    wbuf_init(&payload);

    /* Satellite name + '\0'. */
    wbuf_append(&payload, sv->id, 4);

    /* SLEB128 file offsets for each signal's SOCD chunk,
     * relative to the start of this SATE chunk's FOURCC.
     */
    for (ii = 0; ii < n_obs; ++ii)
    {
        if (socd_offsets[ii] == 0)
            wbuf_sleb128(&payload, 0);
        else
            wbuf_sleb128(&payload, (int64_t)socd_offsets[ii] - (int64_t)sate_file_pos);
    }

    /* Epoch presence RLE. */
    wbuf_uleb128(&payload, sv->when_used - 1);
    prev_end = 0;
    for (ii = 0; ii < sv->when_used; ++ii)
    {
        int absent = sv->when[ii].start - prev_end;
        int present = sv->when[ii].end - sv->when[ii].start;
        wbuf_uleb128(&payload, absent);
        wbuf_uleb128(&payload, present - 1);
        prev_end = sv->when[ii].end;
    }

    offset = mm->used;
    write_chunk(mm, "SATE", payload.data, payload.used);
    wbuf_free(&payload);
    return offset;
}

/* ---- Write EPOC chunk ---- */

static size_t write_epoc_chunk(struct mmbuf *mm, const struct rinex_data *data)
{
    struct wbuf payload;
    size_t offset;
    int ii, span_start;
    int64_t interval_e7;

    wbuf_init(&payload);

    /* ULEB128 count of epochs. */
    wbuf_uleb128(&payload, data->epoch_used);

    if (data->epoch_used == 0)
    {
        offset = mm->used;
        write_chunk(mm, "EPOC", payload.data, payload.used);
        wbuf_free(&payload);
        return offset;
    }

    /* Build epoch spans. */
    span_start = 0;
    while (span_start < data->epoch_used)
    {
        const struct rinex_epoch *e0 = &data->epoch[span_start];
        int span_len = 1;
        uint64_t date, time;

        /* Determine interval from first two epochs in this span. */
        interval_e7 = 0;
        if (span_start + 1 < data->epoch_used)
        {
            const struct rinex_epoch *e1 = &data->epoch[span_start + 1];
            /* Compute interval in sec_e7 units. */
            interval_e7 = e1->sec_e7 - e0->sec_e7;
            if (e1->hh_mm != e0->hh_mm)
            {
                int dmin = (e1->hh_mm / 100 - e0->hh_mm / 100) * 60
                         + (e1->hh_mm % 100 - e0->hh_mm % 100);
                interval_e7 += (int64_t)dmin * 600000000;
            }
            if (e1->yyyy_mm_dd != e0->yyyy_mm_dd)
            {
                /* Day boundary: just make a 1-epoch span. */
                interval_e7 = 0;
            }
        }

        /* Extend the span as far as possible. */
        if (interval_e7 != 0)
        {
            while (span_start + span_len < data->epoch_used)
            {
                if (!same_span(&data->epoch[span_start + span_len - 1],
                    &data->epoch[span_start + span_len], interval_e7))
                    break;
                span_len++;
            }
        }

        /* Encode interval. */
        if (interval_e7 != 0 && (interval_e7 % 10000000) == 0)
            wbuf_sleb128(&payload, -(interval_e7 / 10000000));
        else
            wbuf_sleb128(&payload, interval_e7);

        /* count-minus-1 */
        wbuf_uleb128(&payload, span_len - 1);

        /* date: YYYYMMDD */
        date = e0->yyyy_mm_dd;
        wbuf_uleb128(&payload, date);

        /* time: HH*1e11 + MM*1e9 + SS_e7 */
        time = (uint64_t)(e0->hh_mm / 100) * 100000000000ULL
             + (uint64_t)(e0->hh_mm % 100) * 1000000000ULL
             + (uint64_t)e0->sec_e7;
        wbuf_uleb128(&payload, time);

        span_start += span_len;
    }

    /* RLE-encode receiver clock offsets.
     * Trailing zeros may be omitted.
     */
    {
        int last_nonzero = data->epoch_used;
        while (last_nonzero > 0
            && data->epoch[last_nonzero - 1].clock_offset == 0)
            last_nonzero--;

        ii = 0;
        while (ii < last_nonzero)
        {
            int64_t val = data->epoch[ii].clock_offset;
            int run = 1;
            while (ii + run < last_nonzero
                && data->epoch[ii + run].clock_offset == val)
                run++;
            wbuf_sleb128(&payload, val);
            wbuf_uleb128(&payload, run - 1);
            ii += run;
        }
    }

    offset = mm->used;
    write_chunk(mm, "EPOC", payload.data, payload.used);
    wbuf_free(&payload);
    return offset;
}

/* ---- Write SRNX header ---- */

/** Return a pointer to the start of the RINEX header within \a hdr.
 *
 * CRX (Hatanaka-compressed) files prepend CRINEX-specific lines
 * before the RINEX VERSION / TYPE line.  The SRNX RHDR chunk must
 * contain only the RINEX portion of the header (per the spec), so
 * callers skip any such preamble.
 */
static const char *rinex_header_start(const char *hdr, int len)
{
    const char *p;

    if (len >= 80 && !memcmp(hdr + 60, "RINEX VERSION / TYPE", 20))
        return hdr;

    for (p = memchr(hdr, '\n', (size_t)len); p != NULL; )
    {
        ++p;
        if (hdr + len - p >= 80 && !memcmp(p + 60, "RINEX VERSION / TYPE", 20))
            return p;
        p = memchr(p, '\n', (size_t)(hdr + len - p));
    }

    return hdr;
}

/** Writes the SRNX header chunk. Returns the offset (within \a mm) of
 * the SDIR-offset field for later patching.
 */
static size_t write_srnx_header(struct mmbuf *mm)
{
    char payload[SRNX_PAYLOAD_SIZE];
    struct wbuf tmp;
    size_t chunk_start, sdir_field_offset;
    char hdr[5];
    int hdr_len;

    memset(payload, 0, sizeof payload);

    /* Build payload into a fixed-size buffer.
     * Fields: major=1, minor=1 (0 with --socd), chunk_digest=<g_chunk_digest_id>,
     * file_digest=<g_file_digest_id>, sdir_offset=0 (patched later), padding.
     */
    wbuf_init(&tmp);
    wbuf_uleb128(&tmp, 1); /* major */
    wbuf_uleb128(&tmp, g_use_socd ? 0 : 1); /* minor: 1 when using SOC2 */
    wbuf_uleb128(&tmp, (uint64_t)g_chunk_digest_id); /* chunk digest */
    wbuf_uleb128(&tmp, (uint64_t)g_file_digest_id); /* file digest */
    /* sdir_offset starts here; leave as zeros for patching */
    if (tmp.used > SRNX_PAYLOAD_SIZE)
    {
        fail_and_exit("SRNX payload overflow");
    }
    memcpy(payload, tmp.data, tmp.used);

    /* Build FOURCC + ULEB128(16) header. */
    memcpy(hdr, "SRNX", 4);
    hdr[4] = (char)SRNX_PAYLOAD_SIZE; /* ULEB128(16) is a single byte 0x10 */
    hdr_len = 5;

    chunk_start = mm->used;
    mm_append(mm, hdr, hdr_len);
    /* sdir_field_offset points to where the sdir_offset ULEB128 will go:
     * the byte immediately after major/minor/chunk_digest/file_digest. */
    sdir_field_offset = chunk_start + hdr_len + tmp.used;
    mm_append(mm, payload, SRNX_PAYLOAD_SIZE);
    write_chunk_digest(mm, chunk_start);

    wbuf_free(&tmp);
    return sdir_field_offset;
}

/* ---- Write SDIR chunk ---- */

/* ---- Write EVTF chunks ---- */

/** Write one EVTF chunk per special event.
 * Returns the offset of the first EVTF chunk, or 0 if there are none.
 */
static size_t write_evtf_chunks(struct mmbuf *mm, const struct rinex_data *data)
{
    size_t first_offset = 0;
    int ii;

    for (ii = 0; ii < data->event_used; ++ii)
    {
        struct wbuf payload;
        size_t offset;

        wbuf_init(&payload);
        wbuf_uleb128(&payload, (uint64_t)data->event[ii].epoch_index);
        wbuf_append(&payload, data->event[ii].text, data->event[ii].text_len);

        offset = mm->used;
        write_chunk(mm, "EVTF", payload.data, payload.used);
        wbuf_free(&payload);

        if (ii == 0)
            first_offset = offset;
    }

    return first_offset;
}

static size_t write_sdir_chunk(struct mmbuf *mm, size_t epoc_offset,
    size_t evtf_offset, const struct sdir_entry *entries, int n_entries)
{
    struct wbuf payload;
    size_t offset;
    int ii;

    wbuf_init(&payload);
    wbuf_uleb128(&payload, epoc_offset);
    wbuf_uleb128(&payload, evtf_offset);

    for (ii = 0; ii < n_entries; ++ii)
    {
        wbuf_append(&payload, entries[ii].name, 3);
        wbuf_uleb128(&payload, entries[ii].sate_offset);
    }

    offset = mm->used;
    write_chunk(mm, "SDIR", payload.data, payload.used);
    wbuf_free(&payload);
    return offset;
}

/* ---- Patch SRNX header with SDIR offset ---- */

static void patch_srnx_sdir(struct mmbuf *mm, size_t sdir_field_offset,
    size_t sdir_offset)
{
    unsigned char buf[10];
    int len = 0;
    uint64_t val = sdir_offset;
    int dig_len;

    /* Encode ULEB128 into buf. */
    do {
        unsigned char ch = val & 127;
        val >>= 7;
        if (val)
            ch |= 128;
        buf[len++] = ch;
    } while (val);

    memcpy(mm->data + sdir_field_offset, buf, len);

    /* Recompute the SRNX chunk digest over FOURCC + ULEB128(16) + payload.
     * The SRNX chunk always starts at file offset 0, with a 5-byte header
     * ("SRNX" + one-byte 0x10 for ULEB128(16)) followed by a 16-byte
     * payload, so the digest bytes sit at file offset 21.
     */
    dig_len = rnx_digest_length(g_chunk_digest_id);
    if (dig_len > 0)
    {
        if (rnx_digest(g_chunk_digest_id,
                mm->data, 5 + SRNX_PAYLOAD_SIZE,
                mm->data + 5 + SRNX_PAYLOAD_SIZE) < 0)
        {
            fail_and_exit("Unsupported chunk digest id=%d",
                g_chunk_digest_id);
        }
    }
}

/* ---- Append file-level digest ---- */

/** Computes the file-level digest over bytes [start_offset, mm->used)
 * and appends the digest to \a mm.  Does nothing if the file-digest id
 * is null.
 */
static void append_file_digest(struct mmbuf *mm)
{
    int dig_len;

    dig_len = rnx_digest_length(g_file_digest_id);
    if (dig_len <= 0)
        return;
    mm_require(mm, (size_t)dig_len);
    if (rnx_digest(g_file_digest_id, mm->data, mm->used, mm->data + mm->used) < 0)
    {
        fail_and_exit("Unsupported file digest id=%d", g_file_digest_id);
    }
    mm->used += (size_t)dig_len;
}

/* ---- Main converter ---- */

static int rnx2srnx(const char input_name[], const char output_name[])
{
    struct rinex_data data;
    struct mmbuf mm = { NULL, 0, 0 };
    struct rnx2srnx_scratch scratch;
    struct stat st;
    size_t cap;
    int fd = -1;
    const char *err;
    size_t sdir_field_offset, epoc_offset, evtf_offset, sdir_offset;
    struct sdir_entry *sdir_entries = NULL;
    signed char glo_chan[100];
    int n_sdir = 0, sdir_alloc = 0;
    int sys_idx, status = 0;

    scratch_init(&scratch);
    g_output_name = output_name;

    /* Load the input file. */
    if (!strcmp(input_name, "-"))
    {
        struct rinex_stream *stream = rinex_stdin_stream();
        if (!stream)
        {
            fprintf(stderr, "Unable to open stdin\n");
            return 1;
        }
        err = rinex_load(stream, &data);
        stream->destroy(stream);
    }
    else
    {
        err = rinex_load_file(input_name, &data);
    }
    if (err)
    {
        fprintf(stderr, "Unable to load %s: %s\n", input_name, err);
        return 1;
    }

    /* Pick an upper bound for the output size.  SRNX is compressed
     * relative to RINEX, so input_size + a small margin is a safe
     * ceiling for all realistic observation files.
     */
    if (!strcmp(input_name, "-"))
    {
        cap = (size_t)(data.obs ? data.obs[0] : 0) * 16 + data.file_header_len + 65536;
    }
    else if (stat(input_name, &st) != 0)
    {
        fprintf(stderr, "Unable to stat %s\n", input_name);
        free_rinex_data(&data);
        return 1;
    }
    else
    {
        cap = (size_t)st.st_size + 65536;
    }
    if (cap < 262144)
        cap = 262144;

    /* EVTF preserves decoded text and can expand compressed input. */
    for (sys_idx = 0; sys_idx < data.event_used; ++sys_idx)
    {
        size_t extra = (size_t)data.event[sys_idx].text_len + 128;
        if (extra > SIZE_MAX - cap)
        {
            fprintf(stderr, "Exceptional records exceed output size limit\n");
            free_rinex_data(&data);
            return 1;
        }
        cap += extra;
    }

    /* Open and size the output file. */
    fd = open(output_name, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
    {
        fprintf(stderr, "Unable to create %s\n", output_name);
        free_rinex_data(&data);
        return 1;
    }
    if (ftruncate(fd, (off_t)cap) != 0)
    {
        fprintf(stderr, "ftruncate failed on %s\n", output_name);
        close(fd);
        unlink(output_name);
        free_rinex_data(&data);
        return 1;
    }
    mm.data = mmap(NULL, cap, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mm.data == MAP_FAILED)
    {
        fprintf(stderr, "mmap failed on %s\n", output_name);
        close(fd);
        unlink(output_name);
        free_rinex_data(&data);
        return 1;
    }
    mm.cap = cap;
    mm.used = 0;

    /* GLONASS frequency channels for SOC2 predictors. */
    soc2_parse_glo_chan(data.file_header, data.file_header_len, glo_chan);

    /* 1. Write SRNX header (with placeholder SDIR offset). */
    sdir_field_offset = write_srnx_header(&mm);

    /* 2. Write RHDR chunk (RINEX header only; skip any CRX preamble). */
    {
        const char *rhdr_start = rinex_header_start(data.file_header,
                                                     data.file_header_len);
        int rhdr_len = data.file_header_len
                       - (int)(rhdr_start - data.file_header);
        write_chunk(&mm, "RHDR", rhdr_start, (size_t)rhdr_len);
    }

    /* 3. Write EPOC chunk. */
    epoc_offset = write_epoc_chunk(&mm, &data);

    /* 4. Write EVTF chunks (special event records), if any. */
    evtf_offset = write_evtf_chunks(&mm, &data);

    /* 5. For each satellite, write SOCD chunks then SATE chunk. */
    for (sys_idx = 0; sys_idx < 32; ++sys_idx)
    {
        struct rinex_system_data *p_sys = &data.sys[sys_idx];
        int sv_idx, n_obs;

        if (p_sys->n_obs <= 0)
            continue;
        n_obs = p_sys->n_obs;

        for (sv_idx = p_sys->sv.start; sv_idx < p_sys->sv.end; ++sv_idx)
        {
            struct rinex_satellite_data *p_sv = data.sv[sv_idx];
            size_t *socd_offsets;
            size_t sate_pos, sate_offset;
            int jj;

            if (!p_sv || p_sv->obs_used <= 0)
                continue;

            /* Write SOCD chunks for each signal. */
            socd_offsets = calloc(n_obs, sizeof *socd_offsets);
            if (!socd_offsets)
            {
                fprintf(stderr, "Out of memory\n");
                status = 1; goto done;
            }
            if (g_use_socd)
            {
                for (jj = 0; jj < n_obs; ++jj)
                    socd_offsets[jj] = write_socd_chunk(&mm, &scratch, &data,
                        p_sv, jj, p_sys, input_name);
            }
            else
            {
                struct soc2_plan *plans;
                int64_t *t7;
                int pass;

                plans = calloc((size_t)n_obs, sizeof *plans);
                t7 = calloc((size_t)p_sv->obs_used, sizeof *t7);
                if (!plans || !t7
                    || soc2_plan_sv(&data, p_sv, p_sys, glo_chan, t7, plans) != 0)
                {
                    free(plans);
                    free(t7);
                    free(socd_offsets);
                    fprintf(stderr, "Out of memory\n");
                    status = 1; goto done;
                }
                /* Write referenced chunks first: unpredicted signals (the
                 * reference phases), then predicted carrier phases, then
                 * everything predicted from carrier phase. */
                for (pass = 0; pass < 3; ++pass)
                {
                    for (jj = 0; jj < n_obs; ++jj)
                    {
                        int kind = plans[jj].pred.kind;
                        int phase = p_sys->obs[jj][0] == 'L';
                        if ((pass == 0) != (kind == SRNX_PRED_NONE)
                            || (pass == 1 && !phase) || (pass == 2 && phase))
                            continue;
                        socd_offsets[jj] = write_soc2_chunk(&mm, &scratch,
                            &data, p_sv, jj, p_sys, &plans[jj], t7, input_name);
                    }
                }
                free(plans);
                free(t7);
            }

            /* Write SATE chunk. */
            sate_pos = mm.used;
            sate_offset = write_sate_chunk(&mm, p_sv, n_obs,
                socd_offsets, sate_pos);
            free(socd_offsets);

            /* Record for SDIR. */
            if (n_sdir >= sdir_alloc)
            {
                sdir_alloc = sdir_alloc ? sdir_alloc * 2 : 64;
                sdir_entries = realloc(sdir_entries,
                    sdir_alloc * sizeof *sdir_entries);
                if (!sdir_entries)
                {
                    fprintf(stderr, "Out of memory\n");
                    status = 1; goto done;
                }
            }
            memcpy(sdir_entries[n_sdir].name, p_sv->id, 4);
            sdir_entries[n_sdir].sate_offset = sate_offset;
            n_sdir++;
        }
    }

    /* 6. Write SDIR chunk. */
    sdir_offset = write_sdir_chunk(&mm, epoc_offset, evtf_offset,
        sdir_entries, n_sdir);

    /* 7. Patch SRNX header with SDIR offset. */
    patch_srnx_sdir(&mm, sdir_field_offset, sdir_offset);

    /* 8. Compute and append the file-level digest, if enabled. */
    append_file_digest(&mm);

done:
    g_output_name = NULL;
    if (mm.data && mm.data != MAP_FAILED)
    {
        msync(mm.data, mm.used, MS_SYNC);
        munmap(mm.data, mm.cap);
    }
    if (fd >= 0)
    {
        if (status != 0)
        {
            close(fd);
            unlink(output_name);
        }
        else
        {
            if (ftruncate(fd, (off_t)mm.used) != 0)
                fprintf(stderr, "ftruncate-to-used failed on %s\n", output_name);
            close(fd);
        }
    }
    free(sdir_entries);
    scratch_free(&scratch);
    free_rinex_data(&data);
    return status;
}

/* ---- Filename helpers ---- */

static int is_rinex_file_name(const char name[], size_t len)
{
    const char *end_name;

    if (len < 5)
        return 0;
    end_name = name + len;
    if (end_name[-4] != '.')
        return 0;
    if (!memcmp(end_name - 3, "rnx", 3))
        return 3;
    if ((end_name[-1] == 'o' || end_name[-1] == 'd') && isdigit(end_name[-2]) && isdigit(end_name[-3]))
        return 2;
    return 0;
}

/** Runs `rnxcmp <input> <output>` as a subprocess to verify that \a output
 * (the just-written SRNX file) round-trips to the same observations as
 * \a input.  rnxcmp's stdout/stderr are inherited so its output (if any)
 * is visible directly.
 *
 * \returns Zero if rnxcmp reports the files match, -1 otherwise.
 */
static int run_check(const char *input, const char *output)
{
    pid_t pid;
    int status;

    pid = fork();
    if (pid < 0)
    {
        fprintf(stderr, "rnx2srnx: fork failed: %s\n", strerror(errno));
        return -1;
    }
    if (pid == 0)
    {
        execlp("rnxcmp", "rnxcmp", input, output, (char *)NULL);
        fprintf(stderr, "rnx2srnx: exec rnxcmp failed: %s\n", strerror(errno));
        _exit(127);
    }

    if (waitpid(pid, &status, 0) < 0)
    {
        fprintf(stderr, "rnx2srnx: waitpid failed: %s\n", strerror(errno));
        return -1;
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return 0;

    fprintf(stderr, "rnx2srnx: round-trip check failed\n");
    return -1;
}

static void usage(FILE *out, const char *argv0)
{
    fprintf(out, "Usage: %s [--chunk-digest=<id>] [--file-digest=<id>]\n"
        "          [--max-sleb-run=<N>] [--socd] [--order-search=scan|guess] [--check]\n"
        "          [--] <input.rnx|-> [output.srnx]\n"
        "'-' as input reads from stdin and requires an explicit output name.\n"
        "--socd writes revision 1.0 SOCD chunks instead of SOC2.\n"
        "--order-search chooses how SOC2 delta orders are searched (default: guess).\n",
        argv0);
}

int main(int argc, char *argv[])
{
    const char *input_name = NULL;
    const char *output_arg = NULL;
    char *output_name;
    size_t name_len;
    int ii, do_check = 0;

    for (ii = 1; ii < argc; ++ii)
    {
        const char *arg = argv[ii];
        if (parse_digest_flag(arg, "--chunk-digest=", &g_chunk_digest_id))
            continue;
        if (parse_digest_flag(arg, "--file-digest=", &g_file_digest_id))
            continue;
        if (!strncmp(arg, "--max-sleb-run=", 15))
        {
            char *end;
            long v = strtol(arg + 15, &end, 10);
            if (*end != '\0' || v < 1 || v > INT_MAX)
            {
                fprintf(stderr, "Invalid --max-sleb-run value '%s'\n", arg + 15);
                return EXIT_FAILURE;
            }
            g_max_sleb_run = (int)v;
            continue;
        }
        if (!strcmp(arg, "--check"))
        {
            do_check = 1;
            continue;
        }
        if (!strcmp(arg, "--socd"))
        {
            g_use_socd = 1;
            continue;
        }
        if (parse_order_search(arg))
            continue;
        if (!strcmp(arg, "--help"))
        {
            usage(stdout, argv[0]);
            return EXIT_SUCCESS;
        }
        if (arg[0] == '-' && arg[1] == '-' && arg[2] == '\0')
        {
            /* "--" ends option parsing. */
            ii++;
            break;
        }
        if (arg[0] == '-' && arg[1] != '\0')
        {
            fprintf(stderr, "Unknown option '%s'\n", arg);
            return EXIT_FAILURE;
        }
        if (!input_name)
            input_name = arg;
        else if (!output_arg)
            output_arg = arg;
        else
        {
            fprintf(stderr, "Too many positional arguments\n");
            return EXIT_FAILURE;
        }
    }

    if (!input_name)
    {
        usage(stderr, argv[0]);
        return EXIT_FAILURE;
    }

    output_name = output_arg ? strdup(output_arg) : NULL;
    if (!strcmp(input_name, "-") && !output_name)
    {
        fprintf(stderr, "Output file name required when reading from stdin\n");
        return EXIT_FAILURE;
    }
    name_len = strlen(input_name);
    if (is_rinex_file_name(input_name, name_len))
    {
        if (!output_name)
        {
            output_name = malloc(name_len + 2);
            memcpy(output_name, input_name, name_len - 4);
            strcpy(output_name + name_len - 4, ".srnx");
        }
    }
    else
    {
        if (!output_name)
        {
            output_name = malloc(name_len + 6);
            memcpy(output_name, input_name, name_len);
            strcpy(output_name + name_len, ".srnx");
        }
    }

    if (rnx2srnx(input_name, output_name) != 0)
    {
        free(output_name);
        return EXIT_FAILURE;
    }

    if (do_check && strcmp(input_name, "-") != 0)
    {
        if (run_check(input_name, output_name))
        {
            free(output_name);
            return EXIT_FAILURE;
        }
    }

    free(output_name);
    return EXIT_SUCCESS;
}

#else /* defined(RNX2SRNX_SEARCH) */
/* ============================================================
 * rnx2srnx_search: byte-cost estimator.  Writes no SRNX bytes.
 * ============================================================
 */

/* Runtime block-size tuple used by the DP and the byte-cost accounting.
 * MUST be sorted ascending, all multiples of 8, all in [8, g_max_block].
 * Caller is expected to include 8 (the DP relies on at least one small
 * size for fallback runs of len 1..7).  The format's header byte allows
 * only RNX_N_BLOCK_SIZES sizes; longer tuples are for lower bounds and
 * for backward elimination from a large candidate set. */
#define MAX_TUPLE_LEN  64
static int g_cand[MAX_TUPLE_LEN];
static int g_n_cand;

/* Nonzero (--fixed-order) to choose each column's delta order once,
 * under the first tuple evaluated, and reuse it for later tuples. */
static int g_fixed_order;

/* Nonzero (--soc2) to cost SOC2 chunks (cross-signal prediction, SSI
 * references and split-Rice blocks) instead of SOCD chunks. */
static int g_soc2;

/* Upper bound on candidate block sizes, set via --max-block=N.  Must be
 * a power of 2, >= 8.  Bounds the sparse-table levels computed below. */
static int g_max_block = 512;

/* Number of sparse-table levels needed to cover windows up to
 * g_max_block; computed once from g_max_block after argument parsing. */
static int g_max_levels = 10;

static int compute_max_levels(int max_block)
{
    int levels = 1;
    while ((1 << levels) < max_block)
        levels++;
    return levels + 1;  /* +1 for safety margin */
}

/* ---- Counting-only buffer ----
 *
 * The encoder paths below all funnel through one of these.  We do not
 * actually emit byte values; we only sum byte counts.  This lets us
 * support arbitrary 8*N block sizes without worrying about transpose
 * buffers, header byte slot indices, etc.
 */

struct cbuf
{
    size_t used;
};

static void cbuf_init(struct cbuf *wb) { wb->used = 0; }

static void cbuf_byte(struct cbuf *wb)
{
    wb->used++;
}

static void cbuf_skip(struct cbuf *wb, size_t len)
{
    wb->used += len;
}

static void cbuf_uleb128(struct cbuf *wb, uint64_t val)
{
    wb->used += uleb128_len(val);
}

static void cbuf_sleb128(struct cbuf *wb, int64_t val)
{
    wb->used += sleb128_len(val);
}

/* ---- DP machinery ---- */

/* Scratch carries scratch buffers plus a sparse table of window-maxes
 * over the per-order bit-width arrays.  Each call to dp_core() builds
 * the sparse table for the bw[] array it receives. */
struct scratch
{
    size_t   cap;            /**< capacity in elements */
    int64_t *delta_matrix;   /**< 8 * cap */
    int64_t *full_scaled;    /**< cap */
    int64_t *present_scaled; /**< cap */
    int64_t *dp;             /**< cap + 1 */
    uint8_t *spt;            /**< g_max_levels * cap : window-max sparse table */
    uint8_t *all_bw;         /**< 8 * cap : bw for all 8 delta orders */
    uint32_t *psum_arr;      /**< cap + 1 : prefix sums of sleb[] within dp_core */
    struct block_choice *choices_a;
    struct block_choice *choices_b;
    uint32_t *rice_q;        /**< (RNX_RICE_MAX_R + 1) * (cap + 1), --soc2 only : prefix sums of q+1 per Rice parameter */
    uint64_t *usum;          /**< cap + 1, --soc2 only : prefix sums of capped zigzag values */
};

static void *xrealloc(void *p, size_t bytes)
{
    void *q = realloc(p, bytes);
    if (!q)
    {
        fprintf(stderr, "Out of memory\n");
        exit(EXIT_FAILURE);
    }
    return q;
}

static void scratch_init(struct scratch *s) { memset(s, 0, sizeof *s); }

static void scratch_free(struct scratch *s)
{
    free(s->delta_matrix);
    free(s->full_scaled);
    free(s->present_scaled);
    free(s->dp);
    free(s->spt);
    free(s->all_bw);
    free(s->psum_arr);
    free(s->choices_a);
    free(s->choices_b);
    free(s->rice_q);
    free(s->usum);
    memset(s, 0, sizeof *s);
}

static void scratch_reserve(struct scratch *s, size_t need)
{
    size_t cap;
    if (need <= s->cap)
        return;
    cap = s->cap ? s->cap : 2880;
    while (cap < need)
        cap *= 2;
    s->delta_matrix   = xrealloc(s->delta_matrix, 8 * cap * sizeof *s->delta_matrix);
    s->full_scaled    = xrealloc(s->full_scaled,      cap * sizeof *s->full_scaled);
    s->present_scaled = xrealloc(s->present_scaled,   cap * sizeof *s->present_scaled);
    s->dp             = xrealloc(s->dp,         (cap + 1) * sizeof *s->dp);
    s->spt            = xrealloc(s->spt, (size_t)g_max_levels * cap * sizeof *s->spt);
    s->all_bw         = xrealloc(s->all_bw,       8 * cap * sizeof *s->all_bw);
    s->psum_arr       = xrealloc(s->psum_arr,   (cap + 1) * sizeof *s->psum_arr);
    s->choices_a      = xrealloc(s->choices_a,        cap * sizeof *s->choices_a);
    s->choices_b      = xrealloc(s->choices_b,        cap * sizeof *s->choices_b);
    if (g_soc2)
    {
        s->rice_q = xrealloc(s->rice_q,
            (size_t)(RNX_RICE_MAX_R + 1) * (cap + 1) * sizeof *s->rice_q);
        s->usum   = xrealloc(s->usum, (cap + 1) * sizeof *s->usum);
    }
    s->cap = cap;
}

/* Build sparse table from bw[0..count-1] into scratch->spt.
 * spt[k * cap + ii] = max(bw[ii], bw[ii+1], ..., bw[ii + (1<<k) - 1]).
 * Built for k in [0, levels), where levels covers up to the largest m
 * in g_cand[]. */
static int build_spt(struct scratch *scratch, const uint8_t *bw, int count)
{
    uint8_t *spt = scratch->spt;
    size_t cap = scratch->cap;
    int k, max_m = g_cand[g_n_cand - 1];
    int levels;

    /* Lowest power-of-2 >= max_m's log; we always query at k where
     * (1<<k) <= max_m, so log2_floor(max_m) levels are enough plus 1.
     * Always build at least k=0. */
    levels = 1;
    while ((1 << levels) <= max_m)
        ++levels;
    if (levels > g_max_levels)
        levels = g_max_levels;

    /* k = 0: single-element windows */
    memcpy(spt, bw, (size_t)count);
    for (k = 1; k < levels; ++k)
    {
        int half = 1 << (k - 1);
        int win = 1 << k;
        uint8_t *prev = spt + (size_t)(k - 1) * cap;
        uint8_t *cur  = spt + (size_t)k * cap;
        int ii;
        if (count < win)
        {
            /* Window of size win does not fit anywhere; skip; later
             * levels cannot be queried either. */
            return levels;
        }
        for (ii = 0; ii + win <= count; ++ii)
        {
            uint8_t a = prev[ii];
            uint8_t b = prev[ii + half];
            cur[ii] = a > b ? a : b;
        }
    }
    return levels;
}

/* Query window-max from sparse table for window [ii, ii+m). m must be > 0
 * and ii+m must be <= count.  k is the largest power of 2 with (1<<k) <= m. */
static inline uint8_t spt_query(const uint8_t *spt, size_t cap, int ii, int m)
{
    int k = 31 - __builtin_clz((unsigned int)m);
    const uint8_t *level = spt + (size_t)k * cap;
    uint8_t a = level[ii];
    uint8_t b = level[ii + m - (1 << k)];
    return a > b ? a : b;
}

/** Core DP: walk right-to-left choosing the best block edge per position.
 *
 * \a bw is the precomputed bit-width array for the delta order under test.
 * \a vals is needed only for zero-run detection.  \a choices, when non-NULL,
 * is populated with the optimal edge at each position. */
static int64_t dp_core(struct scratch *scratch,
    const int64_t *vals, const uint8_t *bw, int count, struct block_choice *choices)
{
    uint32_t *psum;
    int64_t *dp;
    int64_t cost;
    int ii, ci, zlen_here, zlen_next, max_L;
    int n_cand = g_n_cand;
    const int *cand = g_cand;
    int last_fit;

    dp = scratch->dp;
    psum = scratch->psum_arr;

    build_spt(scratch, bw, count);

    /* Prefix sums of SLEB128 byte lengths for the SLEB128-run fallback. */
    psum[0] = 0;
    for (ii = 0; ii < count; ++ii)
        psum[ii + 1] = psum[ii] + sleb128_len_from_bw[bw[ii] > 64 ? 64 : bw[ii]];

    dp[count] = 0;
    zlen_next = 0;

    for (ii = count - 1; ii >= 0; --ii)
    {
        int remaining = count - ii, sleb_min_L;
        int64_t best = INT64_MAX, sleb_min;
        struct block_choice best_choice = {BK_SLEB, 0, 1};

        /* Zero-run transition. */
        zlen_here = (vals[ii] == 0) ? zlen_next + 1 : 0;
        zlen_next = zlen_here;
        if (zlen_here > 0)
        {
            cost = 1 + uleb128_len((uint64_t)(zlen_here - 1)) + dp[ii + zlen_here];
            if (cost < best)
            {
                best = cost;
                best_choice.kind = BK_ZERO;
                best_choice.bw = 0;
                best_choice.len = zlen_here;
            }
        }

        /* Block transitions for each candidate size. */
        last_fit = -1;
        for (ci = 0; ci < n_cand; ++ci)
        {
            int m = cand[ci];
            int max_bw;

            if (remaining < m)
                break;
            last_fit = ci;
            max_bw = spt_query(scratch->spt, scratch->cap, ii, m);
            if (max_bw > 32)
                continue;
            cost = 1 + (int64_t)max_bw * m / 8 + dp[ii + m];
            if (cost < best)
            {
                best = cost;
                best_choice.kind = BK_BLOCK;
                best_choice.bw = (short)max_bw;
                best_choice.len = m;
            }
        }
        (void)last_fit;

        /* SLEB128-run transition. */
        max_L = remaining < g_max_sleb_run ? remaining : g_max_sleb_run;
        sleb_min = sleb_run_best(dp, psum, ii, max_L, &sleb_min_L);
        if (sleb_min < best)
        {
            best = sleb_min;
            best_choice.kind = BK_SLEB;
            best_choice.bw = 0;
            best_choice.len = sleb_min_L;
        }

        dp[ii] = best;
        if (choices)
            choices[ii] = best_choice;
    }

    return dp[0];
}

/** SOC2 split-Rice DP over the runtime tuple; see rice_dp().  Requires
 * scratch reserved for at least \a count values. */
static int64_t dp_core_rice(struct scratch *scratch, const int64_t *vals,
    int count)
{
    struct rice_scratch rs;

    rs.dp = scratch->dp;
    rs.psum = scratch->psum_arr;
    rs.usum = scratch->usum;
    rs.rice_q = scratch->rice_q;
    return rice_dp(&rs, vals, count, g_cand, g_n_cand, NULL);
}

/* Context for select_delta_order_rice()'s order evaluations. */
struct rice_order_ctx
{
    struct scratch *scratch;
    const int64_t *delta_matrix;
    int count;
};

static int64_t rice_order_eval(void *ctx, int ord)
{
    struct rice_order_ctx *oc = ctx;
    return dp_core_rice(oc->scratch, oc->delta_matrix + (size_t)ord * oc->count,
        oc->count);
}

/** Select the delta order for the split-Rice coding of the whole
 * \a count-value sequence whose delta matrix (stride \a count) is
 * \a delta_matrix, per --order-search.  \a *out_best_dp_cost receives the
 * DP-only cost for the chosen order. */
static int select_delta_order_rice(struct scratch *scratch,
    const int64_t *delta_matrix, int count, int64_t *out_best_dp_cost)
{
    struct rice_order_ctx oc = { scratch, delta_matrix, count };
    int64_t cost;
    int order = rice_select_order(delta_matrix, count, rice_order_eval, NULL,
        &oc, &cost);
    if (out_best_dp_cost)
        *out_best_dp_cost = cost;
    return order;
}

/* Precompute bit-widths for all 8 delta orders into scratch->all_bw
 * (stride = scratch->cap). */
static void precompute_all_bw(struct scratch *scratch,
    const int64_t *delta_matrix, int count)
{
    int ord, ii;
    for (ord = 0; ord < 8; ++ord)
    {
        const int64_t *vals = delta_matrix + (size_t)ord * count;
        uint8_t *bw = scratch->all_bw + (size_t)ord * scratch->cap;
        for (ii = 0; ii < count; ++ii)
            bw[ii] = twos_comp_bw(vals[ii]);
    }
}

/** Run dp_core for one delta order over a (sub)range, return optimal cost. */
static int64_t dp_for_order_range(struct scratch *scratch,
    const int64_t *delta_matrix, int present_count, int order,
    int start, int run_len)
{
    const int64_t *vals;
    uint8_t *bw;

    if (run_len <= 0)
        return 0;
    vals = delta_matrix + (size_t)order * present_count + start;
    bw   = scratch->all_bw + (size_t)order * scratch->cap + start;
    return dp_core(scratch, vals, bw, run_len, NULL);
}

/** Select the delta order minimizing total cost over the *whole* present
 * sequence; same logic as rnx2srnx's select_delta_order.
 *
 * \a *out_best_dp_cost receives the dp_core-only byte cost for the chosen
 * order (i.e. excluding the `ord` init bytes); callers add those separately. */
static int select_delta_order(struct scratch *scratch,
    const int64_t *delta_matrix, int count, int64_t *out_best_dp_cost)
{
    int best_order = 0;
    int64_t best_dp;
    int64_t best_total;
    int ord, no_improve;

    best_dp = dp_for_order_range(scratch, delta_matrix, count, 0, 0, count);
    best_total = best_dp; /* order 0: no init bytes */
    no_improve = 0;

    for (ord = 1; ord <= 7 && ord < count; ++ord)
    {
        int64_t dp_cost = dp_for_order_range(scratch, delta_matrix, count,
            ord, 0, count);
        int64_t total = (int64_t)ord + dp_cost;

        if (total < best_total)
        {
            best_total = total;
            best_dp = dp_cost;
            best_order = ord;
            no_improve = 0;
        }
        else if (++no_improve >= 2)
        {
            break;
        }
    }

    if (out_best_dp_cost)
        *out_best_dp_cost = best_dp;
    return best_order;
}

/* ---- Indicator RLE byte count ---- */

static size_t rle_indicator_bytes(const char *ind, int count)
{
    int ii;
    size_t rle_used = 0;

    while (count > 0 && ind[count - 1] == ' ')
        count--;

    ii = 0;
    while (ii < count)
    {
        char cur = ind[ii];
        int run_start = ii;
        int idx = indicator_idx((unsigned char)cur);
        while (ii < count && ind[ii] == cur)
            ii++;
        /* Packed encoding: single ULEB128 = (count-1)*11 + idx, matching
         * rle_encode_indicators() in rnx2srnx.c. */
        rle_used += (size_t)uleb128_len(
            (uint64_t)(ii - run_start - 1) * 11 + (uint64_t)idx);
    }

    /* The outer wbuf prepends ULEB128(rle_used) before the RLE bytes. */
    return (size_t)uleb128_len((uint64_t)rle_used) + rle_used;
}

/* ---- Per-(satellite, signal) SOCD payload cost ----
 *
 * Computes the byte length of the SOCD payload (the content after
 * FOURCC + ULEB128(payload_len)) for one (satellite, signal) pair under
 * the current g_cand tuple. */
static size_t compute_socd_payload_bytes(struct scratch *scratch,
    const struct rinex_data *data,
    const struct rinex_satellite_data *sv, int sig_idx,
    const struct rinex_system_data *p_sys)
{
    const int64_t *obs_arr;
    const char *lli_arr, *ssi_arr;
    int obs_count, ii;
    int64_t obs_gcd;
    size_t payload_bytes = 0;
    size_t packed_bytes = 0;

    if (sv->start[sig_idx] < 0)
        return 0; /* signal never observed */

    obs_arr = data->obs + sv->start[sig_idx];
    lli_arr = data->lli + sv->start[sig_idx];
    ssi_arr = data->ssi + sv->start[sig_idx];
    obs_count = sv->obs_used;

    /* obs_name: 8 bytes (3 + NUL + 3 + NUL implicit; written as 8 raw). */
    payload_bytes += 8;
    /* count-minus-1 */
    payload_bytes += (size_t)uleb128_len((uint64_t)(obs_count - 1));
    /* lli + ssi RLEs (each is ULEB128(len) + bytes). */
    payload_bytes += rle_indicator_bytes(lli_arr, obs_count);
    payload_bytes += rle_indicator_bytes(ssi_arr, obs_count);

    /* GCD of non-missing observation values. */
    obs_gcd = 0;
    for (ii = 0; ii < obs_count; ++ii)
        if (obs_arr[ii] != INT64_MIN)
            obs_gcd = gcd(obs_gcd, obs_arr[ii]);
    if (obs_gcd <= 0 || obs_gcd > 1000000)
        obs_gcd = 1;

    {
        int64_t *full_scaled;
        int64_t *present_scaled;
        int64_t *delta_matrix;
        int present_count = 0;
        int order = 0;
        int64_t whole_run_cost = 0;
        int single_run; /* 1 iff every obs is present (no INT64_MIN gaps) */

        scratch_reserve(scratch, (size_t)obs_count);
        full_scaled    = scratch->full_scaled;
        present_scaled = scratch->present_scaled;
        delta_matrix   = scratch->delta_matrix;

        for (ii = 0; ii < obs_count; ++ii)
        {
            if (obs_arr[ii] == INT64_MIN)
            {
                full_scaled[ii] = INT64_MIN;
            }
            else
            {
                full_scaled[ii] = obs_arr[ii] / obs_gcd;
                present_scaled[present_count++] = full_scaled[ii];
            }
        }

        single_run = (present_count == obs_count);

        if (present_count > 0)
        {
            compute_delta_matrix(delta_matrix, present_scaled, present_count);
            precompute_all_bw(scratch, delta_matrix, present_count);
            order = select_delta_order(scratch, delta_matrix, present_count,
                &whole_run_cost);
        }

        /* Order byte (ULEB128 of order or order+8 when gcd > 1). */
        if (obs_gcd > 1)
        {
            packed_bytes += (size_t)uleb128_len((uint64_t)(order + 8));
            packed_bytes += (size_t)uleb128_len((uint64_t)obs_gcd);
        }
        else
        {
            packed_bytes += (size_t)uleb128_len((uint64_t)order);
        }
        /* SLEB128(0) init values: 1 byte each. */
        packed_bytes += (size_t)order;

        /* Walk full_scaled, interleaving absent runs (0xFD) and present
         * delta runs whose cost is the DP result on that slice. */
        if (present_count == 0)
        {
            /* All absent: one 0xFD run covers everything. */
            packed_bytes += 1 + (size_t)uleb128_len((uint64_t)(obs_count - 1));
        }
        else if (single_run)
        {
            /* Optimization: the whole-stream DP cost is exact. */
            packed_bytes += (size_t)whole_run_cost;
        }
        else
        {
            int pi = 0;
            ii = 0;
            while (ii < obs_count)
            {
                if (full_scaled[ii] == INT64_MIN)
                {
                    int absent_len = 0;
                    while (ii + absent_len < obs_count
                        && full_scaled[ii + absent_len] == INT64_MIN)
                        absent_len++;
                    packed_bytes += 1 + (size_t)uleb128_len(
                        (uint64_t)(absent_len - 1));
                    ii += absent_len;
                }
                else
                {
                    int run_end = ii;
                    int run_len;
                    int64_t run_cost;
                    while (run_end < obs_count
                        && full_scaled[run_end] != INT64_MIN)
                        run_end++;
                    run_len = run_end - ii;
                    run_cost = dp_for_order_range(scratch, delta_matrix,
                        present_count, order, pi, run_len);
                    packed_bytes += (size_t)run_cost;
                    pi += run_len;
                    ii = run_end;
                }
            }
        }
    }

    /* packed[] is prefixed by ULEB128(packed_bytes) before being appended
     * to payload[]. */
    payload_bytes += (size_t)uleb128_len((uint64_t)packed_bytes);
    payload_bytes += packed_bytes;
    return payload_bytes;
}

/* ---- Tuple-independent chunk sizes ----
 *
 * SRNX header, RHDR, EPOC, EVTF, and each SATE / SDIR depend on data
 * layout that is independent of g_cand once SOCD sizes are known.  Below
 * we precompute the parts that don't change across tuples (per-(sv, sig)
 * presence info, EPOC payload size, EVTF payload sizes, file header) and
 * keep them in a context struct. */

/* RHDR payload byte count: strip any CRX preamble. */
static size_t rhdr_payload_bytes(const struct rinex_data *data)
{
    const char *hdr = data->file_header;
    int len = data->file_header_len;
    const char *start = hdr;
    const char *p;

    if (len >= 80 && !memcmp(hdr + 60, "RINEX VERSION / TYPE", 20))
        return (size_t)len;

    for (p = memchr(hdr, '\n', (size_t)len); p != NULL; )
    {
        ++p;
        if (hdr + len - p >= 80 && !memcmp(p + 60, "RINEX VERSION / TYPE", 20))
        {
            start = p;
            break;
        }
        p = memchr(p, '\n', (size_t)(hdr + len - p));
    }
    return (size_t)(len - (int)(start - hdr));
}

/* Replicate EPOC byte accounting from rnx2srnx, ignoring tuple choice. */
static size_t epoc_payload_bytes(const struct rinex_data *data)
{
    struct cbuf wb;
    int ii, span_start;
    int64_t interval_e7;

    cbuf_init(&wb);
    cbuf_uleb128(&wb, (uint64_t)data->epoch_used);

    if (data->epoch_used == 0)
        return wb.used;

    span_start = 0;
    while (span_start < data->epoch_used)
    {
        const struct rinex_epoch *e0 = &data->epoch[span_start];
        int span_len = 1;
        uint64_t date, time;

        interval_e7 = 0;
        if (span_start + 1 < data->epoch_used)
        {
            const struct rinex_epoch *e1 = &data->epoch[span_start + 1];
            interval_e7 = e1->sec_e7 - e0->sec_e7;
            if (e1->hh_mm != e0->hh_mm)
            {
                int dmin = (e1->hh_mm / 100 - e0->hh_mm / 100) * 60
                         + (e1->hh_mm % 100 - e0->hh_mm % 100);
                interval_e7 += (int64_t)dmin * 600000000;
            }
            if (e1->yyyy_mm_dd != e0->yyyy_mm_dd)
                interval_e7 = 0;
        }

        if (interval_e7 != 0)
        {
            while (span_start + span_len < data->epoch_used)
            {
                if (!same_span(&data->epoch[span_start + span_len - 1],
                    &data->epoch[span_start + span_len], interval_e7))
                    break;
                span_len++;
            }
        }

        if (interval_e7 != 0 && (interval_e7 % 10000000) == 0)
            cbuf_sleb128(&wb, -(interval_e7 / 10000000));
        else
            cbuf_sleb128(&wb, interval_e7);
        cbuf_uleb128(&wb, (uint64_t)(span_len - 1));

        date = e0->yyyy_mm_dd;
        cbuf_uleb128(&wb, date);
        time = (uint64_t)(e0->hh_mm / 100) * 100000000000ULL
             + (uint64_t)(e0->hh_mm % 100) * 1000000000ULL
             + (uint64_t)e0->sec_e7;
        cbuf_uleb128(&wb, time);

        span_start += span_len;
    }

    {
        int last_nonzero = data->epoch_used;
        while (last_nonzero > 0
            && data->epoch[last_nonzero - 1].clock_offset == 0)
            last_nonzero--;
        ii = 0;
        while (ii < last_nonzero)
        {
            int64_t val = data->epoch[ii].clock_offset;
            int run = 1;
            while (ii + run < last_nonzero
                && data->epoch[ii + run].clock_offset == val)
                run++;
            cbuf_sleb128(&wb, val);
            cbuf_uleb128(&wb, (uint64_t)(run - 1));
            ii += run;
        }
    }
    return wb.used;
}

/* Chunk overhead: FOURCC(4) + ULEB128(payload_len) + payload_len + digest. */
static size_t chunk_on_disk(size_t payload_len, int chunk_digest_len)
{
    size_t n = 4;
    n += (size_t)uleb128_len((uint64_t)payload_len);
    n += payload_len;
    if (chunk_digest_len > 0)
        n += (size_t)chunk_digest_len;
    return n;
}

/* ---- SATE payload byte count ----
 *
 * Given the SATE chunk's file position and the array of SOCD chunk start
 * offsets (or 0 for never-observed signals), compute the SATE payload's
 * byte length. */
static size_t sate_payload_bytes(const struct rinex_satellite_data *sv,
    int n_obs, const size_t *socd_offsets, size_t sate_file_pos)
{
    struct cbuf wb;
    int ii, prev_end;

    cbuf_init(&wb);
    cbuf_skip(&wb, 4); /* satellite name + '\0' */

    for (ii = 0; ii < n_obs; ++ii)
    {
        if (socd_offsets[ii] == 0)
            cbuf_sleb128(&wb, 0);
        else
            cbuf_sleb128(&wb,
                (int64_t)socd_offsets[ii] - (int64_t)sate_file_pos);
    }

    cbuf_uleb128(&wb, (uint64_t)(sv->when_used - 1));
    prev_end = 0;
    for (ii = 0; ii < sv->when_used; ++ii)
    {
        int absent = sv->when[ii].start - prev_end;
        int present = sv->when[ii].end - sv->when[ii].start;
        cbuf_uleb128(&wb, (uint64_t)absent);
        cbuf_uleb128(&wb, (uint64_t)(present - 1));
        prev_end = sv->when[ii].end;
    }
    return wb.used;
}

/* ---- SDIR payload byte count ---- */

static size_t sdir_payload_bytes(size_t epoc_offset, size_t evtf_offset,
    const struct sdir_entry *entries, int n_entries)
{
    struct cbuf wb;
    int ii;
    cbuf_init(&wb);
    cbuf_uleb128(&wb, (uint64_t)epoc_offset);
    cbuf_uleb128(&wb, (uint64_t)evtf_offset);
    for (ii = 0; ii < n_entries; ++ii)
    {
        cbuf_skip(&wb, 3); /* name */
        cbuf_uleb128(&wb, (uint64_t)entries[ii].sate_offset);
    }
    return wb.used;
}

/* ---- Per-file context: parse-once data ---- */

/* One way to code a SOC2 chunk's observations: without a predictor, or
 * with one.  Everything here is independent of the block-size tuple. */
struct soc2_cand
{
    int64_t *seq;          /* obs_count entries: scaled residual, or a marker */
    int      coded_count;  /* entries of seq[] that are not markers */
    size_t   pred_bytes;   /* predictor description */
    size_t   marker_bytes; /* absent-value and unpredicted-value blocks */
};

/* Per-signal cache: tuple-independent data for one (sv, sig) pair. */
struct sig_cache
{
    /* Owned, freed by ctx_free. */
    int64_t *full_scaled;    /* obs_count entries */
    int64_t *present_scaled; /* present_count entries (we don't actually need this after delta) */
    int64_t *delta_matrix;   /* 8 * present_count entries; NULL if present_count == 0 */
    uint8_t *all_bw;         /* 8 * present_count entries; NULL if present_count == 0 */
    int      obs_count;
    int      present_count;
    int64_t  obs_gcd;
    int      single_run;
    int      sig_idx;

    /* Tuple-independent payload prefix (before the packed bytes): obs_name +
     * count-minus-1 + lli RLE + ssi RLE. */
    size_t fixed_payload_bytes;

    /* If single_run, packed_bytes prefix (order header + init values)
     * is added in compute_socd_payload_size_cached() since order varies
     * with tuple. */

    /* --soc2 only: SOC2 payload prefix before the predictor fields
     * (obs_name + count-minus-1 + LLI RLE + mode + SSI RLE), and the
     * candidate codings (cand[0] has no predictor). */
    size_t soc2_prefix_bytes;
    struct soc2_cand soc2[2];
    int n_soc2;

    /* --fixed-order only: delta order plus one, once chosen ([0] for
     * SOCD, [1 + cc] for SOC2 candidate cc); zero until then. */
    unsigned char order_plus1[3];
};

struct sv_cache
{
    const struct rinex_satellite_data *sv;
    int n_obs;
    int n_sigs_observed; /* sigs with start >= 0 */
    int sys_idx;
    struct sig_cache *sigs; /* n_obs entries; entries with start<0 are zeroed */
};

struct file_ctx
{
    char *filename;
    struct rinex_data data;
    int loaded;
    int total_svs;
    struct sv_cache *svs;  /* total_svs entries */
    size_t rhdr_bytes;
    size_t epoc_bytes;
    size_t evtf_total_bytes;
    size_t fixed_pre_first_socd; /* SRNX + RHDR + EPOC + EVTFs on disk */
    int chunk_digest_len;
    int file_digest_len;
    /* --soc2 only: GLONASS frequency channel by slot (index 1..99), from
     * the GLONASS SLOT / FRQ # header record, or GLO_CHAN_UNKNOWN. */
    signed char glo_chan[100];
};


static void sig_cache_free(struct sig_cache *sc)
{
    int ii;
    free(sc->full_scaled);
    free(sc->present_scaled);
    free(sc->delta_matrix);
    free(sc->all_bw);
    for (ii = 0; ii < sc->n_soc2; ++ii)
        free(sc->soc2[ii].seq);
    memset(sc, 0, sizeof *sc);
}

static void ctx_free(struct file_ctx *ctx)
{
    int ss;
    if (!ctx) return;
    if (ctx->loaded)
        free_rinex_data(&ctx->data);
    if (ctx->svs)
    {
        for (ss = 0; ss < ctx->total_svs; ++ss)
        {
            int jj;
            if (ctx->svs[ss].sigs)
            {
                for (jj = 0; jj < ctx->svs[ss].n_obs; ++jj)
                    sig_cache_free(&ctx->svs[ss].sigs[jj]);
                free(ctx->svs[ss].sigs);
            }
        }
        free(ctx->svs);
    }
    free(ctx->filename);
    memset(ctx, 0, sizeof *ctx);
}

/* ---- SOC2 preparation (--soc2) ---- */

/** Prepare SOC2 candidates and SSI coding for every signal of \a svc,
 * using the shared soc2_plan_sv(). */
static int soc2_prepare_sv(struct file_ctx *ctx, struct sv_cache *svc,
    const struct rinex_system_data *p_sys)
{
    const struct rinex_data *data = &ctx->data;
    const struct rinex_satellite_data *sv = svc->sv;
    int count = sv->obs_used, n_obs = svc->n_obs;
    struct soc2_plan *plans;
    int64_t *t7;
    unsigned char *sym;
    int jj;

    t7 = malloc((size_t)count * sizeof *t7);
    plans = malloc((size_t)n_obs * sizeof *plans);
    sym = malloc((size_t)count);
    if (!t7 || !plans || !sym
        || soc2_plan_sv(data, sv, p_sys, ctx->glo_chan, t7, plans) != 0)
    {
        fprintf(stderr, "Out of memory\n");
        free(t7); free(plans); free(sym);
        return -1;
    }

    for (jj = 0; jj < n_obs; ++jj)
    {
        struct sig_cache *sc = &svc->sigs[jj];
        const struct soc2_plan *plan = &plans[jj];
        const int64_t *obs;
        const char *ssi_arr;
        struct soc2_pred none = {0, 0, 0, 1, 0, 0};
        size_t ssi_plain, ssi_pred;
        int cc;

        if (sv->start[jj] < 0)
            continue;
        obs = data->obs + sv->start[jj];
        ssi_arr = data->ssi + sv->start[jj];

        /* Mode value plus SSIs: a plain eleven-symbol RLE, or a
         * twelve-symbol RLE against an S observation on the same band.
         * The predictor kind occupies the mode's low two bits, so it never
         * changes the mode's ULEB128 length. */
        soc2_ssi_symbols(sym, ssi_arr, NULL, count);
        ssi_plain = 1 + rle_symbols_bytes(sym, count, 11);
        ssi_pred = ssi_plain;
        if (plan->s_ref >= 0)
        {
            soc2_ssi_symbols(sym, ssi_arr, data->obs + sv->start[plan->s_ref], count);
            ssi_pred = (size_t)uleb128_len(((uint64_t)plan->s_ref + 1) << 2)
                + rle_symbols_bytes(sym, count, 12);
        }
        sc->soc2_prefix_bytes = 8 + (size_t)uleb128_len((uint64_t)(count - 1))
            + rle_indicator_bytes(data->lli + sv->start[jj], count)
            + (ssi_pred < ssi_plain ? ssi_pred : ssi_plain);

        sc->n_soc2 = plan->pred.kind != SRNX_PRED_NONE ? 2 : 1;
        for (cc = 0; cc < sc->n_soc2; ++cc)
        {
            struct soc2_cand *cand = &sc->soc2[cc];
            const struct soc2_pred *pr = cc ? &plan->pred : &none;
            cand->seq = malloc((size_t)count * sizeof *cand->seq);
            if (!cand->seq)
            {
                fprintf(stderr, "Out of memory\n");
                free(t7); free(plans); free(sym);
                return -1;
            }
            cand->coded_count = soc2_build_seq(cand->seq, obs, count,
                sc->obs_gcd, pr, cc ? data->obs + sv->start[pr->ref] : NULL, t7);
            cand->pred_bytes = soc2_pred_bytes(pr);
            cand->marker_bytes = soc2_marker_bytes(cand->seq, obs, count,
                sc->obs_gcd);
        }
    }

    free(t7);
    free(plans);
    free(sym);
    return 0;
}

/** Load a RINEX file and precompute everything that does not depend on
 * the block-size tuple: delta matrices, per-order bit-widths, RLE byte
 * counts, plus the (tuple-independent) byte sizes of SRNX/RHDR/EPOC/EVTF.
 * Returns 0 on success, -1 on error (message printed to stderr). */
static int ctx_load(struct file_ctx *ctx, const char *filename)
{
    const char *err;
    int sys_idx;
    int sv_count = 0;

    memset(ctx, 0, sizeof *ctx);
    ctx->filename = strdup(filename);

    err = rinex_load_file(filename, &ctx->data);
    if (err)
    {
        fprintf(stderr, "Unable to load %s: %s\n", filename, err);
        return -1;
    }
    ctx->loaded = 1;

    ctx->chunk_digest_len = rnx_digest_length(g_chunk_digest_id);
    if (ctx->chunk_digest_len < 0) ctx->chunk_digest_len = 0;
    ctx->file_digest_len  = rnx_digest_length(g_file_digest_id);
    if (ctx->file_digest_len < 0)  ctx->file_digest_len = 0;
    if (g_soc2)
        soc2_parse_glo_chan(ctx->data.file_header,
            ctx->data.file_header_len, ctx->glo_chan);

    /* Count active satellites. */
    for (sys_idx = 0; sys_idx < 32; ++sys_idx)
    {
        const struct rinex_system_data *p_sys = &ctx->data.sys[sys_idx];
        int sv_idx;
        if (p_sys->n_obs <= 0)
            continue;
        for (sv_idx = p_sys->sv.start; sv_idx < p_sys->sv.end; ++sv_idx)
        {
            const struct rinex_satellite_data *p_sv = ctx->data.sv[sv_idx];
            if (!p_sv || p_sv->obs_used <= 0)
                continue;
            sv_count++;
        }
    }
    ctx->total_svs = sv_count;
    ctx->svs = calloc((size_t)(sv_count > 0 ? sv_count : 1), sizeof *ctx->svs);
    if (!ctx->svs) { fprintf(stderr, "Out of memory\n"); return -1; }

    /* Compute tuple-independent chunk sizes. */
    ctx->rhdr_bytes = rhdr_payload_bytes(&ctx->data);
    ctx->epoc_bytes = epoc_payload_bytes(&ctx->data);

    {
        int ii;
        size_t total = 0;
        for (ii = 0; ii < ctx->data.event_used; ++ii)
        {
            size_t payload = (size_t)uleb128_len(
                (uint64_t)ctx->data.event[ii].epoch_index)
                + (size_t)ctx->data.event[ii].text_len;
            total += chunk_on_disk(payload, ctx->chunk_digest_len);
        }
        ctx->evtf_total_bytes = total;
    }

    {
        size_t srnx_bytes = 4 + 1 + SRNX_PAYLOAD_SIZE
            + (size_t)ctx->chunk_digest_len; /* "SRNX" + ULEB(0x10) + payload + digest */
        size_t rhdr_disk = chunk_on_disk(ctx->rhdr_bytes, ctx->chunk_digest_len);
        size_t epoc_disk = chunk_on_disk(ctx->epoc_bytes, ctx->chunk_digest_len);
        ctx->fixed_pre_first_socd = srnx_bytes + rhdr_disk + epoc_disk
            + ctx->evtf_total_bytes;
    }

    /* Per-(sv, sig) tuple-independent precomputation. */
    {
        int ss = 0;
        for (sys_idx = 0; sys_idx < 32; ++sys_idx)
        {
            struct rinex_system_data *p_sys = &ctx->data.sys[sys_idx];
            int n_obs, sv_idx, jj;
            if (p_sys->n_obs <= 0)
                continue;
            n_obs = p_sys->n_obs;
            for (sv_idx = p_sys->sv.start; sv_idx < p_sys->sv.end; ++sv_idx)
            {
                struct rinex_satellite_data *p_sv = ctx->data.sv[sv_idx];
                struct sv_cache *svc;
                if (!p_sv || p_sv->obs_used <= 0)
                    continue;
                svc = &ctx->svs[ss++];
                svc->sv = p_sv;
                svc->n_obs = n_obs;
                svc->sys_idx = sys_idx;
                svc->sigs = calloc((size_t)n_obs, sizeof *svc->sigs);
                if (!svc->sigs)
                {
                    fprintf(stderr, "Out of memory\n");
                    return -1;
                }
                for (jj = 0; jj < n_obs; ++jj)
                {
                    struct sig_cache *sc = &svc->sigs[jj];
                    sc->sig_idx = jj;
                    if (p_sv->start[jj] < 0)
                        continue;
                    svc->n_sigs_observed++;

                    {
                        const int64_t *obs_arr = ctx->data.obs + p_sv->start[jj];
                        const char *lli_arr  = ctx->data.lli + p_sv->start[jj];
                        const char *ssi_arr  = ctx->data.ssi + p_sv->start[jj];
                        int obs_count = p_sv->obs_used;
                        int ii, present_count = 0;
                        int64_t obs_gcd = 0;
                        int single_run;
                        size_t fixed = 0;

                        sc->obs_count = obs_count;
                        sc->full_scaled = malloc((size_t)obs_count
                            * sizeof *sc->full_scaled);
                        if (!sc->full_scaled)
                        { fprintf(stderr, "Out of memory\n"); return -1; }

                        for (ii = 0; ii < obs_count; ++ii)
                            if (obs_arr[ii] != INT64_MIN)
                                obs_gcd = gcd(obs_gcd, obs_arr[ii]);
                        if (obs_gcd <= 0 || obs_gcd > 1000000)
                            obs_gcd = 1;
                        sc->obs_gcd = obs_gcd;

                        for (ii = 0; ii < obs_count; ++ii)
                        {
                            if (obs_arr[ii] == INT64_MIN)
                                sc->full_scaled[ii] = INT64_MIN;
                            else
                            {
                                sc->full_scaled[ii] = obs_arr[ii] / obs_gcd;
                                present_count++;
                            }
                        }
                        sc->present_count = present_count;
                        sc->single_run = single_run =
                            (present_count == obs_count);

                        /* SOCD delta matrices (not needed with --soc2,
                         * whose candidates are built per satellite below). */
                        if (present_count > 0 && !g_soc2)
                        {
                            int64_t *present_scaled;
                            int64_t *dm;
                            uint8_t *abw;
                            int kk;
                            present_scaled = malloc((size_t)present_count
                                * sizeof *present_scaled);
                            dm = malloc(8 * (size_t)present_count
                                * sizeof *dm);
                            abw = malloc(8 * (size_t)present_count);
                            if (!present_scaled || !dm || !abw)
                            { fprintf(stderr, "Out of memory\n"); return -1; }
                            present_count = 0;
                            for (ii = 0; ii < obs_count; ++ii)
                                if (sc->full_scaled[ii] != INT64_MIN)
                                    present_scaled[present_count++] =
                                        sc->full_scaled[ii];
                            compute_delta_matrix(dm, present_scaled,
                                present_count);
                            /* per-order bit-widths. */
                            for (kk = 0; kk < 8; ++kk)
                            {
                                const int64_t *vals = dm
                                    + (size_t)kk * present_count;
                                uint8_t *bw = abw + (size_t)kk * present_count;
                                int qq;
                                for (qq = 0; qq < present_count; ++qq)
                                    bw[qq] = twos_comp_bw(vals[qq]);
                            }
                            sc->present_scaled = present_scaled;
                            sc->delta_matrix = dm;
                            sc->all_bw = abw;
                        }

                        /* Tuple-independent payload prefix bytes. */
                        fixed += 8; /* obs_name */
                        fixed += (size_t)uleb128_len((uint64_t)(obs_count - 1));
                        fixed += rle_indicator_bytes(lli_arr, obs_count);
                        fixed += rle_indicator_bytes(ssi_arr, obs_count);
                        sc->fixed_payload_bytes = fixed;
                        (void)single_run;
                    }
                }
                if (g_soc2 && soc2_prepare_sv(ctx, svc, p_sys) != 0)
                    return -1;
            }
        }
    }
    return 0;
}

/* Stride within cached delta_matrix / all_bw is the per-sig present_count,
 * so dp_core can be invoked with stride == cap by copying into scratch.
 * Simpler: copy the relevant order's bw/vals slice into scratch arrays
 * sized for the slice, since dp_core uses scratch->cap as the SPT stride. */
static int64_t dp_with_cache(struct scratch *scratch,
    const int64_t *delta_matrix, const uint8_t *all_bw, int present_count,
    int order, int start, int run_len)
{
    if (run_len <= 0)
        return 0;
    scratch_reserve(scratch, (size_t)run_len);
    return dp_core(scratch,
        delta_matrix + (size_t)order * present_count + start,
        all_bw + (size_t)order * present_count + start,
        run_len, NULL);
}

/* Same as select_delta_order but operating on a cached delta_matrix / all_bw
 * (stride = present_count rather than scratch->cap).
 *
 * \a *out_best_dp_cost receives the dp_core-only cost for the chosen order
 * (init bytes are added by the caller). */
static int select_delta_order_cached(struct scratch *scratch,
    const int64_t *delta_matrix, const uint8_t *all_bw, int present_count,
    int64_t *out_best_dp_cost)
{
    int best_order = 0;
    int64_t best_dp;
    int64_t best_total;
    int ord, no_improve;

    best_dp = dp_with_cache(scratch, delta_matrix, all_bw, present_count,
        0, 0, present_count);
    best_total = best_dp; /* order 0: no init bytes */
    no_improve = 0;
    for (ord = 1; ord <= 7 && ord < present_count; ++ord)
    {
        int64_t dp_cost = dp_with_cache(scratch, delta_matrix, all_bw,
            present_count, ord, 0, present_count);
        int64_t total = (int64_t)ord + dp_cost;
        if (total < best_total)
        {
            best_total = total;
            best_dp = dp_cost;
            best_order = ord;
            no_improve = 0;
        }
        else if (++no_improve >= 2)
            break;
    }
    if (out_best_dp_cost)
        *out_best_dp_cost = best_dp;
    return best_order;
}

/** Compute SOCD payload bytes for one signal using cached per-signal data. */
static size_t socd_payload_bytes_cached(struct scratch *scratch,
    struct sig_cache *sc)
{
    size_t packed = 0;
    int order = 0;
    int64_t whole_cost = 0;

    if (sc->obs_count == 0)
        return 0;

    if (sc->present_count > 0 && g_fixed_order && sc->order_plus1[0])
    {
        order = sc->order_plus1[0] - 1;
        if (sc->single_run)
            whole_cost = dp_with_cache(scratch, sc->delta_matrix, sc->all_bw,
                sc->present_count, order, 0, sc->present_count);
    }
    else if (sc->present_count > 0)
    {
        order = select_delta_order_cached(scratch, sc->delta_matrix,
            sc->all_bw, sc->present_count, &whole_cost);
        sc->order_plus1[0] = (unsigned char)(order + 1);
    }

    if (sc->obs_gcd > 1)
    {
        packed += (size_t)uleb128_len((uint64_t)(order + 8));
        packed += (size_t)uleb128_len((uint64_t)sc->obs_gcd);
    }
    else
    {
        packed += (size_t)uleb128_len((uint64_t)order);
    }
    packed += (size_t)order;

    if (sc->present_count == 0)
    {
        packed += 1 + (size_t)uleb128_len((uint64_t)(sc->obs_count - 1));
    }
    else if (sc->single_run)
    {
        packed += (size_t)whole_cost;
    }
    else
    {
        int ii = 0, pi = 0;
        while (ii < sc->obs_count)
        {
            if (sc->full_scaled[ii] == INT64_MIN)
            {
                int absent_len = 0;
                while (ii + absent_len < sc->obs_count
                    && sc->full_scaled[ii + absent_len] == INT64_MIN)
                    absent_len++;
                packed += 1 + (size_t)uleb128_len((uint64_t)(absent_len - 1));
                ii += absent_len;
            }
            else
            {
                int run_end = ii;
                int run_len;
                int64_t cost;
                while (run_end < sc->obs_count
                    && sc->full_scaled[run_end] != INT64_MIN)
                    run_end++;
                run_len = run_end - ii;
                cost = dp_with_cache(scratch, sc->delta_matrix, sc->all_bw,
                    sc->present_count, order, pi, run_len);
                packed += (size_t)cost;
                pi += run_len;
                ii = run_end;
            }
        }
    }
    return sc->fixed_payload_bytes
        + (size_t)uleb128_len((uint64_t)packed) + packed;
}

/** Compute SOC2 payload bytes for one signal: the cheaper of its
 * candidate codings under the current tuple. */
static size_t soc2_payload_bytes_cached(struct scratch *scratch,
    struct sig_cache *sc)
{
    size_t best = SIZE_MAX;
    int cc;

    scratch_reserve(scratch, (size_t)sc->obs_count);
    for (cc = 0; cc < sc->n_soc2; ++cc)
    {
        const struct soc2_cand *cand = &sc->soc2[cc];
        int64_t *coded = scratch->present_scaled;
        int64_t *dm = scratch->delta_matrix;
        int64_t whole_cost = 0;
        size_t packed = 0, payload;
        int order = 0, n_coded = 0, ii;

        for (ii = 0; ii < sc->obs_count; ++ii)
            if (cand->seq[ii] > SOC2_UNPRED)
                coded[n_coded++] = cand->seq[ii];
        if (n_coded > 0)
        {
            compute_delta_matrix(dm, coded, n_coded);
            if (g_fixed_order && sc->order_plus1[1 + cc])
            {
                order = sc->order_plus1[1 + cc] - 1;
                if (n_coded == sc->obs_count)
                    whole_cost = dp_core_rice(scratch,
                        dm + (size_t)order * n_coded, n_coded);
            }
            else
            {
                order = select_delta_order_rice(scratch, dm, n_coded,
                    &whole_cost);
                sc->order_plus1[1 + cc] = (unsigned char)(order + 1);
            }
        }

        if (sc->obs_gcd > 1)
        {
            packed += (size_t)uleb128_len((uint64_t)(order + 8));
            packed += (size_t)uleb128_len((uint64_t)sc->obs_gcd);
        }
        else
            packed += (size_t)uleb128_len((uint64_t)order);
        packed += (size_t)order;
        packed += cand->marker_bytes;

        if (n_coded == sc->obs_count)
            packed += (size_t)whole_cost;
        else if (n_coded > 0)
        {
            /* Blocks cannot span an absent or unpredicted marker. */
            const int64_t *row = dm + (size_t)order * n_coded;
            int pi = 0;
            ii = 0;
            while (ii < sc->obs_count)
            {
                int run_end = ii;
                while (run_end < sc->obs_count && cand->seq[run_end] > SOC2_UNPRED)
                    run_end++;
                if (run_end > ii)
                {
                    packed += (size_t)dp_core_rice(scratch, row + pi, run_end - ii);
                    pi += run_end - ii;
                    ii = run_end;
                }
                else
                    ii++;
            }
        }

        payload = sc->soc2_prefix_bytes + cand->pred_bytes
            + (size_t)uleb128_len((uint64_t)packed) + packed;
        if (payload < best)
            best = payload;
    }
    return best;
}

/* ---- Per-tuple total file bytes ---- */

static size_t total_bytes_for_tuple(struct scratch *scratch,
    const struct file_ctx *ctx)
{
    size_t offset = ctx->fixed_pre_first_socd;
    int ss, jj;
    int n_sdir_entries = 0;
    struct sdir_entry *sdir_entries;

    sdir_entries = calloc((size_t)(ctx->total_svs > 0 ? ctx->total_svs : 1),
        sizeof *sdir_entries);
    if (!sdir_entries)
    {
        fprintf(stderr, "Out of memory\n");
        exit(EXIT_FAILURE);
    }

    for (ss = 0; ss < ctx->total_svs; ++ss)
    {
        struct sv_cache *svc = &ctx->svs[ss];
        size_t *socd_offsets;
        size_t sate_pos, sate_payload, sate_disk;

        socd_offsets = calloc((size_t)svc->n_obs, sizeof *socd_offsets);
        if (!socd_offsets)
        {
            fprintf(stderr, "Out of memory\n");
            exit(EXIT_FAILURE);
        }
        for (jj = 0; jj < svc->n_obs; ++jj)
        {
            struct sig_cache *sc = &svc->sigs[jj];
            size_t payload, disk;
            if (svc->sv->start[jj] < 0)
            {
                socd_offsets[jj] = 0;
                continue;
            }
            payload = g_soc2 ? soc2_payload_bytes_cached(scratch, sc)
                             : socd_payload_bytes_cached(scratch, sc);
            disk    = chunk_on_disk(payload, ctx->chunk_digest_len);
            socd_offsets[jj] = offset;
            offset += disk;
        }

        sate_pos = offset;
        sate_payload = sate_payload_bytes(svc->sv, svc->n_obs, socd_offsets, sate_pos);
        sate_disk = chunk_on_disk(sate_payload, ctx->chunk_digest_len);
        free(socd_offsets);

        memcpy(sdir_entries[n_sdir_entries].name, svc->sv->id, 4);
        sdir_entries[n_sdir_entries].sate_offset = sate_pos;
        n_sdir_entries++;
        offset += sate_disk;
    }

    {
        size_t epoc_offset = 4 + 1 + SRNX_PAYLOAD_SIZE
            + (size_t)ctx->chunk_digest_len /* SRNX */
            + chunk_on_disk(ctx->rhdr_bytes, ctx->chunk_digest_len);
        size_t evtf_offset = ctx->data.event_used > 0
            ? epoc_offset + chunk_on_disk(ctx->epoc_bytes, ctx->chunk_digest_len)
            : 0;
        size_t sdir_payload = sdir_payload_bytes(epoc_offset, evtf_offset,
            sdir_entries, n_sdir_entries);
        size_t sdir_disk = chunk_on_disk(sdir_payload, ctx->chunk_digest_len);
        offset += sdir_disk;
    }
    offset += (size_t)ctx->file_digest_len;

    free(sdir_entries);
    return offset;
}

/* ---- Tuple parsing ---- */

static int parse_tuple(const char *s, int out[MAX_TUPLE_LEN], int *out_n)
{
    int n = 0;
    const char *p = s;
    while (*p)
    {
        char *endp;
        long v;
        if (n >= MAX_TUPLE_LEN)
        {
            fprintf(stderr, "Tuple too long (max %d): %s\n", MAX_TUPLE_LEN, s);
            return -1;
        }
        v = strtol(p, &endp, 10);
        if (endp == p || v <= 0 || (v % 8) != 0 || v > 65536)
        {
            fprintf(stderr, "Bad tuple value at '%s' (must be 8*N, 8..65536)\n",
                p);
            return -1;
        }
        out[n++] = (int)v;
        p = endp;
        if (*p == ',') p++;
        else if (*p && !isspace((unsigned char)*p))
        {
            fprintf(stderr, "Unexpected character '%c' in tuple '%s'\n",
                *p, s);
            return -1;
        }
        else if (isspace((unsigned char)*p))
            break;
    }
    if (n == 0)
    {
        fprintf(stderr, "Empty tuple\n");
        return -1;
    }
    /* Verify ascending order. */
    {
        int ii;
        for (ii = 1; ii < n; ++ii)
            if (out[ii] <= out[ii - 1])
            {
                fprintf(stderr, "Tuple must be strictly ascending: %s\n", s);
                return -1;
            }
    }
    /* Must include 8 as the smallest member. */
    if (out[0] != 8)
    {
        fprintf(stderr, "Tuple must include 8 as smallest member: %s\n", s);
        return -1;
    }
    *out_n = n;
    return 0;
}

/* Tuple list storage. */
struct tuple_entry
{
    int  cand[MAX_TUPLE_LEN];
    int  n;
};

struct tuple_list
{
    struct tuple_entry *entries;
    int n;
    int cap;
};

static void tuple_list_init(struct tuple_list *tl)
{
    tl->entries = NULL;
    tl->n = 0;
    tl->cap = 0;
}

static void tuple_list_free(struct tuple_list *tl)
{
    free(tl->entries);
    tl->entries = NULL;
    tl->n = tl->cap = 0;
}

static void tuple_list_push(struct tuple_list *tl, const int cand[MAX_TUPLE_LEN], int n)
{
    if (tl->n == tl->cap)
    {
        tl->cap = tl->cap ? tl->cap * 2 : 16;
        tl->entries = xrealloc(tl->entries, (size_t)tl->cap * sizeof *tl->entries);
    }
    memcpy(tl->entries[tl->n].cand, cand, sizeof tl->entries[tl->n].cand);
    tl->entries[tl->n].n = n;
    tl->n++;
}

static int tuple_list_load_file(struct tuple_list *tl, const char *path)
{
    FILE *fp = fopen(path, "r");
    char line[1024];
    if (!fp)
    {
        fprintf(stderr, "Unable to open tuples file %s\n", path);
        return -1;
    }
    while (fgets(line, sizeof line, fp))
    {
        int cand[MAX_TUPLE_LEN];
        int n = 0;
        char *trim = line;
        /* skip leading whitespace */
        while (*trim && isspace((unsigned char)*trim)) trim++;
        if (*trim == '\0' || *trim == '#')
            continue;
        if (parse_tuple(trim, cand, &n) != 0)
        {
            fclose(fp);
            return -1;
        }
        tuple_list_push(tl, cand, n);
    }
    fclose(fp);
    return 0;
}

/* ---- Tuple display for output ---- */
static void format_tuple(char *buf, size_t buflen, const int cand[], int n)
{
    int ii;
    size_t pos = 0;
    for (ii = 0; ii < n; ++ii)
    {
        int written = snprintf(buf + pos, buflen - pos,
            ii == 0 ? "%d" : "-%d", cand[ii]);
        if (written <= 0 || (size_t)written >= buflen - pos)
            break;
        pos += (size_t)written;
    }
}

/* ---- main ---- */
static void usage(FILE *out, const char *argv0)
{
    fprintf(out,
        "Usage: %s [--chunk-digest=<id>] [--file-digest=<id>]\n"
        "          [--tuple=N1,N2,...] [--tuples-file=PATH] [--soc2] [--fixed-order]\n"
        "          [--order-search=scan|guess]\n"
        "          [--max-sleb-run=<N>] [--max-block=<N>] [--timing] [--help] <input> ...\n",
        argv0);
}

int main(int argc, char *argv[])
{
    int status = EXIT_SUCCESS;
    struct tuple_list tuples;
    struct scratch scratch;
    int ii;
    int first_pos_arg = -1;
    int verbose_timing = 0;

    tuple_list_init(&tuples);
    scratch_init(&scratch);

    for (ii = 1; ii < argc; ++ii)
    {
        const char *arg = argv[ii];
        if (parse_digest_flag(arg, "--chunk-digest=", &g_chunk_digest_id))
            continue;
        if (parse_digest_flag(arg, "--file-digest=", &g_file_digest_id))
            continue;
        if (!strcmp(arg, "--help"))
        {
            usage(stdout, argv[0]);
            return EXIT_SUCCESS;
        }
        if (!strncmp(arg, "--tuple=", 8))
        {
            int cand[MAX_TUPLE_LEN]; int n = 0;
            if (parse_tuple(arg + 8, cand, &n) != 0)
                return EXIT_FAILURE;
            tuple_list_push(&tuples, cand, n);
            continue;
        }
        if (!strncmp(arg, "--tuples-file=", 14))
        {
            if (tuple_list_load_file(&tuples, arg + 14) != 0)
                return EXIT_FAILURE;
            continue;
        }
        if (!strcmp(arg, "--timing"))
        {
            verbose_timing = 1;
            continue;
        }
        if (!strcmp(arg, "--soc2"))
        {
            g_soc2 = 1;
            continue;
        }
        if (!strcmp(arg, "--fixed-order"))
        {
            g_fixed_order = 1;
            continue;
        }
        if (parse_order_search(arg))
            continue;
        if (!strncmp(arg, "--max-sleb-run=", 15))
        {
            char *end;
            long v = strtol(arg + 15, &end, 10);
            if (*end != '\0' || v < 1 || v > INT_MAX)
            {
                fprintf(stderr, "Invalid --max-sleb-run value '%s'\n", arg + 15);
                return EXIT_FAILURE;
            }
            g_max_sleb_run = (int)v;
            continue;
        }
        if (!strncmp(arg, "--max-block=", 12))
        {
            char *end;
            long v = strtol(arg + 12, &end, 10);
            if (*end != '\0' || v < 8 || v > 65536)
            {
                fprintf(stderr, "Invalid --max-block value '%s' (must be 8..65536)\n",
                        arg + 12);
                return EXIT_FAILURE;
            }
            /* Must be a power of 2 for sparse table alignment. */
            if ((v & (v - 1)) != 0)
            {
                fprintf(stderr, "--max-block must be a power of 2\n");
                return EXIT_FAILURE;
            }
            g_max_block = (int)v;
            continue;
        }
        if (!strcmp(arg, "--"))
        {
            first_pos_arg = ii + 1;
            break;
        }
        if (arg[0] == '-' && arg[1] != '\0')
        {
            fprintf(stderr, "Unknown option '%s'\n", arg);
            return EXIT_FAILURE;
        }
        first_pos_arg = ii;
        break;
    }

    if (first_pos_arg < 0)
    {
        usage(stderr, argv[0]);
        return EXIT_FAILURE;
    }
    if (tuples.n == 0)
    {
        fprintf(stderr,
            "No tuples specified; use --tuple= or --tuples-file=.\n");
        return EXIT_FAILURE;
    }

    /* Validate tuples against the final --max-block value now that all
     * arguments (which may specify --max-block after --tuple=) have
     * been parsed. */
    for (ii = 0; ii < tuples.n; ++ii)
    {
        int jj;
        for (jj = 0; jj < tuples.entries[ii].n; ++jj)
        {
            if (tuples.entries[ii].cand[jj] > g_max_block)
            {
                fprintf(stderr, "Block size %d exceeds --max-block=%d\n",
                    tuples.entries[ii].cand[jj], g_max_block);
                return EXIT_FAILURE;
            }
        }
    }
    g_max_levels = compute_max_levels(g_max_block);

    for (ii = first_pos_arg; ii < argc; ++ii)
    {
        struct file_ctx ctx;
        struct timespec t0, t1, t2;
        int tt;

        clock_gettime(CLOCK_MONOTONIC, &t0);
        if (ctx_load(&ctx, argv[ii]) != 0)
        {
            ctx_free(&ctx);
            status = EXIT_FAILURE;
            continue;
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);

        for (tt = 0; tt < tuples.n; ++tt)
        {
            char tup_buf[MAX_TUPLE_LEN * 8];
            size_t total;
            int kk;
            for (kk = 0; kk < tuples.entries[tt].n; ++kk)
                g_cand[kk] = tuples.entries[tt].cand[kk];
            g_n_cand = tuples.entries[tt].n;

            total = total_bytes_for_tuple(&scratch, &ctx);
            format_tuple(tup_buf, sizeof tup_buf, g_cand, g_n_cand);
            printf("%s\t%s\t%zu\n", argv[ii], tup_buf, total);
        }
        clock_gettime(CLOCK_MONOTONIC, &t2);

        if (verbose_timing)
        {
            double parse_ms = (t1.tv_sec - t0.tv_sec) * 1000.0
                + (t1.tv_nsec - t0.tv_nsec) / 1e6;
            double dp_ms    = (t2.tv_sec - t1.tv_sec) * 1000.0
                + (t2.tv_nsec - t1.tv_nsec) / 1e6;
            fprintf(stderr,
                "# %s parse=%.1fms tuples=%d total_eval=%.1fms eval_per_tuple=%.1fms\n",
                argv[ii], parse_ms, tuples.n, dp_ms,
                tuples.n > 0 ? dp_ms / tuples.n : 0.0);
        }
        fflush(stdout);
        ctx_free(&ctx);
    }

    scratch_free(&scratch);
    tuple_list_free(&tuples);
    return status;
}

#endif /* defined(RNX2SRNX_SEARCH) */
