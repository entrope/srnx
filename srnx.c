/** srnx.c - Succinct RINEX reader implementation.
 * Copyright 2020 Michael Poole.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "srnx.h"
#include "rinex_p.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <x86intrin.h>

#if !defined(O_CLOEXEC)
# define O_CLOEXEC 0
#endif

#define MATRIX_8X  0x00
#define MATRIX_16X 0x20
#define MATRIX_32X 0x40
#define MATRIX_64X 0x60
#define BLOCK_EMPTY 0xFE
#define BLOCK_SLEB128 0xFF

/** srnx_system_info holds information about a satellite system's
 * observations in a file.
 */
struct srnx_system_info
{
    /** Observation codes for this system. */
    struct srnx_obs_code *code;

    /** Number of codes in #code. */
    int codes_len;
};

/* Doc comment in srnx.h. */
struct srnx_reader
{
    /** Pointer to the memory-mapped file data. */
    const char *data;

    /** Valid length of #data, excluding the file digest and one chunk
     * digest.  (This means it is just past the end of the last valid
     * chunk, simplifying bounds checking.)
     */
    size_t data_size;

    /** Mapped length of #data. */
    size_t data_mapped;

    /** Holds the last line number that generated an error. */
    int error_line;

    /** SRNX major version number. */
    int major;

    /** SRNX minor version number. */
    int minor;

    /** Enumerated identifier for chunk digests. */
    int chunk_digest;

    /** Offset of the RHDR chunk. */
    int rhdr_offset;

    /** Offset of the first chunk after SRNX, RHDR and SDIR. */
    int next_offset;

    /** Offset of the SDIR chunk, if any. */
    int64_t sdir_offset;

    /** Offset of the EPOC chunk, if any; negative if unknown. */
    int64_t epoc_offset;

    /** Offset of the first EVTF chunk, if any; negative if unknown. */
    int64_t evtf_offset;

    /** Index of satellite systems into #sys_info.
     * This is indexed by the LSBs of the satellite system letter.
     */
    uint8_t sys_idx[32];

    /** Information about the n'th satellite system(s).
     * \a sys_info[0] is reserved for unsupported systems.
     */
    struct srnx_system_info sys_info[33];
};

/* Doc comment in srnx.h. */
struct srnx_obs_reader
{
    /** SRNX reader that this reader is associated with. */
    const struct srnx_reader *parent;

    /** Number of valid elements in #obs. */
    unsigned short obs_valid;

    /** Read pointer within #obs. */
    unsigned char obs_idx;

    /** When block_left > 0, what type is the block? */
    char block_code;

    /** If we are in the middle of decoding a block of observations, how
     * many are left?
     */
    int block_left;

    /** Number of observation values in the SOCD chunk. */
    uint64_t n_values;

    /** Observation scaling value. */
    uint64_t scale;

    /* The following offsets are all relative to \a parent->data. */

    /** Offset of (RLE-compressed) LLI indicator block. */
    uint64_t lli_offset;

    /** Offset of next observation read position. */
    uint64_t data_offset;

    /** End of SOCD payload. */
    uint64_t data_end;

    /* TODO: Add scaling factor, delta coding state. */

    /** Decoded observation values. */
    int64_t obs[256];
};

/* Negative SRNX error numbers. */
enum srnx_errno
{
    SRNX_NOT_SRNX = -1,
    SRNX_CORRUPT = -2,
    SRNX_BAD_MAJOR = -3,
    SRNX_BAD_STATE = -4,
    SRNX_NO_CHUNK = -5,
    SRNX_UNKNOWN_SYSTEM = -6,
    SRNX_UNKNOWN_CODE = -7,
    SRNX_UNKNOWN_SATELLITE = -8,
    SRNX_END_OF_DATA = -9,
    SRNX_IMPLEMENTATION_ERROR = -10
};

/* Doc comment in srnx.h. */
void srnx_free(void *ptr)
{
    free(ptr);
}

/* Doc comment in srnx.h. */
const char *srnx_strerror(int err)
{
    if (err > 0)
    {
        return strerror(err);
    }

    switch ((enum srnx_errno)err)
    {
    case SRNX_NOT_SRNX:
        return "Not a SRNX file";
    case SRNX_CORRUPT:
        return "Corrupt SRNX file";
    case SRNX_BAD_MAJOR:
        return "Unsupported SRNX major version";
    case SRNX_BAD_STATE:
        return "SRNX reader in a bad state for that operation";
    case SRNX_NO_CHUNK:
        return "No such chunk found";
    case SRNX_UNKNOWN_SYSTEM:
        return "Unknown satellite system";
    case SRNX_UNKNOWN_CODE:
        return "Unknown observation code";
    case SRNX_UNKNOWN_SATELLITE:
        return "Unknown satellite";
    case SRNX_END_OF_DATA:
        return "End of observation data";
    case SRNX_IMPLEMENTATION_ERROR:
        return "Implementation error";
    }

    return "Unknown SRNX error code";
}

/** Decodes a ULEB128 from \a *d, returning it and advancing \a *d. */
static uint64_t uleb128(const char **d)
{
    uint64_t accum = **d & 127;
    int shift = 0;

    while (**d & 128)
    {
        shift += 7;
        accum = (uint64_t)((*++*d) & 127) << shift;
    }

    return accum;
}

/** Decodes a SLEB128 from \a *d, returning it and advancing \a *d. */
static int64_t sleb128(const char **d)
{
    uint64_t ul;
    int64_t mag;

    ul = uleb128(d);
    mag = ul >> 1;
    return (ul & 1) ? -mag : mag;
}

/** Returns the length of digests for digest \a digest_id. */
static int srnx_digest_length(int digest_id)
{
    return digest_id ? (1 << (digest_id & 7)) : 0;
}

/* Doc comment in srnx.h */
void srnx_convert_s64_to_double(
    void *s64,
    int count,
    int scale
)
{
    const int64_t *in = s64;
    double *out = s64;
    double d_scale = scale / 1000.0;

#if defined(__AVX2__)
    const __m256d v_scale = _mm256_set1_pd(d_scale);
    const __m256d hh = _mm256_set1_pd(0x0018000000000000);
    for (; count >= 4; in += 4, out += 4, count -= 4)
    {
        __m256i xx = _mm256_load_si256((__m256i *)in);
        xx = _mm256_add_epi64(xx, _mm256_castpd_si256(hh));
        __m256d yy = _mm256_sub_pd(_mm256_castsi256_pd(xx), hh);
        _mm256_storeu_pd(out, _mm256_mul_pd(yy, v_scale));
    }
#endif

    while (count-- > 0)
    {
        *out++ = *in++ * d_scale;
    }
}

