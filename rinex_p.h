/* rinex_p.h - Private definitions for RINEX observation parsing.
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

#if !defined(RINEX_P_H_a03d7227_442c_4822_a2d2_04bd8c5ff3e4)
#define RINEX_P_H_a03d7227_442c_4822_a2d2_04bd8c5ff3e4

#include <fcntl.h>
#include <stddef.h>

#include "rinex.h"

#if defined(__AVX2__)
# include <x86intrin.h>
#elif defined(__ARM_NEON)
# include <arm_neon.h>
#endif

/** RINEX_EXTRA is the extra length of stream buffers to ease vectorization. */
#define RINEX_EXTRA 80

/** BLOCK_SIZE is how much data we normally try to read into a buffer. */
#define BLOCK_SIZE (1024 * 1024 - RINEX_EXTRA)

/** page_size is the value of sysconf(_SC_PAGE_SIZE). */
extern long page_size;

/** rnx_v23_parser is a RINEX v2.xx or v3.xx parser. */
struct rnx_v23_parser
{
    /** base is the standard rinex_parser contents. */
    struct rinex_parser base;

    /** buffer_alloc is the allocated length of #base.buffer. */
    int buffer_alloc;

    /** obs_alloc is the allocated length of #base.lli, #base.ssi and
     * #base.obs.
     */
    int obs_alloc;

    /** parse_ofs is the current read offset in base.stream->buffer. */
    uint64_t parse_ofs;
};

/** crx_v23_parser is a CRX (Hatanaka compressed) v2.xx or v3.xx parser. */
struct crx_v23_parser
{
    /** base describes the uncompressed RINEX content. */
    struct rnx_v23_parser base;

    /** epoch_alloc is the allocated length of #epoch_text. */
    int epoch_alloc;

    /** max_order is the maximum differential order for any (active)
     * observation.
     */
    int max_order;

    /** epoch_text is the current uncompressed epoch header line, ending
     * with a line feed ('\n') character.
     */
    char *epoch_text;

    /** order is a pair of entries for each observation. \a order[2*n+0]
     * is the differential order of each observation, up to 9.
     * \a order[2*n+1] is how much of #diff is valid for the observation.
     * The allocated length of `order` is 2*#base.obs_alloc.
     */
    unsigned char *order;

    /** diff holds the differential state for each observation.  The
     * allocated length is #base.obs_alloc * 10, in order-major layout.
     * That is, \a diff[obs+n*base.obs_alloc] is the \a n'th-order
     * history for observation \a obs.  Unused space is zero-filled.
     */
    int *diff;
};

/** Initializes #page_size and other internal mmap state.
 *
 * \returns Zero on success, non-zero (setting errno) on failure.
 */
int rnx_mmap_init(void);

/** Memory-maps a zero-padded block from \a fd for reading.
 *
 * \warning \a offset and \a tot_len must be aligned to multiples of #page_size.
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
void *memmem
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
 */
int rnx_get_newlines(
    struct rinex_parser *p,
    uint64_t *p_whence,
    int *p_body_ofs,
    int n_header,
    int n_body
);

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
    struct rnx_v23_parser *p,
    int eol_ofs
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
int parse_fixed
(
    int64_t *p_out,
    const char *start,
    int width,
    int frac
);

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

#endif /* !defined(RINEX_P_H_a03d7227_442c_4822_a2d2_04bd8c5ff3e4) */
