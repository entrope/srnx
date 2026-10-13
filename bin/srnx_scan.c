/** srnx_scan.c - Summarize the contents of an SRNX file.
 * Copyright 2026 Michael Poole.
 * SPDX-License-Identifier: MIT-Modern-Variant
 */

#include "rinex/digest.h"
#include "rinex/rnx_priv.h"

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

/** Find the next chunk at or after offset.
 * Returns the file offset of the chunk, or -1 if none can be found.
 * Sets *p_fourcc, *p_len on success.
 */
static int64_t find_next_chunk(
    const char *data, uint64_t limit, int64_t offset,
    const char **p_fourcc, uint64_t *p_len, int digest_length)
{
    while (offset >= 0 && (uint64_t)(offset + 4) <= limit)
    {
        const char *rptr = data + offset + 4;
        uint64_t len;

        /* Need at least 4 (FOURCC) + 1 (min ULEB128 length) bytes. */
        if ((uint64_t)(offset + 5) > limit)
            return -1;

        len = read_uleb128(&rptr);

        /* Validate that payload + digest fits in file. */
        int64_t payload_start = (int64_t)(rptr - data);
        int64_t chunk_end = payload_start + (int64_t)len + digest_length;
        if (chunk_end < 0 || (uint64_t)chunk_end > limit)
        {
            printf("  WARNING: chunk at offset %" PRId64
                " (%.4s) would extend past end of file\n",
                offset, data + offset);
            return -1;
        }

        *p_fourcc = data + offset;
        *p_len = len;
        return offset;
    }
    return -1;
}

/** Prints the epoch count and first span of an EPOC chunk, or of an EPO2
 * chunk (with times in picoseconds rather than 1e-7 s) if \a epo2.
 */
static void print_first_span(uint64_t total_epochs, const char *payload, int epo2)
{
    const char *rptr = payload;
    uint64_t ticks = epo2 ? 1000000000000ULL : 10000000ULL;
    int64_t interval = read_sleb128(&rptr);
    /* Negative interval = seconds; positive = already in ticks. */
    if (interval < 0) interval = -interval * (int64_t)ticks;

    uint64_t count_minus_1 = read_uleb128(&rptr);
    uint64_t date = read_uleb128(&rptr);
    uint64_t time_of_day = read_uleb128(&rptr);

    int yyyy;
    if (date < 1000000)
    {
        int yy = (int)(date / 10000);
        yyyy = (yy >= 80) ? 1900 + yy : 2000 + yy;
    }
    else
    {
        yyyy = (int)(date / 10000);
    }

    int mm = (int)((date % 10000) / 100);
    int dd = (int)(date % 100);

    int hh = (int)(time_of_day / (10000 * ticks));
    int mi = (int)((time_of_day % (10000 * ticks)) / (100 * ticks));
    uint64_t sec = time_of_day % (100 * ticks);
    int ss = (int)(sec / ticks);
    uint64_t frac = sec % ticks;

    printf("  Epochs: %" PRIu64
        ", Span: interval=%.*f s, count=%" PRIu64
        ", %04d-%02d-%02d %02d:%02d:%02d.%0*" PRIu64 "\n",
        total_epochs, epo2 ? 12 : 7,
        (double)interval / (double)ticks, count_minus_1 + 1,
        yyyy, mm, dd, hh, mi, ss, epo2 ? 12 : 7, frac);
}

typedef struct {
    uint64_t scale_value;
    uint64_t count;
} scale_entry;

typedef struct {
    int         epoc_count;
    int         epo2_count;
    int         sdir_count;
    int         evtf_count;
    int         sate_count;
    uint64_t    socd_count;
    uint64_t    soc2_count;
    uint64_t    soc2_kind_count[4];  /* by predictor kind; [3] = extended */
    uint64_t    soc2_ssi_ref_count;
    uint64_t    soc2_order_count[3][8]; /* [predictor kind][delta order] */
    uint64_t    total_epochs;
    scale_entry scales[256];
    int         n_scales;
    uint64_t    delta_order_count[8];
    uint64_t    block_header_count[256];
    /* sleb_run_hist[n] = count of 0xFF blocks with run length n (1..128, >128). */
    uint64_t    sleb_run_hist[130]; /* index 0 unused; index 129 = ">128" */
} file_stats_t;

