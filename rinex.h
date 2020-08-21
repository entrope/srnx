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

#include <stdint.h>

/* The design for this API is to provide a simple "pull"-based API for
 * reading data from a RINEX-like file, one epoch or event at a time,
 * with minimal data allocations and simple handling.
 */

#if defined(__cplusplus)
extern "C" {
#endif /* defined(__cplusplus) */

/** RINEX_EXTRA is the extra length of stream buffers to ease vectorization. */
#define RINEX_EXTRA 31

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

/** rinex_signal identifies the combination of a satellite (SV) and a
 * signal from it.
 *
 * The #u64 value allows for easy comparison, hash indexing, and similar
 * operations.  The #id structure contains the SV number and observation
 * code in their normal RINEX 2.x or 3.x formats.
 */
union rinex_signal
{
    /** Signal identifier as a single integer. */
    uint64_t u64;

    /** Signal identifier decomposed into satellite number and
     * observation code.
     */
    struct
    {
        /** Satellite number from which this signal was observed. */
        char sv[4];

        /** Observation code for the measurement. */
        char obs[4];
    } id;
};

typedef union rinex_signal rinex_signal_t;

/** rinex_epoch holds the date, time, epoch flag, count of satellites
 * (or special records or cycle slips), and receiver clock offset.
 */
struct rinex_epoch
{
    /** Decimal-coded date.
     * Contains the sum year * 1000 + month * 100 + day.
     */
    int yyyy_mm_dd;

    /** Decimal-coded minute of day.
     * Contains the sum of hour * 100 + minute.
     */
    short hh_mm;

    /** Epoch flag (normally '0' through '6'). */
    char flag;

    /** Seconds of minute times 1e7. */
    int sec_e7;

    /** Number of satellites or special event records. */
    int n_sats;

    /** Fractional clock offset, times 1e12. */
    int64_t clock_offset;
};

/** rinex_error indicates the result of parsing a RINEX file. */
enum rinex_error
{
    /** RINEX_ERR_NOT_OBSERVATION indicates that the RINEX file is
     * not an observation-type file.
     */
    RINEX_ERR_NOT_OBSERVATION = -6,

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
    RINEX_ERR_BAD_FORMAT = -4,

    /** RINEX_ERR_SYSTEM indicates a system-level failure, as indicated
     * in errno.
     */
    RINEX_ERR_SYSTEM = -2,

    /** RINEX_EOF indicates the end of file was reached. */
    RINEX_EOF = -1,

    /** RINEX_SUCCESS indicates that no error occurred. */
    RINEX_SUCCESS = 0
};

typedef enum rinex_error rinex_error_t;

/** rinex_parser is an abstract base type for loading data from
 * RINEX-like files.
 */
struct rinex_parser
{
    /** buffer_len is the number of bytes of data in #buffer. */
    int buffer_len;

    /** signal_len is the number of observations from this epoch. */
    int signal_len;

    /** buffer contains text related to the current data.
     *
     * Before #read is called, this holds the file header.
     *
     * When a special event is read, this holds the event records;
     * zero or more lines, counted by \a epoch.n_sats, with '\n' as the
     * line terminator.
     *
     * When an observation or cycle slip record is read, this holds the
     * observation data (#signal_len entries, each 16 bytes long).
     */
    char *buffer;

    /** signal contains the signal identifiers for each signal. */
    rinex_signal_t *signal;

    /** stream is the source of data for this file parser. */
    struct rinex_stream *stream;

    /** epoch identifies the current record's epoch. */
    struct rinex_epoch epoch;

    /** read is the reader function.  It is called to retrieve the next
     * epoch-level record from the file.
     *
     * \param[in] p Parser to read from.
     * \returns A rinex_error status code on failure, 0 on EOF, 1 on
     *   success.
     */
    int (*read)(struct rinex_parser *p);

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
 * \returns A rinex_error status code.
 */
rinex_error_t rinex_open(struct rinex_parser **p_parser, struct rinex_stream *stream);

/** rinex_parse_obs parses a 14-character observation code, and returns
 * the value times 1000.
 *
 * \param[in] c A RINEX observation value in F14.3 format.
 * \returns The value times 1000, or INT64_MIN on format error.
 */
int64_t rinex_parse_obs(const char c[]);

struct rinex_stream *rinex_mmap_stream(const char *filename);
struct rinex_stream *rinex_stdio_stream(const char *filename);
struct rinex_stream *rinex_stdin_stream(void);

#if defined(__cplusplus)

inline bool operator<(rinex_signal a, rinex_signal b)
{
    return a.u64 < b.u64;
}

inline bool operator==(rinex_signal a, rinex_signal b)
{
    return a.u64 == b.u64;
}

}
#endif /* defined(__cplusplus) */

#endif /* !defined(RINEX_H_b56f2d84_3a1e_4708_b3c2_6655d0b571c5) */
