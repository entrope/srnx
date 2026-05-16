/** srnx_split.c - Split an SRNX file into per-category byte streams.
 * Copyright 2026 Michael Poole.
 * SPDX-License-Identifier: MIT-Modern-Variant
 *
 * Walks an SRNX file chunk-by-chunk (and inside SOCD chunks,
 * block-by-block) and writes every byte to one of a fixed set of
 * per-category output files in append mode.  Per-category and total
 * byte counts are reported on stdout.
 *
 * The intent is to localize where residual entropy lives inside SRNX:
 * by feeding each output stream alone through a general-purpose
 * compressor (kanzi, bzip3, zstd, ...) we can see which constituent
 * streams contribute the gap between SRNX and SRNX+post-compression.
 *
 * Output files are opened with O_APPEND.  Re-running the tool against
 * the same directory accumulates more bytes to each stream; the
 * caller is expected to clear the output directory before starting a
 * fresh measurement run.
 */

#include "rinex/digest.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#if !defined(O_CLOEXEC)
# define O_CLOEXEC 0
#endif

/* ---- ULEB128 / SLEB128 readers (copied from srnx_scan.c) ---- */

static uint64_t read_uleb128(const char **p)
{
    uint64_t result = 0;
    int shift = 0;
    unsigned char byte;
    do {
        byte = **(unsigned char **)p;
        (*p)++;
        result |= (uint64_t)(byte & 0x7F) << shift;
        shift += 7;
    } while (byte & 0x80);
    return result;
}

static int64_t read_sleb128(const char **p)
{
    uint64_t ul = read_uleb128(p);
    return (int64_t)(ul >> 1) ^ -(int64_t)(ul & 1);
}

/* ---- Categories ---- */

enum cat {
    CAT_CHUNK_FRAMES,   /* 4-byte FOURCC + ULEB128 payload length, every chunk */
    CAT_DIGESTS,        /* per-chunk and file-level digest bytes */
    CAT_SRNX_HDR,       /* SRNX chunk payload (5 ULEB128s + padding) */
    CAT_RHDR,           /* RHDR chunk payload (RINEX text header) */
    CAT_EPOC,           /* EPOC chunk payload */
    CAT_SDIR,           /* SDIR chunk payload */
    CAT_EVTF,           /* EVTF chunk payloads */
    CAT_SATE_PRESENCE,  /* SATE chunk payloads (name + signal offsets + presence RLE) */
    CAT_SOCD_META,      /* SOCD: name, count-1, length prefixes, schema, scale, initial deltas */
    CAT_SOCD_LLI,       /* SOCD LLI RLE bytes */
    CAT_SOCD_SSI,       /* SOCD SSI RLE bytes */
    CAT_SOCD_LLI_RAW,   /* SOCD LLI bytes, RLE-decoded, padded to obs_count */
    CAT_SOCD_SSI_RAW,   /* SOCD SSI bytes, RLE-decoded, padded to obs_count */
    CAT_SOCD_HEADERS,   /* SOCD block-header bytes (1 per block) */
    CAT_SOCD_MATRIX,    /* SOCD transposed bit-matrix payload bytes */
    CAT_SOCD_ABSENT,    /* SOCD 0xFD block bodies (ULEB128 count) */
    CAT_SOCD_ZERO,      /* SOCD 0xFE block bodies (ULEB128 count) */
    CAT_SOCD_SLEB,      /* SOCD 0xFF block bodies (ULEB128 count + N SLEB128s) */
    CAT_OTHER,          /* unrecognized chunk payloads */
    N_CAT
};

static const char *cat_names[N_CAT] = {
    "chunk_frames.bin",
    "digests.bin",
    "srnx_hdr.bin",
    "rhdr.bin",
    "epoc.bin",
    "sdir.bin",
    "evtf.bin",
    "sate_presence.bin",
    "socd_meta.bin",
    "socd_lli.bin",
    "socd_ssi.bin",
    "socd_lli_raw.bin",
    "socd_ssi_raw.bin",
    "socd_headers.bin",
    "socd_matrix.bin",
    "socd_absent.bin",
    "socd_zero.bin",
    "socd_sleb.bin",
    "other.bin",
};

