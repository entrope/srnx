/** srnx_scan.c - Summarize the contents of an SRNX file.
 * Copyright 2026 Michael Poole.
 * SPDX-License-Identifier: MIT-Modern-Variant
 */

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

static void print_first_span(uint64_t total_epochs, const char *payload)
{
    const char *rptr = payload;
    int64_t interval = read_sleb128(&rptr);
    /* Negative interval = seconds; positive = already in 1e-7 s units. */
    if (interval < 0) interval = -interval * 10000000;

    uint64_t count_minus_1 = read_uleb128(&rptr);
    uint64_t date = read_uleb128(&rptr);
    uint64_t time_e11 = read_uleb128(&rptr);

    int yyyy;
    if (date < 1000000) {
        int yy = (int)(date / 10000);
        yyyy = (yy >= 80) ? 1900 + yy : 2000 + yy;
    } else {
        yyyy = (int)(date / 10000);
    }

    int mm = (int)((date % 10000) / 100);
    int dd = (int)(date % 100);

    int hh = (int)(time_e11 / 100000000000ULL);
    int mi = (int)((time_e11 % 100000000000ULL) / 1000000000ULL);
    uint64_t sec_e7 = time_e11 % 1000000000ULL;
    int ss = (int)(sec_e7 / 10000000);
    uint64_t frac = sec_e7 % 10000000;

    printf("  Epochs: %" PRIu64
        ", Span: interval=%.7f s, count=%" PRIu64
        ", %04d-%02d-%02d %02d:%02d:%02d.%07" PRIu64 "\n",
        total_epochs,
        (double)interval / 10000000.0, count_minus_1 + 1,
        yyyy, mm, dd, hh, mi, ss, frac);
}

