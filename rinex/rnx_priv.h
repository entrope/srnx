/* rnx_priv.h - Private definitions for RINEX observation parsing.
 * Copyright 2020 Michael Poole.
 * SPDX-License-Identifier: MIT-Modern-Variant
 */

#if !defined(RNX_PRIV_H_a03d7227_442c_4822_a2d2_04bd8c5ff3e4)
#define RNX_PRIV_H_a03d7227_442c_4822_a2d2_04bd8c5ff3e4

#include <fcntl.h>
#include <stddef.h>
#include <stdlib.h>

#include "rinex/rinex.h"
#include "rinex/rinex_load.h"
#include "rinex/srnx.h"

/** RINEX_EXTRA is the extra length of stream buffers to ease vectorization. */
#define RINEX_EXTRA 80

/** BLOCK_SIZE is how much data we normally try to read into a buffer. */
#define BLOCK_SIZE (1024 * 1024 - RINEX_EXTRA)

/** RNX_META_FLAG is used to flag special values in rinex_parser.obs[]. */
#define RNX_META_FLAG INT64_MIN

/* Bit-matrix block sizes selected by the high 3 bits of a SOCD
 * block-header byte (indexes 0..6; index 7 selects the special
 * codes 0xE0-0xFF).  All values are multiples of 8. */
#define RNX_N_BLOCK_SIZES 7
static const int rnx_block_sizes[RNX_N_BLOCK_SIZES] = {
    8, 16, 24, 32, 40, 48, 112
};

/* Split-Rice block sizes selected by the high 3 bits of a SOC2
 * block-header byte; the low 5 bits are the Rice parameter. */
static const int rnx_rice_block_sizes[RNX_N_BLOCK_SIZES] = {
    8, 24, 56, 144, 240, 640, 2048
};
#define RNX_MAX_RICE_BLOCK 2048
#define RNX_RICE_MAX_R 31

/* SOC2 block codes beyond the SOCD set. */
#define SRNX_BLOCK_RESET   0xFB
#define SRNX_BLOCK_UNPRED  0xFC

/* SOC2 predictor kinds and Doppler stencils. */
#define SRNX_PRED_NONE  0
#define SRNX_PRED_VALUE 1
#define SRNX_PRED_RATE  2

/** Exact floor(\a un / \a ud).  Most SOC2 predictions fit 64-bit hardware
 * division; the rest use a double-precision estimate corrected by an
 * exact remainder, falling back to 128-bit division (a slow library call
 * on most targets) only if the estimate is far off. */
static inline uint64_t rnx_udiv128(unsigned __int128 un, unsigned __int128 ud)
{
    if ((un >> 64) == 0 && (ud >> 64) == 0)
        return (uint64_t)un / (uint64_t)ud;
    if ((ud >> 64) == 0 && (uint64_t)(un >> 64) < (uint64_t)ud)
    {
        double est = ((double)(uint64_t)(un >> 64) * 18446744073709551616.0
            + (double)(uint64_t)un) / (double)(uint64_t)ud;
        if (est < 1.8e19)
        {
            uint64_t qq = (uint64_t)est;
            __int128 rem = (__int128)un - (__int128)qq * (__int128)ud;
            int iter;
            for (iter = 0; iter < 8 && rem < 0; ++iter)
            {
                qq--;
                rem += (__int128)ud;
            }
            for (; iter < 8 && rem >= (__int128)ud; ++iter)
            {
                qq++;
                rem -= (__int128)ud;
            }
            if (rem >= 0 && rem < (__int128)ud)
                return qq;
        }
    }
    return (uint64_t)(un / ud);
}

/** Divide, rounding to the nearest integer with ties away from zero,
 * as SOC2 predictions require.  \a den must be nonzero. */
static inline int64_t rnx_rdiv128(__int128 num, __int128 den)
{
    unsigned __int128 un, ud;
    uint64_t qq;
    int neg;

    if (den < 0)
    {
        num = -num;
        den = -den;
    }
    neg = num < 0;
    un = (unsigned __int128)(neg ? -num : num);
    ud = (unsigned __int128)den;
    qq = rnx_udiv128(un + ud / 2, ud);
    return neg ? -(int64_t)qq : (int64_t)qq;
}