static FILE *cat_fp[N_CAT];
static uint64_t cat_total[N_CAT];   /* across all input files */
static uint64_t cat_file[N_CAT];    /* this input file only */

/* Whether to write the raw (RLE-decoded) LLI/SSI streams.  These can be
 * large for a big corpus — one byte per epoch per signal per satellite. */
static int g_write_raw_indicators = 0;

/* Indicator alphabet for the proposed packed RLE encoding. */
static const char ALPHABET[] = " 0123456789";  /* 11 symbols */

/* Per-byte histograms of decoded indicator bytes (LLI, SSI). */
static uint64_t lli_byte_hist[256];
static uint64_t ssi_byte_hist[256];

/* Run-length bucket counters.  Bucket boundaries correspond to ULEB128
 * width transitions for the proposed (count-1)*11 + idx packed encoding:
 *   bucket 0: count in [1,    11]  — saves 1 byte/run (2 -> 1)
 *   bucket 1: count in [12,  128]  — saves 0 bytes    (2 -> 2)
 *   bucket 2: count in [129,1489]  — saves 1 byte/run (3 -> 2)
 *   bucket 3: count in [1490,16384]— saves 0 bytes    (3 -> 3)
 *   bucket 4: count >= 16385       — saves 1 byte/run (4 -> 3)
 */
#define N_BUCKETS 5
static const int bucket_saves[N_BUCKETS] = { 1, 0, 1, 0, 1 };
static uint64_t lli_run_bucket[N_BUCKETS];
static uint64_t ssi_run_bucket[N_BUCKETS];
static uint64_t lli_total_runs;
static uint64_t ssi_total_runs;
/* Runs whose symbol is outside the 11-character alphabet. */
static uint64_t lli_runs_out_of_alphabet;
static uint64_t ssi_runs_out_of_alphabet;
static uint64_t lli_bytes_out_of_alphabet;
static uint64_t ssi_bytes_out_of_alphabet;

/* Reusable decode buffer for one SOCD's raw indicator stream. */
static char *g_decode_buf;
static size_t g_decode_buf_cap;

static char *ensure_decode_buf(size_t needed)
{
    if (needed > g_decode_buf_cap)
    {
        size_t new_cap = g_decode_buf_cap ? g_decode_buf_cap * 2 : 4096;
        while (new_cap < needed) new_cap *= 2;
        char *p = (char *)realloc(g_decode_buf, new_cap);
        if (!p)
        {
            fprintf(stderr, "realloc(%zu): %s\n", new_cap, strerror(errno));
            exit(EXIT_FAILURE);
        }
        g_decode_buf = p;
        g_decode_buf_cap = new_cap;
    }
    return g_decode_buf;
}

static int bucket_for_count(uint64_t count)
{
    if (count <=    11) return 0;
    if (count <=   128) return 1;
    if (count <=  1489) return 2;
    if (count <= 16384) return 3;
    return 4;
}

static int in_alphabet(unsigned char ch)
{
    return ch == ' ' || (ch >= '0' && ch <= '9');
}

/** Decode an LLI/SSI RLE block into raw bytes and update stats.
 * \a obs_count is the number of observations in the SOCD; output is
 * padded to that length with spaces if the RLE stream is shorter.
 * Returns the decoded buffer (owned by ensure_decode_buf).
 * Each run is a single ULEB128: val = (count-1)*11 + idx where
 * idx indexes ALPHABET[].  The parameters runs_oo / bytes_oo are kept
 * for API consistency but will always remain zero (new format is
 * alphabet-constrained by construction).
 */
