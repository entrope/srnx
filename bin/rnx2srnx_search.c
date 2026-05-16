/** rnx2srnx_search.c - SRNX block-size tuple search tool.
 * Copyright 2021 Michael Poole.
 * SPDX-License-Identifier: MIT-Modern-Variant
 *
 * Clone of rnx2srnx that amortizes RINEX parsing across multiple
 * candidate block-size tuples.  For each input file and each requested
 * tuple, reports the total encoded byte count to stdout.  No SRNX
 * bytes are written; only the byte cost of an encoding under the given
 * tuple is computed.
 *
 * The DP that picks block boundaries is parameterized over a runtime
 * tuple of block sizes (each a positive multiple of 8) instead of the
 * spec's hard-coded {8, 16, 32, 64, 128}.  The transpose step is
 * skipped entirely: we only need byte counts, not decodable output.
 *
 * Output: one line per (file, tuple), tab-separated:
 *     <file>\t<tuple>\t<bytes>
 * where <tuple> is dash-separated, e.g. "8-16-32-64-128".
 */

#include "rinex/digest.h"
#include "rinex/rinex_load.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* ---- Configuration globals ---- */

/* Digest ids match rnx2srnx defaults so the regression check (spec tuple
 * + spec defaults) reproduces an unmodified rnx2srnx run's file size. */
static int g_chunk_digest_id = 2;    /* CRC32C */
static int g_file_digest_id  = 5;    /* BLAKE3-256 */

/* Runtime block-size tuple used by the DP and the byte-cost accounting.
 * MUST be sorted ascending, all multiples of 8, all in [8, MAX_BLOCK].
 * Caller is expected to include 8 (the DP relies on at least one small
 * size for fallback runs of len 1..7).  MAX_BLOCK is bounded by the
 * sparse-table levels precomputed below. */
#define MAX_TUPLE_LEN  7
#define MAX_BLOCK     512   /* generous; 2^9 = 512 */
#define MAX_LEVELS    10    /* log2(MAX_BLOCK) + 1 */
static int g_cand[MAX_TUPLE_LEN];
static int g_n_cand;

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

static void cbuf_uleb128(struct cbuf *wb, uint64_t val)
{
    wb->used += uleb128_len(val);
}

static void cbuf_sleb128(struct cbuf *wb, int64_t val)
{
    wb->used += sleb128_len(val);
}

/* sleb128 length keyed by two's-complement bit-width. */
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

static uint8_t twos_comp_bw(int64_t val)
{
    uint64_t v = (val >= 0) ? (uint64_t)val : (uint64_t)(~val);
    return v == 0 ? 1 : (uint8_t)(65 - __builtin_clzll(v));
}

static int64_t gcd_i64(int64_t a, int64_t b)
{
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    while (b) { int64_t t = b; b = a % b; a = t; }
    return a;
}

/* ---- Compute 8 delta orders into d[0..8*count-1] ---- */

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

/* ---- DP machinery ---- */

enum block_kind
{
    BK_BLOCK,
    BK_ZERO,
    BK_SLEB
};

