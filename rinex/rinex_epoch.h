/* rinex_epoch.h - Declaration of the rinex_epoch structure.
 * Copyright 2020 Michael Poole.
 * SPDX-License-Identifier: MIT-Modern-Variant
 */

#if !defined(RINEX_EPOCH_H_43ca0335_bdd9_4a02_b105_08ea028538a7)
#define RINEX_EPOCH_H_43ca0335_bdd9_4a02_b105_08ea028538a7

#include <stdint.h>

/** rinex_epoch holds the date, time, epoch flag, count of satellites
 * (or special records or cycle slips), and receiver clock offset.
 */
struct rinex_epoch
{
    /* These values could conceivably be packed into three 64-bit fields,
     * but that would leave almost no room for growth and would be
     * awkward to work with, so leave it slightly less compact.
     *
     * log2(10000 * 12 * 31 * 24 * 60) = 32.32 (yyyy .. mm)
     * log2(2*609999999) = 30.2 (seconds)
     * log2(1.1e14) = ~46.6 (clock offset)
     * log2(99999) = 16.6 (picoseconds beyond seconds times 1e7)
     * plus 3 bits for flag, 1 for has_ps and 9 bits for n_sats
     * rounding up each field gives a total of 141 bits
     */

    /** Decimal-coded date.
     * Contains the sum year * 10000 + month * 100 + day.
     * ceil(log2(99991231)) = 27 bits used.
     */
    int yyyy_mm_dd;

    /** Decimal-coded minute of day.
     * Contains the sum of hour * 100 + minute.
     * ceil(log2(5960)) = 13 bits used.
     */
    short hh_mm;

    /** Epoch flag (normally '0' through '6'). */
    char flag;

    /** Nonzero if the epoch has RINEX 4.02's optional five extra
     * digits of seconds (see #sec_ps).  An SRNX file with an EPO2 chunk
     * sets this for every epoch, and one with an EPOC chunk for none.
     */
    char has_ps;

    /** Seconds of minute times 1e7.
     * As in RINEX, this is floored (not rounded) when #sec_ps is used.
     * ceil(log2(609999999)) = 30 bits used.
     */
    int sec_e7;

    /** Number of satellites or special event records. */
    int n_sats;

    /** Fractional clock offset, times 1e12. */
    int64_t clock_offset;

    /** Picoseconds beyond #sec_e7, from 0 to 99999: the RINEX 4.02
     * extra digits of seconds, or zero if #has_ps is zero.
     */
    int sec_ps;
};

#endif /* !defined(RINEX_EPOCH_H_43ca0335_bdd9_4a02_b105_08ea028538a7) */