static char *decode_indicator(const char *rle, uint64_t rle_len,
    uint64_t obs_count,
    uint64_t *byte_hist, uint64_t *run_bucket, uint64_t *total_runs,
    uint64_t *runs_oo, uint64_t *bytes_oo)
{
    char *out = ensure_decode_buf((size_t)obs_count + 1);
    const char *p = rle;
    const char *end = rle + rle_len;
    uint64_t decoded = 0;

    (void)runs_oo;
    (void)bytes_oo;

    while (p < end && decoded < obs_count)
    {
        uint64_t val = read_uleb128(&p);
        unsigned char ch = (unsigned char)ALPHABET[val % 11];
        uint64_t count = val / 11 + 1;
        if (decoded + count > obs_count)
            count = obs_count - decoded;

        byte_hist[ch] += count;
        (*total_runs)++;
        run_bucket[bucket_for_count(count)]++;

        memset(out + decoded, ch, (size_t)count);
        decoded += count;
    }

    /* Trailing implicit spaces — present in the semantic stream but not
     * stored, so they don't affect savings estimates and aren't
     * counted in the histograms. */
    if (decoded < obs_count)
        memset(out + decoded, ' ', (size_t)(obs_count - decoded));

    return out;
}

/** Synthetic categories hold bytes that are not part of the input file
 * (they are RLE-decoded reconstructions).  They are excluded from the
 * per-file invariant check and only emitted when -r is given. */
static int is_synthetic_cat(enum cat c)
{
    return c == CAT_SOCD_LLI_RAW || c == CAT_SOCD_SSI_RAW;
}

static void cat_write(enum cat c, const void *p, size_t len)
{
    if (len == 0) return;
    if (is_synthetic_cat(c) && !g_write_raw_indicators) return;
    if (fwrite(p, 1, len, cat_fp[c]) != len)
    {
        fprintf(stderr, "write to %s failed: %s\n",
            cat_names[c], strerror(errno));
        exit(EXIT_FAILURE);
    }
    if (!is_synthetic_cat(c))
    {
        cat_file[c] += len;
        cat_total[c] += len;
    }
}

static void open_cat_files(const char *outdir)
{
    if (mkdir(outdir, 0755) < 0 && errno != EEXIST)
    {
        fprintf(stderr, "mkdir(%s): %s\n", outdir, strerror(errno));
        exit(EXIT_FAILURE);
    }

    for (int i = 0; i < N_CAT; i++)
    {
        if (is_synthetic_cat((enum cat)i) && !g_write_raw_indicators)
            continue;
        char path[4096];
        snprintf(path, sizeof path, "%s/%s", outdir, cat_names[i]);
        cat_fp[i] = fopen(path, "ab");
        if (!cat_fp[i])
        {
            fprintf(stderr, "fopen(%s): %s\n", path, strerror(errno));
            exit(EXIT_FAILURE);
        }
    }
}

static void close_cat_files(void)
{
    for (int i = 0; i < N_CAT; i++)
    {
        if (cat_fp[i]) fclose(cat_fp[i]);
        cat_fp[i] = NULL;
    }
}

/* ---- Chunk walker ---- */

/** Find the next chunk at or after offset.
 * Returns the file offset of the chunk, or -1 if none can be found.
 * Sets *p_fourcc, *p_len, *p_payload_start on success.
 */
static int64_t find_next_chunk(
    const char *data, uint64_t limit, int64_t offset,
    const char **p_fourcc, uint64_t *p_len,
    const char **p_payload_start, int digest_length)
{
    if (offset < 0 || (uint64_t)(offset + 5) > limit)
        return -1;

    const char *rptr = data + offset + 4;
    uint64_t len = read_uleb128(&rptr);

    int64_t payload_start = (int64_t)(rptr - data);
    int64_t chunk_end = payload_start + (int64_t)len + digest_length;
    if (chunk_end < 0 || (uint64_t)chunk_end > limit)
    {
        fprintf(stderr, "  WARNING: chunk at offset %" PRId64
            " (%.4s) would extend past end of file\n",
            offset, data + offset);
        return -1;
    }

    *p_fourcc = data + offset;
    *p_len = len;
    *p_payload_start = rptr;
    return offset;
}

