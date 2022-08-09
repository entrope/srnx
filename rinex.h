/** rinex.h - Reader for RINEX observation files.
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

#if !defined(RINEX_H_b56f2d84_3a1e_4708_b3c2_6655d0b571c5)
#define RINEX_H_b56f2d84_3a1e_4708_b3c2_6655d0b571c5

#include "rinex_epoch.h"

/* The design for this API is to provide a simple "pull"-based API for
 * reading data from a RINEX-like file, one epoch or event at a time,
 * with minimal data allocations and simple handling.
 */

#if defined(__cplusplus)
extern "C" {
#endif /* defined(__cplusplus) */

/** rinex_stream is a utility class that abstracts filesystem operations
 * from rinex_file_load().  It provides a buffered view into a stream
 * that can move forward in user-controlled step sizes.
 */
struct rinex_stream
{
    /** Advances #buffer by at least \a step bytes, trying to ensure
     * #size is at least \a req_size.
     *
     * \param[in,out] stream Pointer to the stream being used.
     * \param[in] req_size Number of bytes of real data requested to be
     *   in #buffer.
     * \param[in] step Number of bytes to advance in #buffer.
     * \returns Zero on success, a C errno value on failure.
     */
    int (*advance)(
        struct rinex_stream *stream,
        unsigned int req_size,
        unsigned int step
    );

    /** Cleans up \a stream and any dynamically allocated memory it uses. */
    void (*destroy)(struct rinex_stream *stream);

    /** Pointer to the current buffer in the file.
     * This must be readable for at least #size plus RINEX_EXTRA bytes.
     */
    char *buffer;

    /** Number of bytes of real data in #buffer.
     * #buffer is readable for at least RINEX_EXTRA bytes past that
     * real data.
     */
    unsigned int size;
};

/** rinex_error indicates the result of parsing a RINEX file. */
enum rinex_error
{
    /** RINEX_ERR_NOT_OBSERVATION indicates that the RINEX file is
     * not an observation-type file.
     */
    RINEX_ERR_NOT_OBSERVATION = -4,

    /** RINEX_ERR_UNKNOWN_VERSION indicates that the RINEX file is
     * not supported.  (Currently supported: 2.xx and 3.xx.)
     */
    RINEX_ERR_UNKNOWN_VERSION = -5,

    /** RINEX_ERR_BAD_FORMAT indicates a line with invalid content:
     * - First header is not "RINEX VERSION / TYPE".
     * - Version number is malformed.
     * - "END OF HEADER" followed by non-whitespace before newline.
     * - A header line shorter than 61 characters.
     * - A header line longer than 80 characters.
     * When reading observations:
     * - EPOCH/SAT or EVENT FLAG record is invalid.
     */
    RINEX_ERR_BAD_FORMAT = -2,

    /** RINEX_ERR_SYSTEM indicates a system-level failure, as indicated
     * in errno.
     */
    RINEX_ERR_SYSTEM = -1,

    /** RINEX_EOF indicates the end of file was reached. */
    RINEX_EOF = 0,

    /** RINEX_SUCCESS indicates that no error occurred. */
    RINEX_SUCCESS = 1
};

typedef enum rinex_error rinex_error_t;

/** RINEX_MIN_OBS is the minimum legal value for a RINEX observation. */
#define RINEX_MIN_OBS -999999999999ULL

/** RINEX_MAX_OBS is the maximum legal value for a RINEX observation. */
#define RINEX_MAX_OBS 9999999999999ULL

/** rinex_sv_info holds information for a satellite observed during an
 * epoch.
 */
struct rinex_sv_info
{
    /** system is the ASCII character for the RINEX satellite system.
     * It is one of ' ' (for old, GPS-only RINEXv2 files), 'G', 'R',
     * 'S', 'E', 'C', 'J' or 'I'.
     */
    char system;

    /** number is the binary-coded satellite number, 01 to 99. */
    unsigned char number;

    /** base is the first observation slot assigned to this satellite.
     * The number of observations for it are given by
     * \a rinex_parser.n_obs[system&31].
     */
    unsigned short obs_0;
};

/** rinex_parser is an abstract base type for loading data from files
 * containing RINEX observation-like data.
 */
struct rinex_parser
{
    /** epoch identifies the current record's epoch. */
    struct rinex_epoch epoch;

    /** buffer_len is the number of bytes of data in #buffer. */
    int buffer_len;

    /** error_line indicates where a parse error occurred. */
    int error_line;

    /** buffer contains text related to the current record.
     *
     * Before #read is called, this holds the RINEX file header as text.
     * 
     * After #read is called and a special event is read (as indicated
     * by \a epoch.flag ), this holds the event records:
     * \a epoch.n_sats+1 lines, with '\n' as the line terminator.
     * The first line holds the epoch information (in case, for example,
     * the presence or absence of a timestamp is significant).
     */
    char *buffer;

    /** sats holds metadata about observed satellites.
     *
     * Specifically, \a sats[i] identifies the i'th observed satellite,
     * along with its base index in #obs, #lli and #ssi.  The number of
     * valid entries is given by \a epoch.n_sats.  This is only valid
     * when \a epoch.flag is 0, 1 or 6.
     */
    struct rinex_sv_info *sats;

    /** obs contains the parsed observation values, times 1000.
     *
     * A value less than RINEX_MIN_OBS indicates the observation was
     * missing or blank during this epoch.  This is only valid when
     * \a epoch.flag is 0, 1 or 6.  The indexing is determined by
     * #sats.
     */
    int64_t *obs;

    /** lli contains the loss-of-lock indicators. */
    char *lli;

    /** ssi contains the signal strength indocators. */
    char *ssi;

    /** n_obs counts the possible observations per satellite system.
     *
     * For the satellite system with identifier 'A', n_obs['A' & 31]
     * indicates the number of observations possible for it.
     *
     * (RINEX 2.11 defines 26 observation codes, and RINEX 3.05 and 4.00
     * define up to about 120 codes for the extant satellite systems,
     * plus up to 9 pseudo-observables with receiver channel numbers.)
     */
    unsigned char n_obs[32];

    /** stream is the source of data for this file parser. */
    struct rinex_stream *stream;

    /** read is the reader function.  It is called to retrieve the next
     * epoch-level record from the file.
     *
     * \param[in] p Parser to read from.
     * \returns A rinex_error status code.
     */
    rinex_error_t (*read)(struct rinex_parser *p);

    /** destroy frees any dynamically allocated memory and deallocates
     * the parser.
     *
     * \param[in] p Parser to destroy.
     */
    void (*destroy)(struct rinex_parser *p);
};

/** rinex_open creates a parser that reads data from \a stream.
 *
 * \param[in,out] p_parser Receives a pointer to the created parser.  If
 *   this is not null, rinex_open() first calls rinex_parser.destroy()
 *   on the old parser.
 * \param[in] stream Input stream to use for the parser.
 * \returns NULL on success, else an explanation of the failure.
 */
const char *rinex_open(struct rinex_parser **p_parser, struct rinex_stream *stream);

/** Finds the start of the first line with the given header label.
 *
 * \warning Has undefined behavior for the first header.  This should
 *   not be a problem because the first header should be at p->buffer
 *   anyway.
 * \param[in] p RINEX parser with header in \a p->buffer.
 * \param[in] label Header label to search for.
 * \param[in] sizeof_label Size of \a label, including nul terminator.
 * \returns A pointer into \a rnx->header such that \a !strcmp(ptr+60,label),
 *   or NULL if there is no header with the requested label.
 */
const char *rinex_find_header
(
    const struct rinex_parser *p,
    const char label[],
    unsigned int sizeof_label
);

struct rinex_stream *rinex_mmap_stream(const char *filename);
struct rinex_stream *rinex_stdio_stream(const char *filename);
struct rinex_stream *rinex_stdin_stream(void);

#if defined(__cplusplus)
}
#endif /* defined(__cplusplus) */

#endif /* !defined(RINEX_H_b56f2d84_3a1e_4708_b3c2_6655d0b571c5) */
