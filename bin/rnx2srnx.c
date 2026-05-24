/** rnx2srnx.c - Convert RINEX files to Succinct RINEX format.
 * Copyright 2021 Michael Poole.
 * SPDX-License-Identifier: MIT-Modern-Variant
 */

#include "rinex/digest.h"
#include "rinex/rinex_load.h"

#include <ctype.h>
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
#include <unistd.h>

/* Digest id used for per-chunk digests; default is CRC32C (id=2). */
static int g_chunk_digest_id = 2;

/* Digest id used for the file-level digest; default is BLAKE3-256 (id=5). */
static int g_file_digest_id = 5;

/* Upper bound on SLEB128-run length considered by dp_core (--max-sleb-run). */
static int g_max_sleb_run = 16;

/* Output filename for cleanup on fatal error. */
static const char *g_output_name;

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

static int uleb128_len(uint64_t val)
{
    int n = 1;
    while (val >= 128) { val >>= 7; n++; }
    return n;
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

/* ---- Zigzag bit-width ---- */

/** Return minimum bits for two's-complement representation of \a val. */
static uint8_t twos_comp_bw(int64_t val)
{
    uint64_t v = (val >= 0) ? (uint64_t)val : (uint64_t)(~val);
    return v == 0 ? 1 : (uint8_t)(65 - __builtin_clzll(v));
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

/* Returns the alphabet index of ch, or -1 if not in alphabet. */
static int indicator_idx(unsigned char ch)
{
    if (ch == ' ') return 0;
    if (ch >= '0' && ch <= '9') return 1 + (ch - '0');
    return -1;
}

static void rle_encode_indicators(struct wbuf *wb, const char *ind, int count,
    const char *filename, const char *type)
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
            fail_and_exit("%s: %s byte 0x%02X not in alphabet (%d/%d)",
                filename, type, (unsigned char)cur, ii, count);
        }
        /* Packed encoding: single ULEB128 = (count-1)*11 + idx */
        wbuf_uleb128(&rle, (uint64_t)(ii - run_start - 1) * 11 + (uint64_t)idx);
    }

    wbuf_uleb128(wb, rle.used);
    wbuf_append(wb, rle.data, rle.used);
    wbuf_free(&rle);
}

/* ---- GCD computation ---- */

static int64_t gcd(int64_t a, int64_t b)
{
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    while (b) { int64_t t = b; b = a % b; a = t; }
    return a;
}

/* ---- Delta matrix computation ---- */

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

/* ---- DP-based block selection ----
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
    BK_SLEB     /**< SLEB128 run (1..16 values) */
};

struct block_choice
{
    short kind;     /**< enum block_kind */
    short bw;       /**< for BK_BLOCK: 1..32; else unused */
    int   len;      /**< values consumed by this edge */
};

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
    uint8_t *spt;            /**< 7 * cap : sparse table for dp_core window-max */
    uint8_t *all_bw;         /**< 8 * cap : bw for all 8 delta orders */
    uint32_t *psum_arr;      /**< cap + 1 : prefix sums of sleb[] within dp_core */
    struct block_choice *choices_a; /**< cap; "best so far" */
    struct block_choice *choices_b; /**< cap; "candidate" */
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
    s->spt            = xrealloc(s->spt,          7 * cap * sizeof *s->spt);
    s->all_bw         = xrealloc(s->all_bw,       8 * cap * sizeof *s->all_bw);
    s->psum_arr       = xrealloc(s->psum_arr,   (cap + 1) * sizeof *s->psum_arr);
    s->choices_a      = xrealloc(s->choices_a,        cap * sizeof *s->choices_a);
    s->choices_b      = xrealloc(s->choices_b,        cap * sizeof *s->choices_b);
    s->cap = cap;
}