/** Split a single SOCD payload into per-category streams. */
static void split_socd(const char *payload, uint64_t plen)
{
    const char *p = payload;
    const char *end = payload + plen;

    /* SOCD meta: 8-byte name + count-1 ULEB + lli_len ULEB + ssi_len ULEB +
     * packed_len ULEB + schema ULEB + optional scale ULEB + N initial SLEBs.
     * We accumulate the byte range as we parse so we can issue one write
     * per logical section. */

    /* 8-byte name. */
    const char *meta_start = p;
    p += 8;

    /* count-minus-1 ULEB128. */
    uint64_t obs_count = read_uleb128(&p) + 1;

    /* LLI: ULEB128 length, then that many bytes of RLE. */
    uint64_t lli_len = read_uleb128(&p);
    cat_write(CAT_SOCD_META, meta_start, (size_t)(p - meta_start));
    cat_write(CAT_SOCD_LLI, p, (size_t)lli_len);
    {
        char *raw = decode_indicator(p, lli_len, obs_count,
            lli_byte_hist, lli_run_bucket, &lli_total_runs,
            &lli_runs_out_of_alphabet, &lli_bytes_out_of_alphabet);
        cat_write(CAT_SOCD_LLI_RAW, raw, (size_t)obs_count);
    }
    p += lli_len;

    /* SSI: ULEB128 length, then that many bytes of RLE. */
    const char *ssi_len_start = p;
    uint64_t ssi_len = read_uleb128(&p);
    cat_write(CAT_SOCD_META, ssi_len_start, (size_t)(p - ssi_len_start));
    cat_write(CAT_SOCD_SSI, p, (size_t)ssi_len);
    {
        char *raw = decode_indicator(p, ssi_len, obs_count,
            ssi_byte_hist, ssi_run_bucket, &ssi_total_runs,
            &ssi_runs_out_of_alphabet, &ssi_bytes_out_of_alphabet);
        cat_write(CAT_SOCD_SSI_RAW, raw, (size_t)obs_count);
    }
    p += ssi_len;

    /* Packed observation data. */
    const char *packed_len_start = p;
    uint64_t packed_len = read_uleb128(&p);
    const char *packed_end = p + packed_len;

    uint64_t schema = read_uleb128(&p);
    int delta_order = (int)(schema & 7);
    int has_scale = (int)(schema & 8);
    if (has_scale)
        read_uleb128(&p);

    for (int i = 0; i < delta_order; i++)
        read_sleb128(&p);

    /* Everything from packed_len_start up to here is also meta. */
    cat_write(CAT_SOCD_META, packed_len_start, (size_t)(p - packed_len_start));

    /* Walk blocks. */
    while (p < packed_end)
    {
        uint8_t header = (uint8_t)*p;
        cat_write(CAT_SOCD_HEADERS, p, 1);
        p++;

        if (header >> 5 < 7)
        {
            /* Bit matrix: (k+1) rows * (m/8) bytes.
             * m ∈ {8,16,24,32,40,48,112} per high-3-bit index. */
            static const int block_sizes[7] = {8,16,24,32,40,48,112};
            size_t body_len = (size_t)((header & 0x1F) + 1)
                            * (size_t)(block_sizes[header >> 5] / 8);
            if (p + body_len > packed_end)
            {
                fprintf(stderr,
                    "  WARNING: matrix block body exceeds packed_end\n");
                body_len = (size_t)(packed_end - p);
            }
            cat_write(CAT_SOCD_MATRIX, p, body_len);
            p += body_len;
        }
        else if (header == 0xFD)
        {
            const char *body_start = p;
            read_uleb128(&p);
            cat_write(CAT_SOCD_ABSENT, body_start, (size_t)(p - body_start));
        }
        else if (header == 0xFE)
        {
            const char *body_start = p;
            read_uleb128(&p);
            cat_write(CAT_SOCD_ZERO, body_start, (size_t)(p - body_start));
        }
        else if (header == 0xFF)
        {
            const char *body_start = p;
            uint64_t run_count = read_uleb128(&p);
            for (uint64_t j = 0; j <= run_count; j++)
                read_sleb128(&p);
            cat_write(CAT_SOCD_SLEB, body_start, (size_t)(p - body_start));
        }
        /* Reserved 0xE0..0xFC: 1-byte header only, already written. */
    }

    if (p != packed_end)
    {
        fprintf(stderr,
            "  WARNING: SOCD block walk overshot by %td bytes\n",
            p - packed_end);
        /* Best-effort: account remaining bytes as matrix. */
        if (p < packed_end)
        {
            cat_write(CAT_SOCD_MATRIX, p, (size_t)(packed_end - p));
            p = packed_end;
        }
    }

    /* Any trailing bytes between packed_end and end of SOCD payload would
     * be unexpected, but route them to OTHER so the invariant holds. */
    if (packed_end < end)
        cat_write(CAT_OTHER, packed_end, (size_t)(end - packed_end));
}

