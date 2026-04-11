/** rnx2srnx.c - Convert RINEX files to Succinct RINEX format.
 * Copyright 2021 Michael Poole.
 * SPDX-License-Identifier: MIT-Modern-Variant
 */

#include "rinex/rinex_load.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Growable write buffer ---- */

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

/* ---- Write a chunk to a FILE ---- */

static void write_chunk(FILE *fp, const char fourcc[4],
    const void *payload, size_t payload_len)
{
    struct wbuf hdr;
    wbuf_init(&hdr);
    wbuf_append(&hdr, fourcc, 4);
    wbuf_uleb128(&hdr, payload_len);
    fwrite(hdr.data, 1, hdr.used, fp);
    fwrite(payload, 1, payload_len, fp);
    wbuf_free(&hdr);
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

/* ---- Delta order selection ---- */

/** Estimate encoded size (bytes) of a sequence of steady-state delta values
 * using the same greedy block selection as the encoder.
 */
static int64_t estimate_steady_state_cost(const int64_t *vals, int count)
{
    static const int cand[] = {8, 16, 32, 64};
    int64_t cost = 0;
    int ii = 0;

    while (ii < count)
    {
        int remaining = count - ii;
        int best_m = 0, best_bw = 0;
        double best_cost_per = 1e30;
        int ci;

        /* Zero run. */
        {
            int zlen = 0;
            while (ii + zlen < count && vals[ii + zlen] == 0)
                zlen++;
            if (zlen >= 16)
            {
                cost += 1 + uleb128_len(zlen - 1);
                ii += zlen;
                continue;
            }
        }

        for (ci = 0; ci < 4; ++ci)
        {
            int m = cand[ci], max_bw = 0, kk;
            double cost_per;

            if (remaining < m)
                continue;
            for (kk = 0; kk < m; ++kk)
            {
                int bw = twos_comp_bw(vals[ii + kk]);
                if (bw > max_bw)
                    max_bw = bw;
            }
            if (max_bw > 32)
                continue;
            cost_per = (1.0 + (double)max_bw * m / 8.0) / m;
            if (cost_per < best_cost_per)
            {
                best_cost_per = cost_per;
                best_m = m;
                best_bw = max_bw;
            }
        }

        if (best_m > 0)
        {
            cost += 1 + (int64_t)best_bw * best_m / 8;
            ii += best_m;
        }
        else
        {
            int run_len = remaining < 16 ? remaining : 16;
            int jj;
            cost += 1 + uleb128_len(run_len - 1);
            for (jj = 0; jj < run_len; ++jj)
                cost += sleb128_len(vals[ii + jj]);
            ii += run_len;
        }
    }

    return cost;
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
    char matrix[64 * 8]; /* max 64 bits * 8 bytes/row */

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
    static const int cand[] = {8, 16, 32, 64};
    int ii, jj;

    ii = 0;
    while (ii < run_len)
    {
        int remaining = run_len - ii;
        int best_m = 0, best_bw = 0;
        double best_cost_per = 1e30;
        int ci;

        /* Zero run. */
        {
            int zlen = 0;
            while (ii + zlen < run_len && ord_vals[ii + zlen] == 0)
                zlen++;
            if (zlen >= 16)
            {
                wbuf_byte(packed, 0xFE);
                wbuf_uleb128(packed, zlen - 1);
                ii += zlen;
                continue;
            }
        }

        /* Evaluate candidate block sizes. */
        for (ci = 0; ci < 4; ++ci)
        {
            int m = cand[ci], max_bw = 0, kk;
            double cost_per;

            if (remaining < m)
                continue;
            for (kk = 0; kk < m; ++kk)
            {
                int bw = twos_comp_bw(ord_vals[ii + kk]);
                if (bw > max_bw)
                    max_bw = bw;
            }
            if (max_bw > 32)
                continue;
            cost_per = (8.0 + (double)max_bw * m) / m;
            if (cost_per < best_cost_per)
            {
                best_cost_per = cost_per;
                best_m = m;
                best_bw = max_bw;
            }
        }

        if (best_m > 0)
        {
            unsigned char header;
            if (best_m == 8)
                header = 0x00 | (unsigned char)(best_bw - 1);
            else if (best_m == 16)
                header = 0x20 | (unsigned char)(best_bw - 1);
            else if (best_m == 32)
                header = 0x40 | (unsigned char)(best_bw - 1);
            else
                header = 0x60 | (unsigned char)(best_bw - 1);
            wbuf_byte(packed, header);
            write_transposed_block(packed, ord_vals + ii, best_m, best_bw);
            ii += best_m;
        }
        else
        {
            int run_len2 = remaining < 16 ? remaining : 16;
            wbuf_byte(packed, 0xFF);
            wbuf_uleb128(packed, run_len2 - 1);
            for (jj = 0; jj < run_len2; ++jj)
                wbuf_sleb128(packed, ord_vals[ii + jj]);
            ii += run_len2;
        }
    }
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
 * Returns the file offset where the SOCD chunk starts.
 */
static size_t write_socd_chunk(FILE *fp, const struct rinex_data *data,
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
    offset = ftell(fp);
    write_chunk(fp, "SOCD", payload.data, payload.used);
    wbuf_free(&payload);
    return offset;
}

/* ---- Write SATE chunk ---- */

static size_t write_sate_chunk(FILE *fp,
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

    offset = ftell(fp);
    write_chunk(fp, "SATE", payload.data, payload.used);
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

static size_t write_epoc_chunk(FILE *fp, const struct rinex_data *data)
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
        offset = ftell(fp);
        write_chunk(fp, "EPOC", payload.data, payload.used);
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

    offset = ftell(fp);
    write_chunk(fp, "EPOC", payload.data, payload.used);
    wbuf_free(&payload);
    return offset;
}

/* ---- Write SRNX header ---- */

#define SRNX_PAYLOAD_SIZE 16

/** Writes the SRNX header chunk. Returns the file offset of the
 * SDIR-offset field within the payload (for later patching).
 */
static size_t write_srnx_header(FILE *fp)
{
    char payload[SRNX_PAYLOAD_SIZE];
    struct wbuf hdr;
    size_t sdir_field_offset;

    memset(payload, 0, sizeof payload);

    /* Build payload into a fixed-size buffer.
     * Fields: major=1, minor=0, chunk_digest=0, file_digest=0,
     * sdir_offset=0 (patched later), padding.
     */
    {
        struct wbuf tmp;
        wbuf_init(&tmp);
        wbuf_uleb128(&tmp, 1); /* major */
        wbuf_uleb128(&tmp, 0); /* minor */
        wbuf_uleb128(&tmp, 0); /* chunk digest */
        wbuf_uleb128(&tmp, 0); /* file digest */
        /* sdir_offset starts here; leave as zeros for patching */
        sdir_field_offset = tmp.used; /* offset within payload */
        if (tmp.used > SRNX_PAYLOAD_SIZE)
        {
            fprintf(stderr, "SRNX payload overflow\n");
            exit(EXIT_FAILURE);
        }
        memcpy(payload, tmp.data, tmp.used);
        wbuf_free(&tmp);
    }

    /* Write FOURCC + length + payload. */
    wbuf_init(&hdr);
    wbuf_append(&hdr, "SRNX", 4);
    wbuf_uleb128(&hdr, SRNX_PAYLOAD_SIZE);
    sdir_field_offset += hdr.used; /* now relative to file start */
    fwrite(hdr.data, 1, hdr.used, fp);
    fwrite(payload, 1, SRNX_PAYLOAD_SIZE, fp);
    wbuf_free(&hdr);

    return sdir_field_offset;
}

/* ---- Write SDIR chunk ---- */

struct sdir_entry
{
    char name[4];
    size_t sate_offset;
};

static size_t write_sdir_chunk(FILE *fp, size_t epoc_offset,
    const struct sdir_entry *entries, int n_entries)
{
    struct wbuf payload;
    size_t offset;
    int ii;

    wbuf_init(&payload);
    wbuf_uleb128(&payload, epoc_offset);
    wbuf_uleb128(&payload, 0); /* no EVTF chunks */

    for (ii = 0; ii < n_entries; ++ii)
    {
        wbuf_append(&payload, entries[ii].name, 3);
        wbuf_uleb128(&payload, entries[ii].sate_offset);
    }

    offset = ftell(fp);
    write_chunk(fp, "SDIR", payload.data, payload.used);
    wbuf_free(&payload);
    return offset;
}

/* ---- Patch SRNX header with SDIR offset ---- */

static void patch_srnx_sdir(FILE *fp, size_t sdir_field_offset,
    size_t sdir_offset)
{
    unsigned char buf[10];
    int len = 0;
    uint64_t val = sdir_offset;

    /* Encode ULEB128 into buf. */
    do {
        unsigned char ch = val & 127;
        val >>= 7;
        if (val)
            ch |= 128;
        buf[len++] = ch;
    } while (val);

    fseek(fp, sdir_field_offset, SEEK_SET);
    fwrite(buf, 1, len, fp);
}

/* ---- Main converter ---- */

static void rnx2srnx(const char input_name[], const char output_name[])
{
    struct rinex_data data;
    FILE *fp;
    const char *err;
    size_t sdir_field_offset, epoc_offset, sdir_offset;
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

    /* Open the output file. */
    fp = fopen(output_name, "wb");
    if (!fp)
    {
        fprintf(stderr, "Unable to create %s\n", output_name);
        free_rinex_data(&data);
        return;
    }

    /* 1. Write SRNX header (with placeholder SDIR offset). */
    sdir_field_offset = write_srnx_header(fp);

    /* 2. Write RHDR chunk. */
    write_chunk(fp, "RHDR", data.file_header, data.file_header_len);

    /* 3. Write EPOC chunk. */
    epoc_offset = write_epoc_chunk(fp, &data);

    /* 4. For each satellite, write SOCD chunks then SATE chunk. */
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
                socd_offsets[jj] = write_socd_chunk(fp, &data, p_sv, jj, p_sys);

            /* Write SATE chunk. */
            sate_pos = ftell(fp);
            sate_offset = write_sate_chunk(fp, p_sv, n_obs,
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

    /* 5. Write SDIR chunk. */
    sdir_offset = write_sdir_chunk(fp, epoc_offset, sdir_entries, n_sdir);

    /* 6. Patch SRNX header with SDIR offset. */
    patch_srnx_sdir(fp, sdir_field_offset, sdir_offset);

done:
    fclose(fp);
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

int main(int argc, char *argv[])
{
    const char *input_name;
    char *output_name;
    size_t name_len;

    if (argc < 2)
    {
        fprintf(stderr, "Usage: %s <input.rnx> [output.srnx]\n", argv[0]);
        return EXIT_FAILURE;
    }

    input_name = argv[1];
    output_name = (argc > 2) ? strdup(argv[2]) : NULL;
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
