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

#include "rinex.h"

/** RINEX_EXTRA is the extra length of stream buffers to ease vectorization. */
#define RINEX_EXTRA 80

/** BLOCK_SIZE is how much data we normally try to read into a buffer. */
#define BLOCK_SIZE (1024 * 1024 - RINEX_EXTRA)

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

#endif /* !defined(RINEX_P_H_a03d7227_442c_4822_a2d2_04bd8c5ff3e4) */
