/** rnx2srnx.c - Convert RINEX files to Succinct RINEX format.
 * Copyright 2021 Michael Poole.
 * SPDX-License-Identifier: MIT-Modern-Variant
 */

#include "rinex/digest.h"
#include "rinex/rinex_load.h"

#include <ctype.h>
#include <fcntl.h>
#include <limits.h>
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

/* ---- Memory-mapped output buffer ---- */

struct mmbuf
{
    unsigned char *data;
    size_t used;
    size_t cap;
};

static void mm_require(struct mmbuf *mm, size_t need)
{
    if (mm->used + need > mm->cap)
    {
        fprintf(stderr, "Output exceeds mmap capacity "
            "(used=%zu need=%zu cap=%zu)\n",
            mm->used, need, mm->cap);
        exit(EXIT_FAILURE);
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
        fprintf(stderr, "Out of memory\n");
        exit(EXIT_FAILURE);
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

static int sleb128_len(int64_t val)
{
    uint64_t uv = ((uint64_t)val << 1) ^ (uint64_t)(val >> 63);
    return uleb128_len(uv);
}

/* ---- Zigzag bit-width ---- */

/** Return minimum bits for two's-complement representation of \a val. */
static int twos_comp_bw(int64_t val)
{
    uint64_t v = (val >= 0) ? (uint64_t)val : (uint64_t)(~val);
    return v == 0 ? 1 : (65 - __builtin_clzll(v));
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
        fprintf(stderr, "Unsupported chunk digest id=%d\n", g_chunk_digest_id);
        exit(EXIT_FAILURE);
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

static void rle_encode_indicators(struct wbuf *wb, const char *ind, int count)
{
    struct wbuf rle;
    int ii, run_start;
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
        wbuf_byte(&rle, (unsigned char)cur);
        wbuf_uleb128(&rle, ii - run_start - 1);
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

/** Compute 8 * count delta values.
 * d[ord * count + i] = ord-th order difference at index i,
 * where order 0 is the raw scaled values (with d[ord][-1] = 0 implied).
 */
static int64_t *compute_delta_matrix(const int64_t *scaled, int count)
{
    int64_t *d = malloc(8 * (size_t)count * sizeof *d);
    int ord, ii;

    if (!d)
    {
        fprintf(stderr, "Out of memory\n");
        exit(EXIT_FAILURE);
    }

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

    return d;
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

/** Compute optimal byte cost to encode \a vals[0..count-1] as SOCD blocks.
 *
 * If \a choices is non-NULL, it must have room for \a count entries; on
 * return it is populated such that walking forward from index 0 by
 * \c choices[i].len steps reproduces the optimal block sequence.
 *
 * Runs in O(count) time: bit-widths are computed once, and the per-window
 * max over each candidate block size m ∈ {8,16,32,64,128} is built by
 * doubling (mx[k+1][i] = max(mx[k][i], mx[k][i + 2^k])).
 */
static int64_t compute_block_dp(const int64_t *vals, int count,
    struct block_choice *choices)
{
    static const int cand[] = {8, 16, 32, 64, 128};
    /* Window-max tables; mx[k] is the max bit-width over a window of size
     * 2^(k+3) starting at each position.  Indexed by ci = 0..4 corresponding
     * to m = 8..128.  Uses uint8_t since bit-widths fit in 7 bits. */
    uint8_t *bw_arr;
    uint8_t *mx[5];
    int64_t *dp;
    int64_t result, cost, payload;
    size_t bw_bytes;
    int ii, ci, zlen_here, zlen_next, L, max_L;

    if (count <= 0)
        return 0;

    /* Single allocation: bw[] + 5 mx[] windows (8-byte aligned for dp) + dp. */
    bw_bytes = ((size_t)count * 6 + 7) & ~(size_t)7;
    bw_arr = malloc(bw_bytes + (size_t)(count + 1) * sizeof(int64_t));
    for (ci = 0; ci < 5; ++ci)
        mx[ci] = bw_arr + (size_t)count * (ci + 1);
    dp = (int64_t *)(bw_arr + bw_bytes);

    /* Precompute bit-widths. */
    for (ii = 0; ii < count; ++ii)
    {
        int bw = twos_comp_bw(vals[ii]);
        if (bw > 255) bw = 255;
        bw_arr[ii] = (uint8_t)bw;
    }

    /* Window of size 8 (ci=0): scan 8 values per position.  This is the
     * one O(count*8) step; subsequent windows double in O(count). */
    for (ii = 0; ii + 8 <= count; ++ii)
    {
        uint8_t m = bw_arr[ii];
        int kk;
        for (kk = 1; kk < 8; ++kk)
            if (bw_arr[ii + kk] > m)
                m = bw_arr[ii + kk];
        mx[0][ii] = m;
    }

    /* Doubling for ci = 1..4 (m = 16, 32, 64, 128). */
    for (ci = 1; ci < 5; ++ci)
    {
        int half = cand[ci - 1]; /* size of previous window */
        int m = cand[ci];
        for (ii = 0; ii + m <= count; ++ii)
        {
            uint8_t a = mx[ci - 1][ii];
            uint8_t b = mx[ci - 1][ii + half];
            mx[ci][ii] = a > b ? a : b;
        }
    }

    dp[count] = 0;

    /* zlen_next = length of the maximal zero run starting at ii+1,
     * maintained as we walk right-to-left.  Avoids O(N) re-scanning at
     * every position (which would be O(N^2) on constant signals). */
    zlen_next = 0;

    for (ii = count - 1; ii >= 0; --ii)
    {
        int remaining = count - ii;
        int64_t best = INT64_MAX;
        struct block_choice best_choice = {BK_SLEB, 0, 1};

        /* Block transitions.  cand[] is ascending so we can stop once
        * remaining < m. */
        for (ci = 0; ci < 5; ++ci)
        {
            int m = cand[ci];
            int max_bw;

            if (remaining < m)
                break;
            max_bw = mx[ci][ii];
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

        /* SLEB128-run transition: consider every length 1..min(remaining,16). */
        max_L = remaining < 16 ? remaining : 16;
        payload = 0;
        for (L = 1; L <= max_L; ++L)
        {
            int64_t cost;
            payload += sleb128_len(vals[ii + L - 1]);
            cost = 1 + uleb128_len(L - 1) + payload + dp[ii + L];
            if (cost < best)
            {
                best = cost;
                best_choice.kind = BK_SLEB;
                best_choice.bw = 0;
                best_choice.len = L;
            }
        }

        dp[ii] = best;
        if (choices)
            choices[ii] = best_choice;
    }

    result = dp[0];
    free(bw_arr);
    return result;
}

/* ---- Delta order selection ---- */

/** Estimate encoded size (bytes) of a sequence of steady-state delta values
 * using the same DP-based block selection as the encoder.
 */
static int64_t estimate_steady_state_cost(const int64_t *vals, int count)
{
    return compute_block_dp(vals, count, NULL);
}

/** Select the best delta order (0..7) for a pre-computed delta matrix. */
static int select_delta_order(const int64_t *delta_matrix, int count)
{
    int best_order = 0;
    int64_t best_cost = estimate_steady_state_cost(delta_matrix, count);
    int ord;

    for (ord = 1; ord <= 7 && ord < count; ++ord)
    {
        const int64_t *ord_vals = delta_matrix + (size_t)ord * count;
        int64_t cost = 0;

        /* Cost of zero-valued SLEB128 init values: 1 byte each. */
        cost += ord;

        /* Cost of block-encoded values d[ord][0..count-1]. */
        cost += estimate_steady_state_cost(ord_vals, count);

        if (cost < best_cost)
        {
            best_cost = cost;
            best_order = ord;
        }
    }

    return best_order;
}

/* ---- Bit-matrix transposition (encode direction) ---- */

/** Write a bit-transposed block of \a count values, each \a bits wide.
 * count must be 16 or 32.
 */
static void write_transposed_block(struct wbuf *wb, const int64_t *vals,
    int count, int bits)
{
    int row, col, byte_idx;
    int bytes_per_row = count >> 3;
    char matrix[64 * 8]; /* max 32 bits * 16 bytes/row for count=128 = 512 bytes */

    memset(matrix, 0, bits * bytes_per_row);

    for (row = 0; row < bits; ++row)
    {
        int shift = bits - 1 - row;
        for (col = 0; col < count; ++col)
        {
            uint64_t tc = (uint64_t)vals[col];
            byte_idx = col >> 3;
            if ((tc >> shift) & 1)
                matrix[row * bytes_per_row + byte_idx] |= (char)(0x80 >> (col & 7));
        }
    }

    wbuf_append(wb, matrix, bits * bytes_per_row);
}

/* ---- Encode packed observation data ---- */

/** Encode a run of \a run_len present delta values from \a ord_vals into \a packed. */
static void encode_delta_run(struct wbuf *packed,
    const int64_t *ord_vals, int run_len)
{
    struct block_choice *choices;
    int ii;

    if (run_len <= 0)
        return;

    choices = malloc((size_t)run_len * sizeof *choices);
    compute_block_dp(ord_vals, run_len, choices);

    ii = 0;
    while (ii < run_len)
    {
        struct block_choice c = choices[ii];

        switch (c.kind)
        {
        case BK_BLOCK:
        {
            unsigned char header;
            int log2m =
                c.len ==   8 ? 0 :
                c.len ==  16 ? 1 :
                c.len ==  32 ? 2 :
                c.len ==  64 ? 3 : 4; /* c.len == 128 */
            header = (unsigned char)(log2m << 5)
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

    free(choices);
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
    const int64_t *full_obs, int total_count,
    const int64_t *delta_matrix, int present_count, int order, int64_t obs_gcd)
{
    struct wbuf packed;
    const int64_t *ord_vals;
    int ii, pi;

    ord_vals = (delta_matrix && present_count > 0)
        ? delta_matrix + (size_t)order * present_count
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
        {
            int run_end = ii;
            int run_len;
            while (run_end < total_count && full_obs[run_end] != INT64_MIN)
                run_end++;
            run_len = run_end - ii;
            encode_delta_run(&packed, ord_vals + pi, run_len);
            pi += run_len;
            ii = run_end;
        }
    }

    wbuf_uleb128(wb, packed.used);
    wbuf_append(wb, packed.data, packed.used);
    wbuf_free(&packed);
}

/* ---- Write SOCD chunk ---- */

/** Write a SOCD chunk for one (satellite, signal) pair.
 * Returns the offset (within \a mm) where the SOCD chunk starts.
 */
static size_t write_socd_chunk(struct mmbuf *mm, const struct rinex_data *data,
    const struct rinex_satellite_data *sv, int sig_idx,
    const struct rinex_system_data *p_sys)
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
    rle_encode_indicators(&payload, lli_arr, obs_count);
    rle_encode_indicators(&payload, ssi_arr, obs_count);

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
        int64_t *full_scaled = malloc(obs_count * sizeof *full_scaled);
        int64_t *present_scaled = malloc(obs_count * sizeof *present_scaled);
        int64_t *delta_matrix = NULL;
        int present_count = 0;
        int order = 0;

        if (!full_scaled || !present_scaled)
        {
            free(full_scaled);
            free(present_scaled);
            fprintf(stderr, "Out of memory\n");
            exit(EXIT_FAILURE);
        }

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
            delta_matrix = compute_delta_matrix(present_scaled, present_count);
            order = select_delta_order(delta_matrix, present_count);
        }
        free(present_scaled);

        encode_packed_observations(&payload, full_scaled, obs_count,
            delta_matrix, present_count, order, obs_gcd);

        free(full_scaled);
        free(delta_matrix);
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
        fprintf(stderr, "SRNX payload overflow\n");
        exit(EXIT_FAILURE);
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
            fprintf(stderr, "Unsupported chunk digest id=%d\n",
                g_chunk_digest_id);
            exit(EXIT_FAILURE);
        }
    }
}

/* ---- Append file-level digest ---- */

/** Computes the file-level digest over bytes [start_offset, mm->used)
 * and appends the digest to \a mm.  Does nothing if the file-digest id
 * is null.
 */
static void append_file_digest(struct mmbuf *mm, size_t start_offset)
{
    int dig_len;

    dig_len = rnx_digest_length(g_file_digest_id);
    if (dig_len <= 0)
        return;
    mm_require(mm, (size_t)dig_len);
    if (rnx_digest(g_file_digest_id,
            mm->data + start_offset, mm->used - start_offset,
            mm->data + mm->used) < 0)
    {
        fprintf(stderr, "Unsupported file digest id=%d\n", g_file_digest_id);
        exit(EXIT_FAILURE);
    }
    mm->used += (size_t)dig_len;
}

/* ---- Main converter ---- */

static void rnx2srnx(const char input_name[], const char output_name[])
{
    struct rinex_data data;
    struct mmbuf mm = { NULL, 0, 0 };
    struct stat st;
    size_t cap;
    int fd = -1;
    const char *err;
    size_t sdir_field_offset, epoc_offset, evtf_offset, sdir_offset;
    struct sdir_entry *sdir_entries = NULL;
    int n_sdir = 0, sdir_alloc = 0;
    int sys_idx;

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
                socd_offsets[jj] = write_socd_chunk(&mm, &data, p_sv, jj, p_sys);

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

    /* 8. Compute and append the file-level digest, if enabled.
     * The digest covers all bytes after the SRNX chunk (header + payload
     * + the SRNX chunk's own trailing digest).
     */
    {
        int chunk_dig = rnx_digest_length(g_chunk_digest_id);
        size_t srnx_end = 5 + SRNX_PAYLOAD_SIZE
            + (chunk_dig > 0 ? (size_t)chunk_dig : 0);
        append_file_digest(&mm, srnx_end);
    }

done:
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
            "<input.rnx> [output.srnx]\n", argv[0]);
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