/** Days since 1970-01-01 of a proleptic Gregorian date. */
static inline int64_t rnx_days_from_civil(int yy, int mm, int dd)
{
    int era, yoe, doy, doe;
    yy -= mm <= 2;
    era = (yy >= 0 ? yy : yy - 399) / 400;
    yoe = yy - era * 400;
    doy = (153 * (mm + (mm > 2 ? -3 : 9)) + 2) / 5 + dd - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + doe - 719468;
}

/** Timestamp of \a ep in units of 1e-7 s, continuous across days (two-digit
 * years are interpreted as in RINEX 2).  SOC2 rate predictors use
 * differences of these. */
static inline int64_t rnx_epoch_time_e7(const struct rinex_epoch *ep)
{
    int ymd = ep->yyyy_mm_dd;
    int yy = ymd / 10000;
    if (yy < 100)
        yy += yy < 80 ? 2000 : 1900;
    return (rnx_days_from_civil(yy, ymd / 100 % 100, ymd % 100) * 86400
        + (ep->hh_mm / 100) * 3600 + (ep->hh_mm % 100) * 60) * 10000000LL
        + ep->sec_e7;
}

/** Predicted SSI digit ('1'..'9') for a signal-strength value \a s in
 * milli-units (thousandths of dB-Hz), per the SOC2 SSI reference rule. */
static inline char rnx_ssi_from_strength(int64_t s)
{
    int64_t digit = s < 0 ? 1 : s / 6000;
    digit = digit < 1 ? 1 : digit > 9 ? 9 : digit;
    return (char)('0' + digit);
}

/* Negative SRNX error numbers, returned by srnx.c's functions (declared
 * in srnx.h) and by the internal helpers declared below.
 */
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
    SRNX_IMPLEMENTATION_ERROR = -10,
    SRNX_BAD_DIGEST = -11
};

/** rnx_page_size is the value of sysconf(_SC_PAGE_SIZE). */
extern long rnx_page_size;

/** rnx_version_type is the header text "RINEX VERSION / TYPE".
 * It should be treated as having length 20.
 */
extern const char rnx_version_type[];

/** rnx_v234_parser is a RINEX v2.xx, v3.xx or v4.xx parser. */
struct rnx_v234_parser
{
    /** base is the standard rinex_parser contents. */
    struct rinex_parser base;

    /** parse_ofs is the current read offset in base.stream->buffer. */
    uint64_t parse_ofs;

    /** buffer_alloc is the allocated length of #base.buffer. */
    int buffer_alloc;

    /** sats_alloc is the allocated length of #base.sats. */
    int sats_alloc;

    /** obs_alloc is the allocated length of #base.lli, #base.ssi and
     * #base.obs.
     */
    int obs_alloc;

    /** own_stream is a stream created by rinex_open() (for example, to
     * convert CR LF line endings), destroyed along with this parser.
     */
    struct rinex_stream *own_stream;
};

/** obs_state holds the differential state and flags for a single observation. */
struct obs_state
{
    /** lli is the last LLI value for the observation. */
    char lli;
    /** ssi is the last SSI value for the observation. */
    char ssi;
    /** order is the nominal (final) order of differences for the observation. */
    unsigned char order;
    /** used is how many elements of #diff are currently used. */
    unsigned char used;
    /** diff holds the higher-order differences (old diff[1..5]). */
    int64_t diff[5];
    /** value is the current observation value (old diff[0]). */
    int64_t value;
};

/** crx_v23_parser is a CRX (Hatanaka compressed) v2.xx or v3.xx parser. */
struct crx_v23_parser
{
    /** base describes the uncompressed RINEX content. */
    struct rnx_v234_parser base;

    /** epoch_alloc is the allocated length of #epoch_text. */
    int epoch_alloc;

    /** epoch_text is the current uncompressed epoch header line, ending
     * with a line feed ('\n') character.
     */
    char *epoch_text;

    /** state holds the differential state and flags for each observation.
     * The allocated length is #base.obs_alloc.
     */
    struct obs_state *state;

    /** prev_state is the second buffer for double-buffered satellite
     * reordering.  On delta epochs, state and prev_state are swapped,
     * so the old state can be read from prev_state while writing into
     * state without any per-epoch allocation.
     * The allocated length is #base.obs_alloc.
     */
    struct obs_state *prev_state;