static void scan_file(const char *filename)
{
    int fd = open(filename, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "Cannot open %s: %s\n", filename, strerror(errno));
        return;
    }

    struct stat st;
    fstat(fd, &st);
    size_t file_size = st.st_size;

    void *addr = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (addr == MAP_FAILED) {
        fprintf(stderr, "mmap failed: %s\n", strerror(errno));
        return;
    }

    const char *data = addr;
    const char *rptr;
    uint64_t payload_len;
    int chunk_digest_length = 0;

    /* --- Parse SRNX header chunk --- */
    if (file_size < 5 || memcmp(data, "SRNX", 4) != 0) {
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

    chunk_digest_length = chunk_digest_id ? (1 << (chunk_digest_id & 7)) : 0;

    uint64_t sdir_offset_uleb = read_uleb128(&rptr);
    int64_t sdir_offset = sdir_offset_uleb ? (int64_t)sdir_offset_uleb : -1;

    printf("SRNX: version %" PRIu64 ".%" PRIu64
        ", chunk digest=%d, file digest=%d, SDIR offset=%" PRId64 "\n",
        major_ver, minor_ver,
        chunk_digest_id, file_digest_id, sdir_offset);

    /* Advance past SRNX payload + digest to reach RHDR. */
    const char *next_chunk_ptr = srnx_payload + payload_len + chunk_digest_length;

    /* --- Parse RHDR chunk --- */
    if ((size_t)(next_chunk_ptr - data) + 4 < file_size
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
    int epoc_count = 0;
    int sdir_count = 0;
    int evtf_count = 0;
    int sate_count = 0;
    uint64_t epoc_total_epochs = 0;
    uint64_t socd_count = 0;

    /* SOCD statistics */
    typedef struct {
        uint64_t scale_value;
        uint64_t count;
    } scale_entry;
    scale_entry scales[256];
    int n_scales = 0;

    uint64_t delta_order_count[8] = {0};
    uint64_t block_header_count[256] = {0};

    int64_t offset = next_chunk_ptr - data;
    while (1) {
        const char *fourcc;
        uint64_t plen;

        offset = find_next_chunk(data, file_size, offset,
            &fourcc, &plen, chunk_digest_length);
        if (offset < 0) break;

        rptr = data + offset + 4;
        read_uleb128(&rptr); /* skip payload length field */

        int64_t chunk_end = (int64_t)(rptr - data) + (int64_t)plen
                          + chunk_digest_length;

        /* Check for unrecognized FOURCC. */
        int known = (memcmp(fourcc, "EPOC", 4) == 0
            || memcmp(fourcc, "SDIR", 4) == 0
            || memcmp(fourcc, "EVTF", 4) == 0
            || memcmp(fourcc, "SATE", 4) == 0
            || memcmp(fourcc, "SOCD", 4) == 0);
        if (!known) {
            printf("  WARNING: unrecognized chunk %.4s at offset %" PRId64
                " (%" PRIu64 " bytes)\n", fourcc, offset, plen);
        }

        if (memcmp(fourcc, "EPOC", 4) == 0) {
            epoc_count++;
            epoc_total_epochs = read_uleb128(&rptr);
            print_first_span(epoc_total_epochs, rptr);
        } else if (memcmp(fourcc, "SDIR", 4) == 0) {
            sdir_count++;
            const char *payload_start = rptr;
            const char *payload_end = payload_start + plen;

            /* Skip EPOC offset and EVTF offset. */
            read_uleb128(&rptr);
            read_uleb128(&rptr);

            /* Count satellite entries: each is 3-char ID + ULEB128 offset. */
            const char *sp = rptr;
            int n_sat = 0;
            while (sp + 3 <= payload_end) {
                sp += 3; /* 3-char satellite ID */
                if (sp >= payload_end) break;
                read_uleb128(&sp); /* file offset */
                n_sat++;
            }
            printf("SDIR: %" PRId64 " offset, %d satellites\n",
                offset, n_sat);
        } else if (memcmp(fourcc, "EVTF", 4) == 0) {
            evtf_count++;
        } else if (memcmp(fourcc, "SATE", 4) == 0) {
            sate_count++;
        } else if (memcmp(fourcc, "SOCD", 4) == 0) {
            socd_count++;

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

            delta_order_count[delta_order]++;

            /* Record scale factor. */
            int found = 0;
            for (int i = 0; i < n_scales; i++) {
                if (scales[i].scale_value == scale_value) {
                    scales[i].count++;
                    found = 1;
                    break;
                }
            }
            if (!found && n_scales < 256) {
                scales[n_scales].scale_value = scale_value;
                scales[n_scales].count = 1;
                n_scales++;
            }

            /* Skip initial SLEB128 state values (ZigZag, variable length). */
            for (int i = 0; i < delta_order; i++)
                read_sleb128(&rptr);

            /* Walk blocks until packed_len consumed. */
            while (rptr < packed_end) {
                uint8_t header = *(const uint8_t *)rptr++;
                block_header_count[header]++;

                if (header < 0x80) {
                    /* Bit matrix: (k+1) rows × (m/8) bytes each.
                     * bits per value = (header & 0x1F) + 1
                     * m = 8 << (header >> 5) */
                    rptr += ((header & 0x1F) + 1) * (1 << (header >> 5));
                } else if (header == 0xFD) {
                    /* Absent run: ULEB128 count-minus-1. */
                    read_uleb128(&rptr);
                } else if (header == 0xFE) {
                    /* Zero run: ULEB128 count-minus-1. */
                    read_uleb128(&rptr);
                } else if (header == 0xFF) {
                    /* SLEB128 run: ULEB128 count-minus-1, then values. */
                    uint64_t run_count = read_uleb128(&rptr);
                    for (uint64_t j = 0; j <= run_count; j++)
                        read_sleb128(&rptr);
                }
                /* 0x80–0xFD: reserved, no payload to skip. */
            }

            if (rptr != packed_end) {
                printf("  WARNING: SOCD block walk overshot by %td bytes\n",
                    rptr - packed_end);
            }
        }

        offset = chunk_end;
    }

    printf("\nChunk summary:\n");
    printf("  EPOC: %d\n", epoc_count);
    printf("  SDIR: %d\n", sdir_count);
    printf("  EVTF: %d\n", evtf_count);
    printf("  SATE: %d\n", sate_count);
    printf("  SOCD: %" PRIu64 "\n", socd_count);

    if (n_scales > 0) {
        printf("\nScale factors (observation unit multiples):\n");
        for (int i = 0; i < n_scales; i++) {
            uint64_t sv = scales[i].scale_value;
            printf("  %.6g: %" PRIu64 " SOCDs\n",
                (double)sv / 1000.0, scales[i].count);
        }
    }

    if (epoc_total_epochs > 0) {
        printf("\nDelta orders:\n");
        for (int i = 0; i < 8; i++) {
            if (delta_order_count[i] > 0)
                printf("  order %d: %" PRIu64 " SOCDs\n",
                    i, delta_order_count[i]);
        }

        printf("\nBlock headers:\n");
        for (int i = 0; i < 256; i++) {
            if (block_header_count[i] > 0) {
                if (i < 0x80) {
                    int m = 8 << (i >> 5);
                    int bits = (i & 0x1F) + 1;
                    printf("  0x%02X (%d×%d-bit matrix): %" PRIu64 "\n",
                        i, m, bits, block_header_count[i]);
                } else if (i == 0xFD) {
                    printf("  0xFD (absent run): %" PRIu64 "\n",
                        block_header_count[i]);
                } else if (i == 0xFE) {
                    printf("  0xFE (zero run): %" PRIu64 "\n",
                        block_header_count[i]);
                } else if (i == 0xFF) {
                    printf("  0xFF (SLEB128 run): %" PRIu64 "\n",
                        block_header_count[i]);
                } else {
                    printf("  0x%02X (reserved): %" PRIu64 "\n",
                        i, block_header_count[i]);
                }
            }
        }
    }

    munmap(addr, file_size);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: srnx_scan <file.srnx>\n");
        return EXIT_FAILURE;
    }
    for (int i = 1; i < argc; i++) {
        if (argc > 2)
            printf("=== %s ===\n", argv[i]);
        scan_file(argv[i]);
    }
    return EXIT_SUCCESS;
}
