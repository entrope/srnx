/* rinex_epoch.h - Declaration of the rinex_epoch structure.
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

#if !defined(RINEX_EPOCH_H_43ca0335_bdd9_4a02_b105_08ea028538a7)
#define RINEX_EPOCH_H_43ca0335_bdd9_4a02_b105_08ea028538a7

#include <stdint.h>

/** rinex_epoch holds the date, time, epoch flag, count of satellites
 * (or special records or cycle slips), and receiver clock offset.
 */
struct rinex_epoch
{
    /* These values could conceivably be packed into two 64-bit fields,
     * but that would leave almost no room for growth and would be
     * awkward to work with, so leave it slightly less compact.
     *
     * log2(10000 * 12 * 31 * 24 * 60) = 32.32 (yyyy .. mm)
     * log2(2*609999999) = 30.2 (seconds)
     * log2(1.1e14) = ~46.6 (clock offset)
     * plus 4 bits for flag and 10 bits for n_sats
     */

    /** Decimal-coded date.
     * Contains the sum year * 10000 + month * 100 + day.
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

#endif /* !defined(RINEX_EPOCH_H_43ca0335_bdd9_4a02_b105_08ea028538a7) */