    /** clk_state holds the differential state for the receiver clock offset. */
    struct obs_state clk_state;

    /** sattab is a reusable satellite reorder table for v2 delta epochs.
     * sattab[i] is the index of the i-th new satellite in the old list,
     * or -1 if it is new.
     */
    int *sattab;

    /** sattab_alloc is the allocated length of #sattab. */
    int sattab_alloc;
};

/** Initializes #rnx_page_size and other internal mmap state.
 *
 * \returns Zero on success, non-zero (setting errno) on failure.
 */
int rnx_mmap_init(void);

/** Memory-maps a zero-padded block from \a fd for reading.
 *
 * \warning \a offset and \a tot_len must be aligned to multiples of #rnx_page_size.
 * \param[in] fd File descriptor to map from.
 * \param[in] offset Byte offset within file to map at.
 * \param[in] len Length of file region to map.
 * \param[in] tot_len Total length of region to memory-map.
 * \returns MAP_FAILED on failure (setting errno), otherwise a non-null
 *   pointer to the memory-mapped file contents.
 */
void *rnx_mmap_padded(int fd, off_t offset, size_t f_len, size_t tot_len);

/** Searches for \a needle in \a haystack.
 *
 * \param[in] haystack Data to search in.
 * \param[in] h_size Number of bytes in \a haystack.
 * \param[in] needle Data to search for.
 * \param[in] n_size Number of bytes in \a needle.
 * \returns A pointer to the first instance of \a needle within
 *   \a haystack, or NULL if \a haystack does not contain \a needle.
 */
void *rnx_memmem
(
    const char *haystack, size_t h_size,
    const char *needle, size_t n_size
);

/** Search for a RINEX header line.
 *
 * \param[in] in Buffer containing the entire plausible header.
 * \param[in] in_size Length of \a in.
 * \param[in] header Text of header to search for.
 * \param[in] sizeof_header sizeof(header), including trailing NULL.
 * \returns RINEX error code on failure, otherwise the offset of the
 *   start of the header line.
 */
int rnx_find_header
(
    const char in[],
    size_t in_size,
    const char header[],
    size_t sizeof_header
);

/** rnx_ensure_sats ensures that \a p has enough capacity to hold
 * \a p->base.epoch.n_sats satellites of data.
 */
rinex_error_t rnx_ensure_sats(struct rnx_v234_parser *p);

/** rnx_get_newlines tries to ensure multiple lines are in \a p->stream.
 *
 * \param[in,out] p Parser needing data to be copied.
 * \param[in] p_whence Offset at which to start counting.
 * \param[out] p_body_ofs Receives offset of body data within
 *   \a p->stream->buffer.
 * \param[in] n_header Number of "header" lines to fetch.
 * \param[in] n_body Number of "body" lines to fetch.
 * \returns Number of bytes in p->stream needed to get \a n_header +
 *   \a n_body newlines, or non-positive rinex_error_t value on failure.
 *   Either count may be zero; if both are, the result is \a *p_whence,
 *   which must then be positive.
 */
int rnx_get_newlines(
    struct rinex_parser *p,
    uint64_t *p_whence,
    int *p_body_ofs,
    int n_header,
    int n_body
);

/** rnx_errorf formats an error message into \a p->errmsg.
 *
 * \param[in,out] p Parser whose error message is set.
 * \param[in] fmt printf-style format string.
 * \return A pointer to \a p->errmsg.
 */
const char *rnx_errorf(struct rinex_parser *p, const char *fmt, ...);

/** rnx_copy_line copies text to \a p->base.buffer.
 *
 * This copies text in the range [ \a p->parse_ofs, \a eol_ofs ).
 *
 * \param[in,out] p Parser that has a line of text.
 * \param[in] eol_ofs One past the the end-of-line character,
 *   relative to \a p->base.stream->buffer.
 * \return RINEX_ERR_SYSTEM on memory allocation failure, or else
 *   RINEX_SUCCESS.
 */
int rnx_copy_text(
    struct rnx_v234_parser *p,
    int eol_ofs
);

/** Parses a RINEX v2-style header line from \a line into \a p.
 *
 * \param[out] p Receives parsed header information.
 * \param[in] line Header line.
 */