/* Block sizes for high-3-bits values 0..6. */
static const int cand[7] = {8, 16, 24, 32, 40, 48, 112};

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
        for (ci = 0; ci < 7; ++ci)
        {
            int m = cand[ci];
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
            bw[ii] = (uint8_t)twos_comp_bw(vals[ii]);;
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

/** Write a bit-transposed block of \a count values, each \a bits wide.
 * count must be 16 or 32.
 */
static void write_transposed_block(struct wbuf *wb, const int64_t *vals,
    int count, int bits)
{
    int byte_col, row;
    int bytes_per_row = count >> 3;
    char matrix[32 * 16]; /* max 32 bits * 16 bytes/row for count=128 = 512 bytes */

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
            int hi3 =
                c.len ==   8 ? 0 :
                c.len ==  16 ? 1 :
                c.len ==  24 ? 2 :
                c.len ==  32 ? 3 :
                c.len ==  40 ? 4 :
                c.len ==  48 ? 5 : 6; /* c.len == 112 */
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
    rle_encode_indicators(&payload, lli_arr, obs_count, filename, "LLI");
    rle_encode_indicators(&payload, ssi_arr, obs_count, filename, "SSI");

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

#define SRNX_PAYLOAD_SIZE 16

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
     * Fields: major=1, minor=0, chunk_digest=<g_chunk_digest_id>,
     * file_digest=<g_file_digest_id>, sdir_offset=0 (patched later), padding.
     */
    wbuf_init(&tmp);
    wbuf_uleb128(&tmp, 1); /* major */
    wbuf_uleb128(&tmp, 0); /* minor */
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

struct sdir_entry
{
    char name[4];
    size_t sate_offset;
};

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

static void rnx2srnx(const char input_name[], const char output_name[])
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
    int n_sdir = 0, sdir_alloc = 0;
    int sys_idx;

    scratch_init(&scratch);
    g_output_name = output_name;

    /* Load the input file. */
    err = rinex_load_file(input_name, &data);
    if (err)
    {
        fprintf(stderr, "Unable to load %s: %s\n", input_name, err);
        return;
    }

    /* Pick an upper bound for the output size.  SRNX is compressed
     * relative to RINEX, so input_size + a small margin is a safe
     * ceiling for all realistic observation files.
     */
    if (stat(input_name, &st) != 0)
    {
        fprintf(stderr, "Unable to stat %s\n", input_name);
        free_rinex_data(&data);
        return;
    }
    cap = (size_t)st.st_size + 65536;
    if (cap < 262144)
        cap = 262144;

    /* Open and size the output file. */
    fd = open(output_name, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
    {
        fprintf(stderr, "Unable to create %s\n", output_name);
        free_rinex_data(&data);
        return;
    }
    if (ftruncate(fd, (off_t)cap) != 0)
    {
        fprintf(stderr, "ftruncate failed on %s\n", output_name);
        close(fd);
        free_rinex_data(&data);
        return;
    }
    mm.data = mmap(NULL, cap, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mm.data == MAP_FAILED)
    {
        fprintf(stderr, "mmap failed on %s\n", output_name);
        close(fd);
        free_rinex_data(&data);
        return;
    }
    mm.cap = cap;
    mm.used = 0;

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
                goto done;
            }
            for (jj = 0; jj < n_obs; ++jj)
                socd_offsets[jj] = write_socd_chunk(&mm, &scratch, &data, p_sv, jj, p_sys, input_name);

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
                    goto done;
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
        if (ftruncate(fd, (off_t)mm.used) != 0)
            fprintf(stderr, "ftruncate-to-used failed on %s\n", output_name);
        close(fd);
    }
    free(sdir_entries);
    scratch_free(&scratch);
    free_rinex_data(&data);
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

int main(int argc, char *argv[])
{
    const char *input_name = NULL;
    const char *output_arg = NULL;
    char *output_name;
    size_t name_len;
    int ii;

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
        fprintf(stderr, "Usage: %s [--chunk-digest=<id>] [--file-digest=<id>] "
            "[--max-sleb-run=<N>] <input.rnx> [output.srnx]\n", argv[0]);
        return EXIT_FAILURE;
    }

    output_name = output_arg ? strdup(output_arg) : NULL;
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

    rnx2srnx(input_name, output_name);

    free(output_name);
    return EXIT_SUCCESS;
}