/** Process one SRNX file. Returns 0 on success, -1 on failure. */
static int split_file(const char *filename)
{
    /* Reset per-file counters. */
    memset(cat_file, 0, sizeof cat_file);

    int fd = open(filename, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
    {
        fprintf(stderr, "cannot open %s: %s\n", filename, strerror(errno));
        return -1;
    }

    struct stat st;
    if (fstat(fd, &st) < 0)
    {
        fprintf(stderr, "fstat(%s): %s\n", filename, strerror(errno));
        close(fd);
        return -1;
    }
    size_t file_size = (size_t)st.st_size;

    void *addr = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (addr == MAP_FAILED)
    {
        fprintf(stderr, "mmap(%s): %s\n", filename, strerror(errno));
        return -1;
    }

    const char *data = addr;

    /* --- SRNX header chunk --- */
    if (file_size < 5 || memcmp(data, "SRNX", 4) != 0)
    {
        fprintf(stderr, "%s: not an SRNX file\n", filename);
        munmap(addr, file_size);
        return -1;
    }

    const char *rptr = data + 4;
    uint64_t srnx_payload_len = read_uleb128(&rptr);
    /* FOURCC + ULEB length goes to chunk_frames. */
    cat_write(CAT_CHUNK_FRAMES, data, (size_t)(rptr - data));

    const char *srnx_payload = rptr;

    /* Read digest ids from the SRNX payload. */
    uint64_t major_ver = read_uleb128(&rptr); (void)major_ver;
    uint64_t minor_ver = read_uleb128(&rptr); (void)minor_ver;
    int chunk_digest_id = (int)read_uleb128(&rptr);
    int file_digest_id = (int)read_uleb128(&rptr);

    int chunk_dig_raw = rnx_digest_length(chunk_digest_id);
    int file_dig_raw = rnx_digest_length(file_digest_id);
    if (chunk_dig_raw < 0 || file_dig_raw < 0)
    {
        fprintf(stderr,
            "%s: unsupported digest id (chunk=%d, file=%d)\n",
            filename, chunk_digest_id, file_digest_id);
        munmap(addr, file_size);
        return -1;
    }
    int chunk_digest_length = chunk_dig_raw;

    if ((size_t)file_dig_raw > file_size)
    {
        fprintf(stderr,
            "%s: file smaller than declared file digest\n", filename);
        munmap(addr, file_size);
        return -1;
    }
    size_t walk_limit = file_size - (size_t)file_dig_raw;

    /* Write the SRNX payload (everything between length prefix and digest). */
    cat_write(CAT_SRNX_HDR, srnx_payload, (size_t)srnx_payload_len);
    /* And its chunk digest. */
    cat_write(CAT_DIGESTS, srnx_payload + srnx_payload_len,
        (size_t)chunk_digest_length);

    int64_t offset = (int64_t)(srnx_payload - data)
                   + (int64_t)srnx_payload_len
                   + chunk_digest_length;

    /* --- Remaining chunks --- */
    while (1)
    {
        const char *fourcc;
        const char *payload_start;
        uint64_t plen;

        int64_t found = find_next_chunk(data, walk_limit, offset,
            &fourcc, &plen, &payload_start, chunk_digest_length);
        if (found < 0) break;

        /* FOURCC + ULEB length. */
        cat_write(CAT_CHUNK_FRAMES, data + found,
            (size_t)(payload_start - (data + found)));

        enum cat payload_cat;
        if (memcmp(fourcc, "RHDR", 4) == 0)         payload_cat = CAT_RHDR;
        else if (memcmp(fourcc, "EPOC", 4) == 0)    payload_cat = CAT_EPOC;
        else if (memcmp(fourcc, "SDIR", 4) == 0)    payload_cat = CAT_SDIR;
        else if (memcmp(fourcc, "EVTF", 4) == 0)    payload_cat = CAT_EVTF;
        else if (memcmp(fourcc, "SATE", 4) == 0)    payload_cat = CAT_SATE_PRESENCE;
        else if (memcmp(fourcc, "SOCD", 4) == 0)    payload_cat = (enum cat)-1;
        else
        {
            fprintf(stderr,
                "  WARNING: unrecognized chunk %.4s at offset %" PRId64
                " (%" PRIu64 " bytes)\n", fourcc, found, plen);
            payload_cat = CAT_OTHER;
        }

        if (payload_cat == (enum cat)-1)
        {
            /* SOCD: split internally. */
            split_socd(payload_start, plen);
        }
        else
        {
            cat_write(payload_cat, payload_start, (size_t)plen);
        }

        /* Per-chunk digest. */
        cat_write(CAT_DIGESTS, payload_start + plen,
            (size_t)chunk_digest_length);

        offset = (int64_t)(payload_start - data) + (int64_t)plen
               + chunk_digest_length;
    }

    /* File-level digest at the tail. */
    if (file_dig_raw > 0)
        cat_write(CAT_DIGESTS, data + walk_limit, (size_t)file_dig_raw);

    munmap(addr, file_size);

    /* Invariant check. */
    uint64_t sum = 0;
    for (int i = 0; i < N_CAT; i++) sum += cat_file[i];

    printf("%s: %zu bytes -> sum=%" PRIu64 " %s\n",
        filename, file_size, sum,
        (sum == file_size) ? "OK" : "*** MISMATCH ***");

    return (sum == file_size) ? 0 : -1;
}

static void print_corpus_totals(int n_files, uint64_t input_bytes)
{
    uint64_t sum = 0;
    for (int i = 0; i < N_CAT; i++)
    {
        if (is_synthetic_cat((enum cat)i)) continue;
        sum += cat_total[i];
    }

    printf("\n=== Corpus totals (%d file%s, %" PRIu64 " input bytes) ===\n",
        n_files, n_files == 1 ? "" : "s", input_bytes);

    for (int i = 0; i < N_CAT; i++)
    {
        if (is_synthetic_cat((enum cat)i)) continue;
        double pct = input_bytes ? 100.0 * (double)cat_total[i]
                                        / (double)input_bytes
                                 : 0.0;
        printf("  %-20s %15" PRIu64 "  %6.2f%%\n",
            cat_names[i], cat_total[i], pct);
    }

    printf("  %-20s %15" PRIu64 "  %6.2f%%\n",
        "TOTAL", sum, input_bytes ? 100.0 : 0.0);

    if (sum != input_bytes)
        printf("  *** corpus mismatch: sum != input total ***\n");
}

#if OBSOLETE_LLI_SSI_STATS
static void print_indicator_stats(const char *label,
    const uint64_t hist[256],
    const uint64_t run_bucket[N_BUCKETS],
    uint64_t total_runs,
    uint64_t runs_oo, uint64_t bytes_oo)
{
    uint64_t total_bytes = 0;
    for (int i = 0; i < 256; i++) total_bytes += hist[i];

    printf("\n=== %s decoded-byte histogram"
        " (%" PRIu64 " bytes across %" PRIu64 " runs) ===\n",
        label, total_bytes, total_runs);

    /* Sort observed bytes by descending count. */
    int order[256], n = 0;
    for (int i = 0; i < 256; i++) if (hist[i]) order[n++] = i;
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (hist[order[j]] > hist[order[i]])
            {
                int t = order[i]; order[i] = order[j]; order[j] = t;
            }

    for (int i = 0; i < n; i++)
    {
        unsigned char ch = (unsigned char)order[i];
        double pct = total_bytes ? 100.0 * (double)hist[ch]
                                        / (double)total_bytes : 0.0;
        const char *mark = in_alphabet(ch) ? "  " : "* ";
        if (ch >= 0x20 && ch < 0x7F)
            printf("  %s'%c' (0x%02X): %15" PRIu64 "  %6.2f%%\n",
                mark, ch, ch, hist[ch], pct);
        else
            printf("  %s    (0x%02X): %15" PRIu64 "  %6.2f%%\n",
                mark, ch, hist[ch], pct);
    }
    if (runs_oo)
        printf("  * out-of-alphabet total: %" PRIu64 " runs, %" PRIu64
            " bytes\n", runs_oo, bytes_oo);
    else
        printf("  (all bytes in alphabet \" 0123456789\")\n");

    printf("  Run-length buckets (count -> bytes saved per run under"
        " (count-1)*11+idx packing):\n");
    static const char *labels[N_BUCKETS] = {
        "    1..   11 -> 1",
        "   12..  128 -> 0",
        "  129.. 1489 -> 1",
        " 1490..16384 -> 0",
        "16385+       -> 1",
    };
    uint64_t saved = 0;
    for (int i = 0; i < N_BUCKETS; i++)
    {
        printf("    %s : %15" PRIu64 " runs\n",
            labels[i], run_bucket[i]);
        saved += run_bucket[i] * (uint64_t)bucket_saves[i];
    }
    printf("  Estimated bytes saved by packed encoding: %" PRIu64 "\n",
        saved);
}
#endif

int main(int argc, char *argv[])
{
    int arg_i = 1;
    while (arg_i < argc && argv[arg_i][0] == '-' && argv[arg_i][1] != '\0')
    {
        if (strcmp(argv[arg_i], "-r") == 0)
        {
            g_write_raw_indicators = 1;
            arg_i++;
        }
        else if (strcmp(argv[arg_i], "--") == 0)
        {
            arg_i++;
            break;
        }
        else
        {
            fprintf(stderr, "Unknown option: %s\n", argv[arg_i]);
            return EXIT_FAILURE;
        }
    }

    if (argc - arg_i < 2)
    {
        fprintf(stderr,
            "Usage: srnx_split [-r] <output_dir> <file.srnx>...\n"
            "  -r: also write raw (RLE-decoded) LLI/SSI streams.\n");
        return EXIT_FAILURE;
    }

    open_cat_files(argv[arg_i]);

    int n_files = 0;
    uint64_t input_bytes = 0;
    int n_failed = 0;

    for (int i = arg_i + 1; i < argc; i++)
    {
        struct stat st;
        if (stat(argv[i], &st) == 0)
            input_bytes += (uint64_t)st.st_size;

        if (split_file(argv[i]) == 0)
            n_files++;
        else
            n_failed++;
    }

    close_cat_files();

    print_corpus_totals(n_files, input_bytes);

#if OBSOLETE_LLI_SSI_STATS
    print_indicator_stats("LLI", lli_byte_hist, lli_run_bucket,
        lli_total_runs, lli_runs_out_of_alphabet,
        lli_bytes_out_of_alphabet);
    print_indicator_stats("SSI", ssi_byte_hist, ssi_run_bucket,
        ssi_total_runs, ssi_runs_out_of_alphabet,
        ssi_bytes_out_of_alphabet);
#endif

    if (n_failed)
        fprintf(stderr, "%d file(s) failed\n", n_failed);

    return n_failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