/** rnx_v34_parse_time parses the date and time fields of the RINEX 3/4
 * epoch line \a line (at least 35 characters, with the epoch flag in
 * column 32) into \a epoch.  For special events (flags 2-5) the date
 * and time may be entirely blank, which gives a zero date and time.
 * \returns 0 on success, RINEX_ERR_BAD_FORMAT if a field is malformed.
 */
int rnx_v34_parse_time(struct rinex_epoch *epoch, const char *line);

int rnx_v2_parse_time(
    struct rnx_v234_parser *p,
    const char *line
);

/** rnx_read_v2 reads an observation data record from \a p_. */
rinex_error_t rnx_read_v2(struct rinex_parser *p_);

/** rnx_read_v34 reads an observation data record from \a p_. */
rinex_error_t rnx_read_v34(struct rinex_parser *p_);

/** rnx_has_crlf checks whether the first line in \a stream's buffer
 * ends with CR LF.
 */
int rnx_has_crlf(const struct rinex_stream *stream);

/** rnx_crlf_stream creates a stream that reads from \a inner, dropping
 * the CR from each CR LF pair.  Destroying it does not destroy \a inner.
 * Returns NULL on allocation failure.
 */
struct rinex_stream *rnx_crlf_stream(struct rinex_stream *inner);

/** rnx_free_v23 deallocates \a p_, which must be a rnx_v234_parser. */
void rnx_free_v23(struct rinex_parser *p_);

/** crx_free_v23 deallocates \a p_, which must be a crx_v23_parser. */
void crx_free_v23(struct rinex_parser *p_);

const char *rnx_open_v23(
    struct rnx_v234_parser *p,
    struct rinex_stream *stream,
    int hdr_ofs
);

const char *crx_open_v23(
    struct crx_v23_parser *crx,
    struct rinex_stream *stream
);

/** (Re-)initializes \a *p_srnx for a new open attempt: allocates a new
 * srnx_reader if \a *p_srnx is NULL, else releases the previous reader's
 * mapped data and observation-code tables; either way, the result is
 * zeroed with \a sdir_offset, \a epoc_offset and \a evtf_offset set to
 * -1.  \a struct srnx_reader is opaque outside srnx.c, so this is the
 * only way for another translation unit (e.g. srnx_parse.c) to obtain
 * one.
 *
 * \param[in,out] p_srnx Pointer to SRNX reader object to (re-)initialize.
 * \returns The (possibly newly allocated) reader, or NULL on ENOMEM.
 */
struct srnx_reader *srnx_reset(struct srnx_reader **p_srnx);

/** Parses the SRNX/RHDR chunk headers from an in-memory buffer into an
 * already-reset \a srnx (see srnx_reset()).
 *
 * On success, \a srnx->data is set to \a data and \a srnx->data_mapped
 * is left at 0 (the buffer is borrowed, not owned); the caller decides
 * whether/how to release \a data.
 *
 * \param[in,out] srnx SRNX reader to populate; must already be reset.
 * \param[in] data Pointer to the whole SRNX file contents in memory.
 * \param[in] size Number of bytes at \a data.
 * \param[in] flags SRNX_OPEN_UNCHECKED to skip the file-level digest check.
 * \returns Zero on success, else a positive errno or negative srnx_errno.
 */
int srnx_open_buffer(
    struct srnx_reader *srnx,
    const char *data,
    size_t size,
    int flags
);

/** Parses the epoch-presence run-length encoding out of the SATE chunk
 * for satellite \a name, without allocating a rinex_satellite_data.
 *
 * \param[in] srnx SRNX reader to use.
 * \param[in] name Satellite name to look up.
 * \param[in] n_signals Number of SLEB128 signal offsets to skip in the
 *   SATE payload before the presence run-length data.
 * \param[in,out] p_runs Receives a (re)allocated array of presence runs;
 *   must be initialized to NULL for a fresh call.
 * \param[in,out] p_n_runs Receives the number of valid entries in
 *   \a *p_runs.
 * \param[in,out] p_alloc Receives the allocated length of \a *p_runs.
 * \param[out] p_obs_count Receives the total number of present epochs
 *   (the sum of each run's length).
 * \returns Zero on success, else a positive errno or negative srnx_errno.
 */
int srnx_get_presence_runs(
    struct srnx_reader *srnx,
    struct srnx_satellite_name name,
    int n_signals,
    struct rinex_range **p_runs,
    int *p_n_runs,
    int *p_alloc,
    int *p_obs_count
);