/* Parse a RINEX v2.xx file header. */
static int srnx_parse_rhdr_v2(
    struct srnx_reader *srnx,
    const char rhdr[],
    uint64_t rhdr_len
)
{
    static const char n_types_of_observ[] = "# / TYPES OF OBSERV";
    const char *line;
    int res, ii, jj, n_obs;
    char obs_type;

    /* Observation type should be 40th character of first line. */
    obs_type = rhdr[40];
    if (!strchr(" GRSEM", obs_type))
    {
        srnx->error_line = __LINE__;
        return SRNX_CORRUPT;
    }
    if (obs_type == 'M')
    {
        srnx->sys_idx[' ' & 31] = 1;
        srnx->sys_idx['G' & 31] = 1;
        srnx->sys_idx['R' & 31] = 1;
        srnx->sys_idx['S' & 31] = 1;
        srnx->sys_idx['E' & 31] = 1;
    }
    else if (obs_type == ' ')
    {
        srnx->sys_idx[' ' & 31] = 1;
        srnx->sys_idx['G' & 31] = 1;
    }
    else
    {
        srnx->sys_idx[obs_type & 31] = 1;
    }

    /* Find the (first) # / TYPES OF OBSERV line. */
    res = rnx_find_header(rhdr, rhdr_len, n_types_of_observ, sizeof n_types_of_observ);
    if (res < 0)
    {
        srnx->error_line = __LINE__;
        return SRNX_CORRUPT;
    }
    line = rhdr + res;

    /* How many observation types per satellite? */
    res = parse_uint(&n_obs, line, 6);
    if (res)
    {
        srnx->error_line = __LINE__;
        return res;
    }

    /* Allocate space for the observation code names. */
    srnx->sys_info[1].code = calloc(n_obs, sizeof *srnx->sys_info[1].code);
    if (!srnx->sys_info[1].code)
    {
        srnx->error_line = __LINE__;
        return ENOMEM;
    }

    /* Load the observation code names.
     * ii counts total observation codes loaded, jj counts per line.
     */
    for (ii = jj = 0; ii < n_obs; ++ii, ++jj)
    {
        memcpy(srnx->sys_info[1].code[ii].name, line + 10 + jj * 6, 2);
        srnx->sys_info[1].code[ii].name[2] = '\0';
        srnx->sys_info[1].code[ii].name[3] = '\0';

        /* If codes continue to next line, advance to it and check it. */
        if ((jj == 8) && (ii + 1 < n_obs))
        {
            line = strchr(line, '\n');
            if (!line
                || line >= rhdr + rhdr_len
                || memcmp(line + 61, n_types_of_observ, sizeof(n_types_of_observ) - 1))
            {
                srnx->error_line = __LINE__;
                return SRNX_CORRUPT;
            }
            ++line;
            jj = -1;
        }
    }

    srnx->sys_info[1].codes_len = n_obs;
    return 0;
}

/* Parse a RINEX v3.xx file header. */
static int srnx_parse_rhdr_v3(
    struct srnx_reader *srnx,
    const char rhdr[],
    uint64_t rhdr_len
)
{
    static const char sys_n_obs[] = "SYS / # / OBS TYPES";
    const char *line;
    int res, ii, jj, kk, n_obs;
    char sys_id;

    /* Find the (first) SYS / # / OBS TYPES line. */
    res = rnx_find_header(rhdr, rhdr_len, sys_n_obs, sizeof sys_n_obs);
    if (res < 0)
    {
        srnx->error_line = __LINE__;
        return SRNX_CORRUPT;
    }
    line = rhdr + res;

    /* Keep going as long as we are processing the same headers.
     * kk counts the satellite system (srnx->sys_info[kk]).
     */
    for (kk = 1; !memcmp(line + 60, sys_n_obs, sizeof(sys_n_obs) - 1); ++kk)
    {
        /* How many observations for this system? */
        sys_id = line[0];
        res = parse_uint(&n_obs, line + 3, 3);
        if (res || (n_obs < 1))
        {
            srnx->error_line = __LINE__;
            return SRNX_CORRUPT;
        }

        srnx->sys_info[kk].code = calloc(n_obs, sizeof(srnx->sys_info[0].code[0]));
        if (!srnx->sys_info[kk].code)
        {
            srnx->error_line = __LINE__;
            return ENOMEM;
        }

        /* Copy observation codes, continuing to following lines. */
        srnx->sys_idx[sys_id & 31] = kk;
        for (ii = jj = 0; ii < n_obs; ++ii, ++jj)
        {
            memcpy(srnx->sys_info[kk].code[ii].name, line + 7 + 4 * jj, 3);
            srnx->sys_info[kk].code[ii].name[3] = '\0';

            /* If codes continue to next line, advance to it and check it. */
            if ((jj == 12) && (ii + 1 < n_obs))
            {
                line = strchr(line, '\n');
                if (!line
                    || line >= rhdr + rhdr_len
                    || memcmp(line + 61, sys_n_obs, sizeof(sys_n_obs) - 1))
                {
                    srnx->error_line = __LINE__;
                    return SRNX_CORRUPT;
                }
                ++line;
                jj = -1;
            }
        }

        /* Advance to the next satellite system. */
        srnx->sys_info[kk++].codes_len = n_obs;
    }

    return 0;
}

/** Parse the RINEX file header.
 *
 * \param[in] srnx SRNX reader to use.
 * \param[in] rhdr Pointer to the start of the RINEX header.
 * \param[in] rhdr_len Number of bytes in \a rhdr.
 * \returns 0 on success, positive \a errno or negative \a srnx_errno on
 *   error (setting \a srnx->error_line).
 */