static void scan_file(const char *filename, file_stats_t *stats)
{
    int fd = open(filename, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
    {
        fprintf(stderr, "Cannot open %s: %s\n", filename, strerror(errno));
        return;
    }

    struct stat st;
    fstat(fd, &st);
    size_t file_size = st.st_size;

    void *addr = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (addr == MAP_FAILED)
    {
        fprintf(stderr, "mmap failed: %s\n", strerror(errno));
        return;
    }

    const char *data = addr;
    const char *rptr;
    uint64_t payload_len;
    int chunk_digest_length = 0;

    /* --- Parse SRNX header chunk --- */
    if (file_size < 5 || memcmp(data, "SRNX", 4) != 0)
    {
        fprintf(stderr, "Not an SRNX file\n");
        munmap(addr, file_size);
        return;
    }
    rptr = data + 4;
    payload_len = read_uleb128(&rptr);

    const char *srnx_payload = rptr;
    uint64_t major_ver = read_uleb128(&rptr);
    uint64_t minor_ver = read_uleb128(&rptr);
    int chunk_digest_id = (int)read_uleb128(&rptr);
    int file_digest_id = (int)read_uleb128(&rptr);

    int chunk_dig_raw = rnx_digest_length(chunk_digest_id);
    int file_dig_raw = rnx_digest_length(file_digest_id);
    if (chunk_dig_raw < 0 || file_dig_raw < 0)
    {
        fprintf(stderr, "Unsupported digest id in SRNX header "
            "(chunk=%d, file=%d)\n", chunk_digest_id, file_digest_id);
        munmap(addr, file_size);
        return;
    }
    chunk_digest_length = chunk_dig_raw;

    /* The file-level digest, if any, sits at the tail of the file and is
     * not a chunk.  Use a separate walk limit that excludes it so we
     * don't misinterpret its bytes as a chunk header. */
    if ((size_t)file_dig_raw > file_size)
    {
        fprintf(stderr, "File smaller than declared file digest\n");
        munmap(addr, file_size);
        return;
    }
    size_t walk_limit = file_size - (size_t)file_dig_raw;

    uint64_t sdir_offset_uleb = read_uleb128(&rptr);
    int64_t sdir_offset = sdir_offset_uleb ? (int64_t)sdir_offset_uleb : -1;

    printf("SRNX: version %" PRIu64 ".%" PRIu64
        ", chunk digest=%d, file digest=%d, SDIR offset=%" PRId64 "\n",
        major_ver, minor_ver,
        chunk_digest_id, file_digest_id, sdir_offset);

    /* Advance past SRNX payload + digest to reach RHDR. */
    const char *next_chunk_ptr = srnx_payload + payload_len + chunk_digest_length;

    /* --- Parse RHDR chunk --- */
    if ((size_t)(next_chunk_ptr - data) + 4 < walk_limit
        && memcmp(next_chunk_ptr, "RHDR", 4) == 0)
    {
        rptr = next_chunk_ptr + 4;
        uint64_t rhdr_payload_len = read_uleb128(&rptr);
        printf("RHDR: %" PRIu64 " bytes\n", rhdr_payload_len);
        next_chunk_ptr = rptr + rhdr_payload_len + chunk_digest_length;
    }
    else
    {
        printf("WARNING: expected RHDR as second chunk\n");
    }

    /* --- Iterate remaining chunks --- */

    int64_t offset = next_chunk_ptr - data;
    while (1)
    {
        const char *fourcc;
        uint64_t plen;

        offset = find_next_chunk(data, walk_limit, offset,
            &fourcc, &plen, chunk_digest_length);
        if (offset < 0) break;

        rptr = data + offset + 4;
        read_uleb128(&rptr); /* skip payload length field */

        int64_t chunk_end = (int64_t)(rptr - data) + (int64_t)plen
                          + chunk_digest_length;

        /* What is the FOURCC? */
        if (memcmp(fourcc, "EPOC", 4) == 0 || memcmp(fourcc, "EPO2", 4) == 0)
        {
            int epo2 = fourcc[3] == '2';
            if (epo2)
                stats->epo2_count++;
            else
                stats->epoc_count++;
            stats->total_epochs = read_uleb128(&rptr);
            print_first_span(stats->total_epochs, rptr, epo2);
        }
        else if (memcmp(fourcc, "SDIR", 4) == 0)
        {
            stats->sdir_count++;
            const char *payload_start = rptr;
            const char *payload_end = payload_start + plen;

            /* Skip EPOC offset and EVTF offset. */
            read_uleb128(&rptr);
            read_uleb128(&rptr);

            /* Count satellite entries: each is 3-char ID + ULEB128 offset. */
            const char *sp = rptr;
            int n_sat = 0;
            while (sp + 3 <= payload_end)
            {
                sp += 3; /* 3-char satellite ID */
                if (sp >= payload_end) break;
                read_uleb128(&sp); /* file offset */
                n_sat++;
            }
            printf("SDIR: %" PRId64 " offset, %d satellites\n",
                offset, n_sat);
        }
        else if (memcmp(fourcc, "EVTF", 4) == 0)
            stats->evtf_count++;
        else if (memcmp(fourcc, "SATE", 4) == 0)
            stats->sate_count++;
        else if (memcmp(fourcc, "SOCD", 4) == 0)
        {
            stats->socd_count++;

            /* Skip observation name (8 bytes). */
            rptr += 8;
            /* Skip observation count-minus-1. */
            read_uleb128(&rptr);
            /* Skip LLI data (ULEB128 length + RLE data). */
            uint64_t lli_len = read_uleb128(&rptr);
            rptr += lli_len;
            /* Skip signal-strength data. */
            uint64_t ssi_len = read_uleb128(&rptr);
            rptr += ssi_len;

            /* Packed observation data. */
            uint64_t packed_len = read_uleb128(&rptr);
            const char *packed_end = rptr + packed_len;

            uint64_t schema = read_uleb128(&rptr);
            int delta_order = (int)(schema & 7);
            int has_scale = (int)(schema & 8);
            uint64_t scale_value = has_scale ? read_uleb128(&rptr) : 1000;

            stats->delta_order_count[delta_order]++;

            /* Record scale factor. */
            int found = 0;
            for (int i = 0; i < stats->n_scales; i++)
            {
                if (stats->scales[i].scale_value == scale_value)
                {
                    stats->scales[i].count++;
                    found = 1;
                    break;
                }
            }
            if (!found && stats->n_scales < 256)
            {
                stats->scales[stats->n_scales].scale_value = scale_value;
                stats->scales[stats->n_scales].count = 1;
                stats->n_scales++;
            }

            /* Skip initial SLEB128 state values (ZigZag, variable length). */
            for (int i = 0; i < delta_order; i++)
                read_sleb128(&rptr);

            /* Walk blocks until packed_len consumed. */
            while (rptr < packed_end)
            {
                uint8_t header = *(const uint8_t *)rptr++;
                stats->block_header_count[header]++;

                if (header >> 5 < RNX_N_BLOCK_SIZES)
                {
                    /* Bit matrix: (k+1) rows × (m/8) bytes each.
                     * bits per value = (header & 0x1F) + 1
                     * m ∈ rnx_block_sizes[] per high-3-bit index */
                    rptr += ((header & 0x1F) + 1) * (rnx_block_sizes[header >> 5] / 8);
                }
                else if (header == 0xFD)
                {
                    /* Absent run: ULEB128 count-minus-1. */
                    read_uleb128(&rptr);
                }
                else if (header == 0xFE)
                {
                    /* Zero run: ULEB128 count-minus-1. */
                    read_uleb128(&rptr);
                }
                else if (header == 0xFF)
                {
                    /* SLEB128 run: ULEB128 count-minus-1, then values. */
                    uint64_t run_count = read_uleb128(&rptr);
                    int hist_idx = (int)(run_count + 1);
                    if (hist_idx > 128) hist_idx = 129;
                    stats->sleb_run_hist[hist_idx]++;
                    for (uint64_t j = 0; j <= run_count; j++)
                        read_sleb128(&rptr);
                }
                /* 0xE0–0xFC: reserved, no payload to skip. */
            }

            if (rptr != packed_end)
            {
                printf("  WARNING: SOCD block walk overshot by %td bytes\n",
                    rptr - packed_end);
            }
        }
        else if (memcmp(fourcc, "SOC2", 4) == 0)
        {
            /* Summarize the mode value; split-Rice blocks are not walked. */
            stats->soc2_count++;
            rptr += 8;               /* observation name */
            read_uleb128(&rptr);     /* count-minus-1 */
            uint64_t lli_len = read_uleb128(&rptr);
            rptr += lli_len;
            uint64_t mode = read_uleb128(&rptr);
            stats->soc2_kind_count[mode & 3]++;
            if (mode >> 2)
                stats->soc2_ssi_ref_count++;
            if ((mode & 3) < 3)
            {
                /* Skip the SSIs and predictor fields to the delta order. */
                uint64_t ssi_len = read_uleb128(&rptr);
                rptr += ssi_len;
                if ((mode & 3) != 0)
                {
                    read_uleb128(&rptr);  /* reference */
                    read_sleb128(&rptr);  /* num */
                    read_uleb128(&rptr);  /* den */
                    if ((mode & 3) == 2)
                    {
                        read_uleb128(&rptr);  /* stencil */
                        read_uleb128(&rptr);  /* max_span */
                    }
                }
                read_uleb128(&rptr);      /* packed length */
                stats->soc2_order_count[mode & 3][read_uleb128(&rptr) & 7]++;
            }
        }
        else
        {
            printf("  WARNING: unrecognized chunk %.4s at offset %" PRId64
                " (%" PRIu64 " bytes)\n", fourcc, offset, plen);
        }

        offset = chunk_end;
    }

    munmap(addr, file_size);
}

static void agg_merge(file_stats_t *agg, const file_stats_t *stats)
{
    agg->epoc_count   += stats->epoc_count;
    agg->epo2_count   += stats->epo2_count;
    agg->sdir_count   += stats->sdir_count;
    agg->evtf_count   += stats->evtf_count;
    agg->sate_count   += stats->sate_count;
    agg->socd_count   += stats->socd_count;
    agg->soc2_count   += stats->soc2_count;
    for (int i = 0; i < 4; i++)
        agg->soc2_kind_count[i] += stats->soc2_kind_count[i];
    agg->soc2_ssi_ref_count += stats->soc2_ssi_ref_count;
    for (int k = 0; k < 3; k++)
        for (int i = 0; i < 8; i++)
            agg->soc2_order_count[k][i] += stats->soc2_order_count[k][i];
    agg->total_epochs += stats->total_epochs;

    for (int i = 0; i < stats->n_scales; i++)
    {
        uint64_t sv = stats->scales[i].scale_value;
        uint64_t c  = stats->scales[i].count;
        int found = 0;
        for (int j = 0; j < agg->n_scales; j++)
        {
            if (agg->scales[j].scale_value == sv)
            {
                agg->scales[j].count += c;
                found = 1;
                break;
            }
        }
        if (!found && agg->n_scales < 256)
        {
            agg->scales[agg->n_scales].scale_value = sv;
            agg->scales[agg->n_scales].count = c;
            agg->n_scales++;
        }
    }

    for (int i = 0; i < 8; i++)
        agg->delta_order_count[i] += stats->delta_order_count[i];
    for (int i = 0; i < 256; i++)
        agg->block_header_count[i] += stats->block_header_count[i];
    for (int i = 1; i <= 129; i++)
        agg->sleb_run_hist[i] += stats->sleb_run_hist[i];
}

static void print_block_header(int i, uint64_t count)
{
    if (i >> 5 < RNX_N_BLOCK_SIZES)
    {
        int m = rnx_block_sizes[i >> 5];
        int bits = (i & 0x1F) + 1;
        printf("  0x%02X (%d\xc3\x97%d-bit matrix): %" PRIu64 "\n",
            i, m, bits, count);
    }
    else if (i == 0xFD)
        printf("  0xFD (absent run): %" PRIu64 "\n", count);
    else if (i == 0xFE)
        printf("  0xFE (zero run): %" PRIu64 "\n", count);
    else if (i == 0xFF)
        printf("  0xFF (SLEB128 run): %" PRIu64 "\n", count);
    else
        printf("  0x%02X (reserved): %" PRIu64 "\n", i, count);
}

static void print_summary(const file_stats_t *agg)
{
    printf("Chunk totals:\n");
    printf("  EPOC: %d\n", agg->epoc_count);
    if (agg->epo2_count)
        printf("  EPO2: %d\n", agg->epo2_count);
    printf("  SDIR: %d\n", agg->sdir_count);
    printf("  EVTF: %d\n", agg->evtf_count);
    printf("  SATE: %d\n", agg->sate_count);
    printf("  SOCD: %" PRIu64 "\n", agg->socd_count);
    printf("  SOC2: %" PRIu64 "\n", agg->soc2_count);
    if (agg->soc2_count > 0)
    {
        printf("    predictor kinds: none %" PRIu64 ", value %" PRIu64
            ", rate %" PRIu64 ", extended %" PRIu64 "\n",
            agg->soc2_kind_count[0], agg->soc2_kind_count[1],
            agg->soc2_kind_count[2], agg->soc2_kind_count[3]);
        printf("    SSI references: %" PRIu64 "\n", agg->soc2_ssi_ref_count);
        for (int k = 0; k < 3; k++)
        {
            printf("    delta orders (predictor kind %d):", k);
            for (int i = 0; i < 8; i++)
                printf(" %" PRIu64, agg->soc2_order_count[k][i]);
            printf("\n");
        }
    }

    printf("\nTotal epochs: %" PRIu64 "\n", agg->total_epochs);

    if (agg->n_scales > 0)
    {
        printf("\nScale factors (observation unit multiples):\n");
        for (int i = 0; i < agg->n_scales; i++)
        {
            uint64_t sv = agg->scales[i].scale_value;
            printf("  %.6g: %" PRIu64 " SOCDs\n",
                (double)sv / 1000.0, agg->scales[i].count);
        }
    }

    if (agg->total_epochs > 0)
    {
        printf("\nDelta orders:\n");
        for (int i = 0; i < 8; i++)
        {
            if (agg->delta_order_count[i] > 0)
            {
                printf("  order %d: %" PRIu64 " SOCDs\n",
                    i, agg->delta_order_count[i]);
            }
        }

        printf("\nBlock headers:\n");
        for (int i = 0; i < 256; i++)
        {
            if (agg->block_header_count[i] > 0)
            {
                print_block_header(i, agg->block_header_count[i]);
            }
        }

        uint64_t sleb_total = 0;
        for (int i = 1; i <= 129; i++)
            sleb_total += agg->sleb_run_hist[i];
        if (sleb_total > 0)
        {
            printf("\nSLEB128 run lengths (0xFF blocks):\n");
            uint64_t cumulative = 0;
            for (int i = 1; i <= 129; i++)
            {
                if (agg->sleb_run_hist[i] == 0)
                    continue;
                cumulative += agg->sleb_run_hist[i];
                if (i == 129)
                    printf("  len >128: %10" PRIu64 "  (%.2f%% cumulative)\n",
                        agg->sleb_run_hist[i],
                        100.0 * (double)cumulative / (double)sleb_total);
                else
                    printf("  len %3d: %10" PRIu64 "  (%.2f%% cumulative)\n",
                        i, agg->sleb_run_hist[i],
                        100.0 * (double)cumulative / (double)sleb_total);
            }
        }
    }
}

static void usage(FILE *out, const char *argv0)
{
    fprintf(out, "Usage: %s [--help] <file.srnx> ...\n", argv0);
}

int main(int argc, char *argv[])
{
    file_stats_t agg, stats;
    int files_processed = 0;

    if (argc < 2)
    {
        usage(stderr, argv[0]);
        return EXIT_FAILURE;
    }
    if (!strcmp(argv[1], "--help"))
    {
        usage(stdout, argv[0]);
        return EXIT_SUCCESS;
    }

    memset(&agg, 0, sizeof agg);
    for (int i = 1; i < argc; i++)
    {
        memset(&stats, 0, sizeof stats);
        scan_file(argv[i], &stats);
        agg_merge(&agg, &stats);
        files_processed++;
    }

    printf("\n=== Aggregate Summary (%d files) ===\n\n", files_processed);
    print_summary(&agg);
    return EXIT_SUCCESS;
}