/** Fills \a n_obs[32] (indexed the same way as rinex_parser.n_obs) from
 * \a srnx's already-parsed RHDR observation-code tables.
 *
 * \param[in] srnx SRNX reader to use.
 * \param[out] n_obs Receives the observation-code count per system.
 */
void srnx_fill_n_obs(struct srnx_reader *srnx, unsigned char n_obs[32]);

/** Creates a row-oriented rinex_parser backed by an SRNX file.
 *
 * \a stream must already have its entire contents available in
 * \a stream->buffer (SRNX requires random access).  As with the text
 * and CRX parsers, the created parser borrows \a stream but does not
 * own it: the caller of rinex_open() remains responsible for calling
 * \a stream->destroy() once it is no longer needed.
 *
 * \param[in,out] p_parser Receives a pointer to the created parser.
 * \param[in] stream Input stream with the whole SRNX file buffered.
 * \returns NULL on success, else an explanation of the failure.
 */
const char *srnx_open_parser(
    struct rinex_parser **p_parser,
    struct rinex_stream *stream
);

/** Parses a fixed-point decimal field.
 *
 * A valid field consists of \a width - \a frac - 1 characters as a
 * signed integer, a decimal point ('.'), and \a frac characters as a
 * fractional part.
 *
 * The signed integer is zero or more spaces, an optional minus sign
 * ('-'), and zero or more digits. The fractional part has zero or more
 * digits followed by zero or more spaces (or a newline).
 *
 * Some examples of valid fixed-point decimals from the RINEX 2.11 and
 * 3.04 specifications are (between the double quotes):
 * "  4375274.   " and "         -.120".
 *
 * \param[out] p_out Receives the parsed value, times \a 10**point.
 * \param[in] start Start of the text field to parse.
 * \param[in] width Width of the total field in characters.
 * \param[in] frac Width of the fractional part in characters.
 * \returns Zero on success, else EINVAL if the field was invalid.
 */
int parse_fixed(
    int64_t *p_out,
    const char *start,
    int width,
    int frac);

/** Parses an unsigned integer field of \a width bytes starting at
 * \a start.
 *
 * If the field is all spaces (' '), writes 0 to \a *p_out and returns 0.
 *
 * \param[out] p_out Receives the parsed integer.
 * \param[in] start Start of the text field to parse.
 * \param[in] width Width of field in characters.
 * \returns Zero on success, else EINVAL if the field had any characters
 *   except spaces and digits or if a space followed a digit.
 */
int parse_uint
(
    int *p_out,
    const char *start,
    int width
);

/** Validates a 3-character satellite ID.
 *
 * A valid satellite ID consists of a GNSS system letter followed by
 * two decimal digits. The valid system letters are ' ' (GPS-only RINEX v2),
 * 'G', 'R', 'S', 'E', 'C', 'J', 'I'.
 *
 * \param[out] p Pointer to the parser; used only to set p->errmsg on failure;
 * \param[out] p_sys Receives the system character.
 * \param[out] p_num Receives the satellite number (01..99).
 * \param[in] satid Pointer to the 3-character satellite ID.
 * \returns Zero on success, else EINVAL if the ID is invalid.
 */
int rnx_parse_satid
(
    struct rinex_parser *p,
    char *p_sys,
    unsigned char *p_num,
    const char satid[3]
);

/* The following macro-heavy code supports runtime selection of function
 * variants that use platform-specific instructions such as SIMD.
 */

#define CHECK_NIL(RETTYPE, SUFFIX, PROTOTYPE)