struct block_choice
{
    short kind;
    short bw;
    int   len;
};

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
    uint8_t *spt;            /**< MAX_LEVELS * cap : window-max sparse table */
    uint8_t *all_bw;         /**< 8 * cap : bw for all 8 delta orders */
    uint32_t *psum_arr;      /**< cap + 1 : prefix sums of sleb[] within dp_core */
    struct block_choice *choices_a;
    struct block_choice *choices_b;
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
    s->spt            = xrealloc(s->spt, MAX_LEVELS * cap * sizeof *s->spt);
    s->all_bw         = xrealloc(s->all_bw,       8 * cap * sizeof *s->all_bw);
    s->psum_arr       = xrealloc(s->psum_arr,   (cap + 1) * sizeof *s->psum_arr);
    s->choices_a      = xrealloc(s->choices_a,        cap * sizeof *s->choices_a);
    s->choices_b      = xrealloc(s->choices_b,        cap * sizeof *s->choices_b);
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
    if (levels > MAX_LEVELS)
        levels = MAX_LEVELS;

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
    int ii, ci, zlen_here, zlen_next, L, max_L;
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
        int remaining = count - ii;
        int64_t best = INT64_MAX;
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
                best_choice.kind = BK_BLOCK; /* placeholder; will overwrite */
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

        /* SLEB128-run transition: lengths 1..min(remaining, 16). */
        max_L = remaining < 16 ? remaining : 16;
        {
            uint32_t pii = psum[ii];
            for (L = 1; L <= max_L; ++L)
            {
                cost = 2 + (int64_t)(psum[ii + L] - pii) + dp[ii + L];
                if (cost < best)
                {
                    best = cost;
                    best_choice.kind = BK_SLEB;
                    best_choice.bw = 0;
                    best_choice.len = L;
                }
            }
        }

        dp[ii] = best;
        if (choices)
            choices[ii] = best_choice;
    }

    return dp[0];
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
        while (ii < count && ind[ii] == cur)
            ii++;
        rle_used += 1;                                   /* the indicator byte */
        rle_used += (size_t)uleb128_len((uint64_t)(ii - run_start - 1));
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
            obs_gcd = gcd_i64(obs_gcd, obs_arr[ii]);
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

#define SRNX_PAYLOAD_SIZE 16

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
static int same_span_epoch(const struct rinex_epoch *prev,
    const struct rinex_epoch *cur, int64_t interval_e7)
{
    int64_t expected_sec;

    if (cur->yyyy_mm_dd != prev->yyyy_mm_dd)
        return 0;
    expected_sec = prev->sec_e7 + interval_e7;
    if (expected_sec >= 600000000 && prev->sec_e7 < 600000000)
    {
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
                if (!same_span_epoch(&data->epoch[span_start + span_len - 1],
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
struct sdir_entry
{
    char   name[4];
    size_t sate_offset;
};

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
};

static void sig_cache_free(struct sig_cache *sc)
{
    free(sc->full_scaled);
    free(sc->present_scaled);
    free(sc->delta_matrix);
    free(sc->all_bw);
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
                                obs_gcd = gcd_i64(obs_gcd, obs_arr[ii]);
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

                        if (present_count > 0)
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
    const struct sig_cache *sc)
{
    size_t packed = 0;
    int order = 0;
    int64_t whole_cost = 0;

    if (sc->obs_count == 0)
        return 0;

    if (sc->present_count > 0)
    {
        order = select_delta_order_cached(scratch, sc->delta_matrix,
            sc->all_bw, sc->present_count, &whole_cost);
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
            payload = socd_payload_bytes_cached(scratch, sc);
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
        if (endp == p || v <= 0 || (v % 8) != 0 || v > MAX_BLOCK)
        {
            fprintf(stderr, "Bad tuple value at '%s' (must be 8*N, 8..%d)\n",
                p, MAX_BLOCK);
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

/* ---- Digest flag parser ---- */
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

/* ---- main ---- */
int main(int argc, char *argv[])
{
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
        fprintf(stderr,
            "Usage: %s [--chunk-digest=<id>] [--file-digest=<id>]\n"
            "          [--tuple=N1,N2,...] [--tuples-file=PATH]\n"
            "          [--timing] <input> [<input> ...]\n",
            argv[0]);
        return EXIT_FAILURE;
    }
    if (tuples.n == 0)
    {
        fprintf(stderr,
            "No tuples specified; use --tuple= or --tuples-file=.\n");
        return EXIT_FAILURE;
    }

    for (ii = first_pos_arg; ii < argc; ++ii)
    {
        struct file_ctx ctx;
        struct timespec t0, t1, t2;
        int tt;

        clock_gettime(CLOCK_MONOTONIC, &t0);
        if (ctx_load(&ctx, argv[ii]) != 0)
        {
            ctx_free(&ctx);
            continue;
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);

        for (tt = 0; tt < tuples.n; ++tt)
        {
            char tup_buf[256];
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
    return EXIT_SUCCESS;
}