static int srnx_parse_rhdr(
    struct srnx_reader *srnx,
    const char rhdr[],
    uint64_t rhdr_len
)
{
    static const char rinex_version_type[] = "RINEX VERSION / TYPE";
    int ii;

    /* "RINEX VERSION / TYPE" line has RINEX version as F9.2 at 0. */
    if (memcmp(rhdr + 60, rinex_version_type, sizeof(rinex_version_type) - 1))
    {
        srnx->error_line = __LINE__;
        return SRNX_CORRUPT;
    }

    /* Initialize srnx->sys_info. */
    for (ii = 0; ii < 33; ++ii)
    {
        srnx->sys_info[ii].code = NULL;
    }

    if (!memcmp(rhdr, "     2.", 7))
    {
        return srnx_parse_rhdr_v2(srnx, rhdr, rhdr_len);
    }

    if (!memcmp(rhdr, "     3.", 7))
    {
        return srnx_parse_rhdr_v3(srnx, rhdr, rhdr_len);
    }

    srnx->error_line = __LINE__;
    return SRNX_BAD_MAJOR;
}

/* Doc comment in srnx.h. */
/* TODO: Optionally check file and chunk digests. */
int srnx_open(struct srnx_reader **p_srnx, const char filename[])
{
    struct stat sbuf;
    void *addr;
    const char *chunk, *rptr, *payload_start;
    size_t file_size, tot_len;
    uint64_t ul, payload_len;
    int fd, res, chunk_digest_length, file_digest, file_digest_length;

    /* Either clean up old srnx_reader, or allocate a new one. */
    if (*p_srnx)
    {
        if ((*p_srnx)->data)
        {
            munmap((void *)(*p_srnx)->data, (*p_srnx)->data_mapped);
        }

        for (ul = 0; ul < 33; ++ul)
        {
            free((*p_srnx)->sys_info[ul].code);
            (*p_srnx)->sys_info[ul].code = NULL;
            (*p_srnx)->sys_info[ul].codes_len = 0;
        }

        memset(*p_srnx, 0, sizeof **p_srnx);
    }
    else
    {
        *p_srnx = calloc(1, sizeof **p_srnx);
        if (!*p_srnx)
        {
            return ENOMEM;
        }
    }
    (*p_srnx)->data = NULL;
    (*p_srnx)->sdir_offset = -1;
    (*p_srnx)->epoc_offset = -1;
    (*p_srnx)->evtf_offset = -1;

    /* Open the requested file. */
    fd = open(filename, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
    {
        (*p_srnx)->error_line = __LINE__;
        return errno;
    }

    /* Find its size. */
    res = fstat(fd, &sbuf);
    if (res < 0)
    {
        (*p_srnx)->error_line = __LINE__;
        return errno;
    }
    file_size = sbuf.st_size;

    /* Memory-map the file. */
    if (!page_size && rnx_mmap_init())
    {
        (*p_srnx)->error_line = __LINE__;
        return errno;
    }
    tot_len = (file_size + RINEX_EXTRA + page_size - 1) & -page_size;
    addr = rnx_mmap_padded(fd, 0, file_size, tot_len);
    if (addr == MAP_FAILED)
    {
        (*p_srnx)->error_line = __LINE__;
        res = errno;
        close(fd);
        return res;
    }
    close(fd);

    /* Check that first chunk is SRNX. */
    chunk = addr;
    if (memcmp(chunk, "SRNX", 4))
    {
        (*p_srnx)->error_line = __LINE__;
        res = SRNX_NOT_SRNX;
late_failure:
        munmap(addr, tot_len);
        return res;
    }
    rptr = chunk + 4;

    /* Read SRNX chunk length. */
    payload_len = uleb128(&rptr);
    if (rptr - (const char *)addr + payload_len >= file_size)
    {
        (*p_srnx)->error_line = __LINE__;
srnx_corrupt:
        munmap(addr, tot_len);
        return SRNX_CORRUPT;
    }
    payload_start = rptr;

    /* Read file major version number. */
    ul = uleb128(&rptr);
    if (ul != 1)
    {
        (*p_srnx)->error_line = __LINE__;
        munmap(addr, tot_len);
        res = SRNX_BAD_MAJOR;
        goto late_failure;
    }
    (*p_srnx)->major = ul;

    /* Read file minor version number. */
    ul = uleb128(&rptr);
    if (ul > INT_MAX)
    {
        (*p_srnx)->error_line = __LINE__;
        goto srnx_corrupt;
    }
    (*p_srnx)->minor = ul;

    /* Read per-chunk digest identifer. */
    ul = uleb128(&rptr);
    if (ul > INT_MAX)
    {
        (*p_srnx)->error_line = __LINE__;
        goto srnx_corrupt;
    }
    (*p_srnx)->chunk_digest = ul;
    chunk_digest_length = srnx_digest_length((*p_srnx)->chunk_digest);

    /* Read file digest identifer. */
    ul = uleb128(&rptr);
    if (ul > INT_MAX)
    {
        (*p_srnx)->error_line = __LINE__;
        goto srnx_corrupt;
    }
    file_digest = ul;
    file_digest_length = srnx_digest_length(file_digest);
    if ((uint64_t)(rptr - (const char *)addr + file_digest_length
        + chunk_digest_length) > file_size)
    {
        (*p_srnx)->error_line = __LINE__;
        goto srnx_corrupt;
    }
    file_size -= file_digest_length + chunk_digest_length;

    /* Check that we didn't walk past the end of the chunk payload. */
    if ((uint64_t)(rptr - payload_start) > payload_len)
    {
        (*p_srnx)->error_line = __LINE__;
        goto srnx_corrupt;
    }

    /* Check that the next chunk is RHDR. */
    chunk = payload_start + payload_len + chunk_digest_length;
    if (memcmp(chunk, "RHDR", 4))
    {
        (*p_srnx)->error_line = __LINE__;
        goto srnx_corrupt;
    }
    (*p_srnx)->rhdr_offset = chunk - (const char *)addr;
    rptr = chunk + 4;

    /* Read RHDR chunk length. */
    payload_len = uleb128(&rptr);
    if (rptr - (const char *)addr + payload_len > file_size)
    {
        (*p_srnx)->error_line = __LINE__;
        goto srnx_corrupt;
    }

    /* Parse the RINEX header. */
    res = srnx_parse_rhdr(*p_srnx, rptr, payload_len);
    if (res)
    {
        goto srnx_corrupt;
    }

    (*p_srnx)->data = addr;
    (*p_srnx)->data_size = file_size;
    (*p_srnx)->data_mapped = tot_len;
    (*p_srnx)->next_offset = (rptr - (const char *)addr)
        + payload_len + chunk_digest_length;

    return 0;
}

/* Doc comment in srnx.h. */
int srnx_get_header(
    struct srnx_reader *srnx,
    const char **p_rhdr,
    size_t *rhdr_len
)
{
    if (!srnx->rhdr_offset)
    {
        srnx->error_line = __LINE__;
        return SRNX_BAD_STATE;
    }

    *p_rhdr = srnx->data + srnx->rhdr_offset;
    *rhdr_len = uleb128(p_rhdr);
    return 0;
}

/** Return the next \a fourcc chunk after \a offset in \a srnx.
 *
 * \warning On error, the caller is responsible for setting \a srnx->error_line.
 *
 * \param[in] srnx SRNX file to read.
 * \param[in] fourcc Four-character code identifying the chunk type.
 * \param[in] whence Starting offset to search at.  Must be a chunk start.
 * \param[out] p_payload Receives pointer to target chunk's payload.
 * \param[out] p_len Receives length of chunk's payload.
 * \param[out] p_start If not NULL, receives the offset of the chunk start.
 * \param[out] p_next If not NULL, receives the offset of the next chunk.
 * \returns Zero on success, else \a SRNX_CORRUPT or \a SRNX_NO_CHUNK.
 */
static int srnx_find_chunk(
    const struct srnx_reader *srnx,
    const char fourcc[4],
    uint64_t whence,
    const char **p_payload,
    uint64_t *p_len,
    int64_t *p_start,
    uint64_t *p_next
)
{
    const char *chunk, *rptr;
    uint64_t payload_len;
    int chunk_digest_length;

    chunk_digest_length = srnx_digest_length(srnx->chunk_digest);
    while (whence + 4 < srnx->data_size)
    {
        chunk = srnx->data + whence;
        rptr = chunk + 4;
        payload_len = uleb128(&rptr);
        if ((rptr - srnx->data) + payload_len > srnx->data_size)
        {
            return SRNX_CORRUPT;
        }
        if (!memcmp(chunk, fourcc, 4))
        {
            *p_payload = rptr;
            *p_len = payload_len;
            if (p_start)
            {
                *p_start = whence;
            }
            if (p_next)
            {
                *p_next = rptr - srnx->data + payload_len + chunk_digest_length;
            }
            return 0;
        }
        whence = rptr - srnx->data + payload_len + chunk_digest_length;
    }

    return SRNX_NO_CHUNK;
}

/** Return the first \a fourcc chunk in in \a srnx, which may be at \a *p_start.
 *
 * \warning On error, the caller is responsible for setting \a srnx->error_line.
 *
 * \param[in] srnx SRNX file to read.
 * \param[in] fourcc Four-character code identifying the chunk type.
 * \param[in,out] p_start Pointer to the offset of the chunk start.
 * \param[out] p_payload Receives pointer to target chunk's payload.
 * \param[out] p_len Receives length of chunk's payload.
 * \param[out] p_next If not NULL, receives the offset of the next chunk.
 * \returns Zero on success, else \a SRNX_CORRUPT or \a SRNX_NO_CHUNK.
 */
static int srnx_find_chunk_cached(
    const struct srnx_reader *srnx,
    const char fourcc[4],
    int64_t *p_start,
    const char **p_payload,
    uint64_t *p_len,
    uint64_t *p_next
)
{
    /* Is it known to be absent? */
    if (*p_start == 0)
    {
        return SRNX_NO_CHUNK;
    }

    /* Do we know where it is? */
    if (*p_start > 0)
    {
        const char *chunk;

        if ((uint64_t)*p_start >= srnx->data_size)
        {
            return SRNX_NO_CHUNK;
        }

        chunk = srnx->data + *p_start;
        if (memcmp(chunk, fourcc, 4))
        {
            return SRNX_BAD_STATE;
        }
        *p_len = uleb128(&chunk);
        if ((chunk - srnx->data) + *p_len > srnx->data_size)
        {
            return SRNX_CORRUPT;
        }

        if (p_next)
        {
            *p_next = chunk - srnx->data + *p_len
                + srnx_digest_length(srnx->chunk_digest);
        }
        *p_payload = chunk;
        return 0;
    }

    /* Search for it starting at \a srnx->next_offset. */
    return srnx_find_chunk(srnx, fourcc, srnx->next_offset, p_payload,
        p_len, p_start, p_next);
}

/* Doc comment in srnx.h. */
int srnx_get_epochs(
    struct srnx_reader *srnx,
    struct rinex_epoch **p_epoch,
    size_t *p_epochs_len
)
{
    uint64_t len, idx, n_epoch, date, time;
    int64_t i64;
    struct rinex_epoch *new_epochs, *epoch;
    const char *epoc, *end;
    int res, sec_e7;
    short hh_mm, mm;

    /* Search for the EPOC chunk. */
    res = srnx_find_chunk_cached(srnx, "EPOC", &srnx->epoc_offset, &epoc, &len, NULL);
    if (res)
    {
        srnx->error_line = __LINE__;
        return res;
    }
    end = epoc + len;

    /* How many epochs are in this file? */
    n_epoch = uleb128(&epoc);
    if (epoc > end)
    {
        srnx->error_line = __LINE__;
        return SRNX_CORRUPT;
    }

    /* Allocate memory for the epochs array.
     * XXX: Separately track used vs alloc'ed lengths?
     */
    new_epochs = *p_epoch
        ? realloc(*p_epoch, n_epoch * sizeof(**p_epoch))
        : calloc(n_epoch, sizeof(**p_epoch));
    if (!new_epochs)
    {
        srnx->error_line = __LINE__;
        return ENOMEM;
    }
    *p_epoch = new_epochs;
    *p_epochs_len = n_epoch;

    /* Walk over the epoch spans. */
    for (idx = 0; (idx < n_epoch) && (epoc < end); )
    {
        i64 = sleb128(&epoc);
        if (epoc >= end)
        {
            srnx->error_line = __LINE__;
            return SRNX_CORRUPT;
        }
        /* Convert to whole seconds? */
        if (i64 < 0)
        {
            i64 *= -10000000;
        }

        len = uleb128(&epoc);
        if (epoc >= end || idx + len > n_epoch)
        {
            srnx->error_line = __LINE__;
            return SRNX_CORRUPT;
        }

        date = uleb128(&epoc);
        if (epoc >= end || date > INT_MAX)
        {
            srnx->error_line = __LINE__;
            return SRNX_CORRUPT;
        }
        /* Convert two-digit year? */
        if (date < 1000000)
        {
            date += (date < 800000) ? 20000000 : 19000000;
        }

        time = uleb128(&epoc);
        if (epoc >= end || time > 2460610000000)
        {
            srnx->error_line = __LINE__;
            return SRNX_CORRUPT;
        }
        sec_e7 = time % 1000000000;
        hh_mm = time / 1000000000;
        mm = hh_mm % 60;

        /* Unpack this epoch span. */
        for (; len > 0; ++idx, --len)
        {
            epoch = (*p_epoch) + idx;
            epoch->yyyy_mm_dd = date;
            epoch->hh_mm = hh_mm;
            epoch->sec_e7 = sec_e7;

            /* Advance seconds count and check for minute rollover.
             * Comparison of previous sec_e7 is needed for leap seconds.
             */
            sec_e7 += i64;
            if (sec_e7 >= 600000000 && epoch->sec_e7 < 600000000)
            {
                sec_e7 -= 600000000;
                hh_mm += 1;
                mm += 1;
                if (mm >= 60)
                {
                    hh_mm += 40; /* increment to next "hour" */
                    mm = 0;
                }
            }
        }
    }

    /* Walk over the receiver clock offset spans. */
    for (idx = 0; (epoc < end) && (idx < n_epoch); )
    {
        i64 = sleb128(&epoc);
        if (epoc >= end)
        {
            srnx->error_line = __LINE__;
            return SRNX_CORRUPT;
        }

        len = uleb128(&epoc);
        if (epoc >= end)
        {
            srnx->error_line = __LINE__;
            return SRNX_CORRUPT;
        }

        for (; len > 0; len--)
        {
            (*p_epoch)[idx++].clock_offset = i64;
        }
    }

    /* Zero the remaining receiver clock offsets. */
    for (; idx < n_epoch; ++idx)
    {
        (*p_epoch)[idx].clock_offset = 0;
    }

    return 0;
}

/* Doc comment in srnx.h. */
int srnx_next_special_event(
    struct srnx_reader *srnx,
    const char **p_event,
    size_t *event_len,
    uint64_t *epoch_index
)
{
    uint64_t payload_len, whence;
    const char *payload;
    int res;

    /* Search for the next EVTF chunk. */
    if (*p_event)
    {
        res = srnx_find_chunk(srnx, "EVTF", *p_event - srnx->data,
            &payload, &payload_len, NULL, &whence);
    }
    else
    {
        res = srnx_find_chunk_cached(srnx, "EVTF", &srnx->evtf_offset,
            &payload, &payload_len, &whence);
    }
    if (res)
    {
        srnx->error_line = __LINE__;
        return res;
    }

    /* Decode the epoch index at the start of the chunk. */
    *p_event = payload;
    *epoch_index = uleb128(p_event);
    if ((uint64_t)(*p_event - payload) >= payload_len)
    {
        srnx->error_line = __LINE__;
        return SRNX_CORRUPT;
    }

    /* Report the event length (remaining payload). */
    *event_len = payload_len - (*p_event - payload);
    *p_event = srnx->data + whence;
    return 0;
}

/* Doc comment in srnx.h. */
int srnx_get_satellites(
    struct srnx_reader *srnx,
    struct srnx_satellite_name **p_name,
    uint64_t *p_names_len
)
{
    uint64_t names_alloc, payload_len, whence;
    struct srnx_satellite_name *next;
    const char *payload, *rptr;
    int res, chunk_digest_length;

    if (*p_name)
    {
        names_alloc = *p_names_len;
    }
    else
    {
        *p_names_len = 0;
        names_alloc = 32;
        *p_name = calloc(names_alloc, sizeof **p_name);
        if (!*p_name)
        {
            srnx->error_line = __LINE__;
            return ENOMEM;
        }
    }

    /* Can we look in the satellite directory? */
    res = srnx_find_chunk_cached(srnx, "SDIR", &srnx->sdir_offset,
        &payload, &payload_len, NULL);
    if (res == 0)
    {
        rptr = payload;

        /* Read the EPOC and EVTF offsets at the start of SDIR. */
        srnx->epoc_offset = uleb128(&rptr);
        srnx->evtf_offset = uleb128(&rptr);
        if (rptr > payload + payload_len)
        {
            srnx->error_line = __LINE__;
            return SRNX_CORRUPT;
        }

        while (rptr + 4 < payload + payload_len)
        {
            /* Do we need to grow the names list? */
            if (*p_names_len >= names_alloc)
            {
                names_alloc <<= 1;
                next = realloc(*p_name, names_alloc * sizeof(**p_name));
                if (!next)
                {
                    srnx->error_line = __LINE__;
                    return ENOMEM;
                }
                *p_name = next;
            }

            next = (*p_name) + *p_names_len++;
            memcpy(next->name, rptr, 3);
            next->name[3] = '\0';
            rptr += 3;
            uleb128(&rptr); /* skip (actually signed) file offset */
            if (rptr > payload + payload_len)
            {
                srnx->error_line = __LINE__;
                return SRNX_CORRUPT;
            }
        }

        return 0;
    }

    /* No satellite directory; must scan SATE chunks. */
    whence = srnx->next_offset;
    chunk_digest_length = srnx_digest_length(srnx->chunk_digest);
    while (!srnx_find_chunk(srnx, "SATE", whence, &payload, &payload_len, NULL, &whence))
    {
        if (payload_len < 4)
        {
                srnx->error_line = __LINE__;
                return SRNX_CORRUPT;
        }

        /* Do we need to grow the names list? */
        if (*p_names_len >= names_alloc)
        {
            names_alloc <<= 1;
            next = realloc(*p_name, names_alloc * sizeof(**p_name));
            if (!next)
            {
                srnx->error_line = __LINE__;
                return ENOMEM;
            }
            *p_name = next;
        }

        /* Save this satellite name. */
        next = (*p_name) + *p_names_len++;
        memcpy(next->name, payload, sizeof next->name);

        /* Continue searching at next chunk in file. */
        whence = (payload - srnx->data) + payload_len + chunk_digest_length;
    }

    return 0;
}

/** Find the SATE chunk for \a name.
 *
 * \param[in] srnx SRNX reader to read from.
 * \param[in] name Name of satellite to search for.
 * \returns The positive offset of the SATE chunk in \a srnx->data, or
 *   a negative \a srnx_errno on failure.
 */
static int64_t srnx_find_sate(
    const struct srnx_reader *srnx,
    struct srnx_satellite_name name
)
{
    uint64_t whence, next, u64;
    const char *payload, *rptr;
    int res;

    /* Do we have a satellite directory? */
    if (srnx->sdir_offset)
    {
        /* Read payload length. */
        rptr = payload = srnx->data + srnx->sdir_offset + 4;
        u64 = uleb128(&rptr);
        if ((int64_t)(srnx->sdir_offset + u64) < srnx->sdir_offset
            || srnx->sdir_offset + u64 > srnx->data_size)
        {
            return SRNX_CORRUPT;
        }
        payload += u64;

        /* Skip the EPOC and EVTF chunk offsets. */
        uleb128(&rptr);
        uleb128(&rptr);

        /* Scan through the satellite directory. */
        while (rptr < payload)
        {
            res = !memcmp(rptr, name.name, sizeof name.name);
            u64 = uleb128(&rptr);
            if (res)
            {
                /* Is the offset too large, or not pointing to a SATE? */
                if ((u64 + 9 >= srnx->data_size)
                    || memcmp(srnx->data + u64, "SATE", 4))
                {
                    return SRNX_CORRUPT;
                }

                /* Does the SATE payload start with this satellite name? */
                payload = rptr = srnx->data + u64 + 4;
                next = uleb128(&rptr);
                if ((next < 4) || memcmp(rptr, name.name, sizeof name.name))
                {
                    return SRNX_CORRUPT;
                }

                /* We found our winner. */
                return u64;
            }
        }
    }
    else
    {
        /* Scan through SATE chunks to find the right one. */
        for (whence = srnx->next_offset; whence < srnx->data_size; whence = next)
        {
            /* Find the next SATE chunk. */
            res = srnx_find_chunk(srnx, "SATE", whence, &payload, &u64,
                NULL, &next);
            if (res < 0)
            {
                return res;
            }
            if (u64 < 4)
            {
                return SRNX_CORRUPT;
            }

            /* Is this the right SATE chunk? */
            if (!memcmp(payload, name.name, sizeof name.name))
            {
                return whence;
            }
        }
    }

    return SRNX_UNKNOWN_SATELLITE;
}

/** Find the SOCD chunk for \a name and \a code.
 *
 * \param[in] srnx SRNX reader to read from.
 * \param[in] name Name of satellite to search for.
 * \param[in] code Observation code to search for.
 * \returns The positive offset of the SOCD chunk in \a srnx->data, or a
 *   negative \a srnx_errno on failure.
 */
static int64_t srnx_find_socd(
    struct srnx_reader *srnx,
    struct srnx_satellite_name name,
    struct srnx_obs_code code
)
{
    uint64_t u64;
    int64_t sate_offset, s64;
    const char *rptr, *payload;
    int s_idx, n_codes, ii;

    /* Do we have a SATE entry for this satellite? */
    sate_offset = srnx_find_sate(srnx, name);
    if (sate_offset < 0)
    {
        srnx->error_line = __LINE__;
        return sate_offset;
    }
    payload = srnx->data + sate_offset + 4;
    u64 = uleb128(&payload);
    rptr = payload + 4; /* srnx_find_sate() confirms satellite name */
    payload += u64 - 4; /* and that payload length >= 4 */

    /* How many codes for this satellite? */
    s_idx = srnx->sys_idx[name.name[0] & 31];
    if (!s_idx)
    {
        srnx->error_line = __LINE__;
        return SRNX_UNKNOWN_SYSTEM;
    }
    n_codes = srnx->sys_info[s_idx].codes_len;

    /* Search for the desired code. */
    for (ii = 0; ii < n_codes; ++ii)
    {
        s64 = sleb128(&rptr);

        /* If the observation is */
        if (memcmp(&code, srnx->sys_info[s_idx].code + ii, sizeof code))
        {
            continue;
        }

        /* If the offset was zero, the observation is absent. */
        if (!s64)
        {
            srnx->error_line = __LINE__;
            return SRNX_UNKNOWN_CODE;
        }

        /* Validate SOCD block is for the expected SV and observation. */
        s64 += sate_offset;
        payload = srnx->data + s64;
        if (memcmp(payload, "SOCD", 4)
            || (uleb128(&payload) < 8)
            || memcmp(payload, name.name, sizeof name)
            || memcmp(payload + 4, code.name, sizeof code))
        {
            srnx->error_line = __LINE__;
            return SRNX_CORRUPT;
        }

        return s64;
    }

    srnx->error_line = __LINE__;
    return SRNX_UNKNOWN_CODE;
}

/* Doc comment in srnx.h. */
int srnx_open_obs_by_index(
    struct srnx_reader *srnx,
    struct srnx_satellite_name name,
    int obs_idx,
    struct srnx_obs_reader **p_rdr
)
{
    const char *rptr;
    uint64_t u64, n_values, lli_offset, data_end;
    int64_t socd_offset;
    struct srnx_obs_code code;
    int sys_idx;

    /* Is the satellite system known for this file? */
    sys_idx = srnx->sys_idx[name.name[0] & 31];
    if (!sys_idx)
    {
        srnx->error_line = __LINE__;
        return SRNX_UNKNOWN_SYSTEM;
    }

    /* Is the observation index valid for the satellite system? */
    if (obs_idx < 0 || obs_idx >= srnx->sys_info[sys_idx].codes_len)
    {
        srnx->error_line = __LINE__;
        return SRNX_UNKNOWN_CODE;
    }

    /* Grab the name of the observation code. */
    memcpy(&code, srnx->sys_info[sys_idx].code + obs_idx, sizeof code);

    /* Try to find the SOCD chunk. */
    socd_offset = srnx_find_socd(srnx, name, code);
    if (socd_offset < 0)
    {
        /* srnx_find_socd() sets \a srnx->error_line. */
        return socd_offset;
    }

    /* How big is the SOCD payload? */
    rptr = srnx->data + socd_offset + 4;
    u64 = uleb128(&rptr);
    if (u64 > (uint64_t)(srnx->data + srnx->data_size - rptr))
    {
        srnx->error_line = __LINE__;
        return SRNX_CORRUPT;
    }
    rptr += 8; /* srnx_find_socd() verifies observation name */

    /* Read number of observations. */
    n_values = 1 + uleb128(&rptr);

    /* Skip LLI indicators. */
    lli_offset = rptr - srnx->data;
    u64 = uleb128(&rptr);
    if (u64 > (uint64_t)(srnx->data + srnx->data_size - rptr))
    {
        srnx->error_line = __LINE__;
        return SRNX_CORRUPT;
    }
    rptr += u64;

    /* Then skip SSI indicators. */
    u64 = uleb128(&rptr);
    if (u64 > (uint64_t)(srnx->data + srnx->data_size - rptr))
    {
        srnx->error_line = __LINE__;
        return SRNX_CORRUPT;
    }
    rptr += u64;

    /* How long is the packed observation data? */
    u64 = uleb128(&rptr);
    if (u64 > (uint64_t)(srnx->data + srnx->data_size - rptr))
    {
        srnx->error_line = __LINE__;
        return SRNX_CORRUPT;
    }
    data_end = rptr - srnx->data + u64;

    /* Allocate *p_rdr if necessary. */
    if (!*p_rdr)
    {
        *p_rdr = malloc(sizeof **p_rdr);
        if (!*p_rdr)
        {
            srnx->error_line = __LINE__;
            return ENOMEM;
        }
    }

    /* Initialize everything we need to keep. */
    memset(*p_rdr, 0, sizeof **p_rdr);
    (*p_rdr)->parent = srnx;
    (*p_rdr)->n_values = n_values;
    (*p_rdr)->lli_offset = lli_offset;
    (*p_rdr)->data_offset = rptr - srnx->data;
    (*p_rdr)->data_end = data_end;
    return 0;
}

void srnx_free_obs_reader(struct srnx_obs_reader *p_socd)
{
    free(p_socd);
}

/** Decompresses indicators from [\a in, \a end) into \a out.
 *
 * \param[out] out Receives decompressed indicators.
 * \param[in] n_values Number of indicators to decompress.
 * \param[in] in Start of RLE-compressed indicators.
 * \param[in] end End-plus-1 of RLE-compressed indicators.
 * \returns Zero on success, else negative \a srnx_errno on failure.
 */
static int decompress_indicators(
    char *out,
    uint64_t n_values,
    const char *in,
    const char *end
)
{
    uint64_t ii, count;
    char ind;

    /* Decompress the RLE-compressed data. */
    for (ii = 0; in < end; )
    {
        ind = *in++;
        count = uleb128(&in) + 1;
        if (ii + count > n_values)
        {
            return SRNX_CORRUPT;
        }
        memset(out + ii, ind, count);
        ii += count;
    }
    if (in > end)
    {
        return SRNX_CORRUPT;
    }

    /* Fill any remaining indicators with spaces. */
    memset(out + ii, ' ', n_values - ii);
    return 0;
}

/* Doc comment in srnx.h. */
int srnx_read_obs_ssi_lli(
    struct srnx_obs_reader *p_socd,
    int *p_n_values,
    char **p_lli,
    char **p_ssi
)
{
    uint64_t u64, n_values;
    const char *inds;
    char *cp;
    int res;

    /* (Re-)Allocate the LLI and SSI arrays. */
    n_values = p_socd->n_values;
    cp = realloc(*p_lli, n_values);
    if (!cp)
    {
        return ENOMEM;
    }
    *p_lli = cp;
    cp = realloc(*p_ssi, n_values);
    if (!cp)
    {
        return ENOMEM;
    }
    *p_ssi = cp;
    *p_n_values = n_values;

    /* Decompress the LLIs. */
    inds = p_socd->parent->data + p_socd->lli_offset;
    u64 = uleb128(&inds);
    res = decompress_indicators(*p_lli, n_values, inds, inds + u64);
    if (res)
    {
        return res;
    }
    inds += u64; /* bounds-checked by srnx_open_obs_by_index() */

    /* Decompress the SSIs. */
    u64 = uleb128(&inds);
    return decompress_indicators(*p_ssi, n_values, inds, inds + u64);
}

/** Attempts to decode observations from the SRNX file into \a p_socd.
 *
 * This reads observations from \a p_socd->data_offset into
 * \a p_socd->obs, decompressing as indicated by the data.
 *
 * \param[in,out] p_socd Observation reader to update.
 * \returns Zero on success, including if \a p_socd->data_offset is at
 *   the end of the SOCD chunk's data.  Otherwise a negative
 *   \a srnx_errno.
 */
static int decode_observations(
    struct srnx_obs_reader *p_socd
)
{
    const char *data, *end;
    size_t avail;
    uint64_t u64;
    int count, bits, idx, res, ii;
    char ch;

    /* Pack down valid observations to the start. */
    if (p_socd->obs_idx > 0)
    {
        p_socd->obs_valid -= p_socd->obs_idx;
        if (p_socd->obs_valid > 0)
        {
            memmove(p_socd->obs, p_socd->obs + p_socd->obs_idx,
                p_socd->obs_valid * sizeof(p_socd->obs[0]));
        }
    }
    idx = 0;

    /* Try to read more until we cannot read any more. */
    data = p_socd->parent->data + p_socd->data_offset;
    end = p_socd->parent->data + p_socd->data_end;
    while (data < end)
    {
        /* How many observations can we read right now? */
        avail = sizeof(p_socd->obs) / sizeof(p_socd->obs[0]) - p_socd->obs_valid;

        /* Do we have block-coded observations to read? */
    try_block:
        if ((count = p_socd->block_left) > 0)
        {
            /* Don't overrun our observation array. */
            if ((size_t)count > avail)
            {
                count = avail;
            }

            /* Decode according to block encoding scheme. */
            if ((unsigned char)p_socd->block_code == BLOCK_SLEB128)
            {
                for (ii = 0; ii < count; ++ii)
                {
                    p_socd->obs[idx++] = sleb128(&data);
                    if (data > end)
                    {
                        p_socd->block_left -= ii;
                        res = SRNX_CORRUPT;
                        goto out;
                    }
                }
            }
            else if ((unsigned char)p_socd->block_code == BLOCK_EMPTY)
            {
                ii = count;
                memset(p_socd->obs + idx, 0, ii * sizeof(p_socd->obs[0]));
            }
            else
            {
                res = SRNX_IMPLEMENTATION_ERROR;
                goto out;
            }

            /* Update bookkeeping. */
            p_socd->block_left -= ii;
            avail -= ii;
        }

        /* The next byte indicates the encoding scheme. */
        ch = *data++;
        if ((unsigned char)ch == BLOCK_EMPTY
            || (unsigned char)ch == BLOCK_SLEB128)
        {
            u64 = uleb128(&data);
            if (u64 > INT_MAX || data > end)
            {
                res = SRNX_CORRUPT;
                goto out;
            }
            p_socd->block_left = u64;
            p_socd->block_code = ch;
            goto try_block;
        }

        /* It looks like a transposed bit matrix. */
        count = 8 << (ch >> 5); /* number of output values */
        bits = (ch & 31) + 1; /* bits per output value */

        /* Is the word count valid?  Do we have enough data? */
        if ((count > 32) || (data + (count >> 3) * bits > end))
        {
            res = SRNX_CORRUPT;
            goto out;
        }

        /* Would this overflow the observation buffer? */
        if (avail < (size_t)count)
        {
            break;
        }

        /* Transpose the matrix. */
        transpose(p_socd->obs + idx, data, bits, count);

        /* Update bookkeeping. */
        data += bits;
        idx += count;
    }

    /* TODO: Decode deltas and scale observations. */
    res = 0;

out:
    p_socd->data_offset = data - p_socd->parent->data;
    p_socd->obs_idx = idx;
    return res;
}

/* Doc comment in srnx.h. */
int srnx_read_obs_value(
    struct srnx_obs_reader *p_socd,
    int64_t *p_value
)
{
    int res;

    /* Do we need more observations? */
    if (p_socd->obs_idx >= p_socd->obs_valid)
    {
        p_socd->obs_valid = 0;
        if (p_socd->data_offset < p_socd->data_end)
        {
            res = decode_observations(p_socd);
            if (res)
            {
                return res;
            }
        }

        if (!p_socd->obs_valid)
        {
            return SRNX_END_OF_DATA;
        }
    }

    *p_value = p_socd->obs[p_socd->obs_idx++];
    return 0;
}

/** Looks up the index for a satellite and observation code combination.
 *
 * \warning On failure, the caller must set \a srnx->error_line.
 * \param[in] srnx SRNX reader to use.
 * \param[in] name Satellite name to look up.
 * \param[in] code Observation code name to look up.
 * \returns Non-negative index on success, or else \a SRNX_UNKNOWN_NAME
 *   or \a SRNX_UNKNOWN_SYSTEM on failure.
 */
static int srnx_obs_name_to_idx(
    const struct srnx_reader *srnx,
    struct srnx_satellite_name name,
    struct srnx_obs_code code
)
{
    int ii, sys_idx, n_codes;

    /* Find the satellite's system index. */
    sys_idx = srnx->sys_idx[name.name[0] & 31];
    if (!sys_idx || !(n_codes = srnx->sys_info[sys_idx].codes_len))
    {
        return SRNX_UNKNOWN_SYSTEM;
    }

    for (ii = 0; ii < n_codes; ++ii)
    {
        if (!memcmp(srnx->sys_info[sys_idx].code[ii].name, code.name, sizeof code.name))
        {
            break;
        }
    }

    if (ii == n_codes)
    {
        return SRNX_UNKNOWN_CODE;
    }

    return ii;
}

/* Doc comment in srnx.h. */
int srnx_get_obs_by_name(
    struct srnx_reader *srnx,
    struct srnx_satellite_name name,
    int codes_len,
    struct srnx_obs_code code[],
    int n_values[],
    int64_t **p_obs,
    char **p_lli,
    char **p_ssi
)
{
    int ii, c_idx, *idx;

    /* Allocate an index for each requested name. */
    idx = alloca(codes_len * sizeof(*idx));
    if (!idx)
    {
        srnx->error_line = __LINE__;
        return ENOMEM;
    }

    /* Translate names to indices. */
    for (ii = 0; ii < codes_len; ++ii)
    {
        c_idx = srnx_obs_name_to_idx(srnx, name, code[ii]);
        if (c_idx < 0)
        {
            srnx->error_line = __LINE__;
            return c_idx;
        }
        idx[ii] = c_idx;
    }

    return srnx_get_obs_by_index(srnx, name, codes_len, idx, n_values,
        p_obs, p_ssi, p_lli);
}

/* Doc comment in srnx.h. */
int srnx_open_obs_by_name(
    struct srnx_reader *srnx,
    struct srnx_satellite_name name,
    struct srnx_obs_code code,
    struct srnx_obs_reader **p_rdr
)
{
    int c_idx;

    /* Translate name to index. */
    c_idx = srnx_obs_name_to_idx(srnx, name, code);
    if (c_idx < 0)
    {
        srnx->error_line = __LINE__;
        return c_idx;
    }

    return srnx_open_obs_by_index(srnx, name, c_idx, p_rdr);
}

/* Doc comment in srnx.h. */
int srnx_get_obs_by_index(
    struct srnx_reader *srnx,
    struct srnx_satellite_name name,
    int idx_len,
    int idx[],
    int n_values[],
    int64_t **p_obs,
    char **p_lli,
    char **p_ssi
)
{
    struct srnx_obs_reader *p_socd;
    int64_t count, *obs;
    int ii, jj, res;

    p_socd = NULL;
    for (ii = 0; ii < idx_len; ++ii)
    {
        /* Open the ii'th observation. */
        res = srnx_open_obs_by_index(srnx, name, idx[ii], &p_socd);
        if (res)
        {
            /* srnx_open_obs_by_index sets \a srnx->error_line. */
            return res;
        }

        /* Read the SSIs and LLIs. */
        res = srnx_read_obs_ssi_lli(p_socd, n_values + ii, p_lli + ii, p_ssi + ii);
        if (res)
        {
            srnx->error_line = __LINE__;
            srnx_free_obs_reader(p_socd);
            return res;
        }

        /* Reallocate memory for the observations. */
        obs = realloc(p_obs[ii], n_values[ii] * (size_t)8);
        if (!obs)
        {
            srnx->error_line = __LINE__;
            srnx_free_obs_reader(p_socd);
            return ENOMEM;
        }
        p_obs[ii] = obs;

        /* Read observation values into p_obs[ii]. */
        for (jj = 0; jj < n_values[ii]; )
        {
            res = decode_observations(p_socd);
            if (res)
            {
                srnx->error_line = __LINE__;
                srnx_free_obs_reader(p_socd);
                return res;
            }

            if (p_socd->obs_valid < n_values[ii] - jj)
            {
                count = p_socd->obs_valid;
            }
            else
            {
                count = n_values[ii] - jj;
            }
            memcpy(p_obs[ii] + jj, p_socd->obs, sizeof(int64_t) * count);
            jj += count;
        }
    }

    /* Deallocate the reader. */
    srnx_free_obs_reader(p_socd);

    return 0;
}