#define CHECK_std(RETTYPE, SUFFIX, PROTOTYPE) \
    if (!rnx_##SUFFIX || (simd && !strcmp(simd, "std"))) \
    { \
        extern RETTYPE rnx_std_##SUFFIX PROTOTYPE; \
        rnx_##SUFFIX = rnx_std_##SUFFIX; \
    }

#if defined(HAVE_AVX2)
# define CHECK_avx2(RETTYPE, SUFFIX, PROTOTYPE) \
    if (__builtin_cpu_supports("avx2") && (!simd || !strcmp(simd, "avx2"))) \
    { \
        extern RETTYPE rnx_avx2_##SUFFIX PROTOTYPE; \
        rnx_##SUFFIX = rnx_avx2_##SUFFIX; \
    }
#else
# define CHECK_avx2(RETTYPE, SUFFIX, PROTOTYPE) \
    CHECK_NIL(RETTYPE, SUFFIX, PROTOTYPE)
#endif

#if defined(HAVE_NEON)
/* NEON is mandatory for ARMv8-a and later, and not detectable from
 * user-space. The "standard" is looking at /proc/cpuinfo (under Linux)
 * or an equivalent declaration from the OS. Instead, we just assume
 * the build did a reasonable thing.
 */
# define CHECK_neon(RETTYPE, SUFFIX, PROTOTYPE) \
    if (!simd || !strcmp(simd, "neon")) \
    { \
        extern RETTYPE rnx_neon_##SUFFIX PROTOTYPE; \
        rnx_##SUFFIX = rnx_neon_##SUFFIX; \
    }
#else
# define CHECK_neon(RETTYPE, SUFFIX, PROTOTYPE) \
    CHECK_NIL(RETTYPE, SUFFIX, PROTOTYPE)
#endif

#define DO_CHECK(MACRO, RETTYPE, SUFFIX, PROTOTYPE) \
    MACRO(RETTYPE, SUFFIX, PROTOTYPE)

#define CHECK_MULTIPLE(RETTYPE, SUFFIX, PROTOTYPE, SIMD_0, SIMD_1, \
    SIMD_2, SIMD_3, SIMD_4, SIMD_5, SIMD_6, SIMD_7, SIMD_8, SIMD_9, ...) \
        CHECK_##SIMD_0(RETTYPE, SUFFIX, PROTOTYPE) \
        CHECK_##SIMD_1(RETTYPE, SUFFIX, PROTOTYPE) \
        CHECK_##SIMD_2(RETTYPE, SUFFIX, PROTOTYPE) \
        CHECK_##SIMD_3(RETTYPE, SUFFIX, PROTOTYPE) \
        CHECK_##SIMD_4(RETTYPE, SUFFIX, PROTOTYPE) \
        CHECK_##SIMD_5(RETTYPE, SUFFIX, PROTOTYPE) \
        CHECK_##SIMD_6(RETTYPE, SUFFIX, PROTOTYPE) \
        CHECK_##SIMD_7(RETTYPE, SUFFIX, PROTOTYPE) \
        CHECK_##SIMD_8(RETTYPE, SUFFIX, PROTOTYPE) \
        CHECK_##SIMD_9(RETTYPE, SUFFIX, PROTOTYPE)

#define RNX_RESOLVE(RETTYPE, SUFFIX, PROTOTYPE, ARGS, ...) \
    static RETTYPE rnx_resolve_##SUFFIX PROTOTYPE; \
    RETTYPE (*rnx_##SUFFIX) PROTOTYPE = &rnx_resolve_##SUFFIX; \
    RETTYPE rnx_resolve_##SUFFIX PROTOTYPE \
    { \
        const char *simd = getenv("RINEX_SIMD"); \
        rnx_##SUFFIX = NULL; \
        CHECK_MULTIPLE(RETTYPE, SUFFIX, PROTOTYPE, __VA_ARGS__, \
            std, NIL, NIL, NIL, NIL, NIL, NIL, NIL, NIL, NIL ) \
        return rnx_##SUFFIX ARGS; \
    }

#define RNX_RESOLVE_VOID(SUFFIX, PROTOTYPE, ARGS, ...) \
    static void rnx_resolve_##SUFFIX PROTOTYPE; \
    void (*rnx_##SUFFIX) PROTOTYPE = &rnx_resolve_##SUFFIX; \
    void rnx_resolve_##SUFFIX PROTOTYPE \
    { \
        const char *simd = getenv("RINEX_SIMD"); \
        rnx_##SUFFIX = NULL; \
        CHECK_MULTIPLE(void, SUFFIX, PROTOTYPE, __VA_ARGS__, \
            std, NIL, NIL, NIL, NIL, NIL, NIL, NIL, NIL, NIL) \
        rnx_##SUFFIX ARGS; \
    }

#endif /* !defined(RNX_PRIV_H_a03d7227_442c_4822_a2d2_04bd8c5ff3e4) */
