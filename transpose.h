/** transpose.h - Succinct RINEX internal bit-matrix transposition.
 * Copyright 2021 Michael Poole.
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

#if !defined(TRANSPOSE_H_b9246516_7271_404a_98ce_73a812f308ca)
#define TRANSPOSE_H_b9246516_7271_404a_98ce_73a812f308ca

#if defined(__cplusplus)
extern "C" {
#endif /* defined(__cplusplus) */

/** Assigns #transpose to implementation \a version.
 *
 * \param[in] version Implementation selector: "generic" for a version
 *   that does not use processor-specific instructions, NULL for the
 *   version preferred on this processor, or a platform-specific string
 *   to select a particular implementation.
*/
extern void transpose_select(const char *version);

/** Pointer to function that will transpose a bit matrix, \a count bits
 * wide by \a bits tall, from \a in to \a out.  Each input column from
 * \a in (\a bits in length) is sign-extended when writing to \a out.
 *
 * \param[out] out  Receives transposed, sign-extended values.
 * \param[in] in    Input bit matrix.
 * \param[in] bits  Number of rows in \a in.
 * \param[in] count Number of columns in \a in.  Must be 8, 16 or 32.
 */
extern void (*transpose)(int64_t *out, const char *in, int bits, int count);

#if defined(__cplusplus)
}
#endif /* defined(__cplusplus) */

#endif /* !defined(TRANSPOSE_H_b9246516_7271_404a_98ce_73a812f308ca) */
