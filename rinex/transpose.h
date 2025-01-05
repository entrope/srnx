/** transpose.h - Succinct RINEX internal bit-matrix transposition.
 * Copyright 2021 Michael Poole.
 * SPDX-License-Identifier: MIT-Modern-Variant
 */

#if !defined(TRANSPOSE_H_b9246516_7271_404a_98ce_73a812f308ca)
#define TRANSPOSE_H_b9246516_7271_404a_98ce_73a812f308ca

#if defined(__cplusplus)
extern "C" {
#endif /* defined(__cplusplus) */

/** Transpose a bit matrix, \a count bits wide by \a bits tall, from \a in to \a out.
 * Each input column from \a in (\a bits in length) is sign-extended when writing to \a out.
 *
 * \param[out] out  Receives transposed, sign-extended values.
 * \param[in] in    Input bit matrix.
 * \param[in] bits  Number of rows in \a in.
 * \param[in] count Number of columns in \a in.  Must be 8, 16 or 32.
 */
extern void transpose(int64_t *out, const char *in, int bits, int count);

#if defined(__cplusplus)
}
#endif /* defined(__cplusplus) */

#endif /* !defined(TRANSPOSE_H_b9246516_7271_404a_98ce_73a812f308ca) */
