/** digest.h - Succinct RINEX digest abstraction.
 * Copyright 2026 Michael Poole.
 * SPDX-License-Identifier: MIT-Modern-Variant
 */

#if !defined(DIGEST_H_07f2f4b1_a38d_4a3a_a5e4_17d1b3c9d4a0)
#define DIGEST_H_07f2f4b1_a38d_4a3a_a5e4_17d1b3c9d4a0

#include <stddef.h>
#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

/** Returns the output length in bytes for digest identifier \a id.
 *
 * \returns Zero for id 0 (null digest), a positive size for supported
 *   digests, or a negative value if \a id is unsupported or unknown.
 */
int rnx_digest_length(int id);

/** Computes the digest of \a len bytes at \a buf using algorithm \a id
 * and writes the result to \a out.
 *
 * \a out must have room for rnx_digest_length(\a id) bytes.  For
 * id == 0 (null digest) no bytes are written.
 *
 * \returns -1 if \a id is unsupported by the build, otherwise the
 *   number of bytes written to \a out.
 */
int rnx_digest(int id, const void *buf, size_t len, unsigned char *out);

#if defined(__cplusplus)
}
#endif

#endif /* !defined(DIGEST_H_07f2f4b1_a38d_4a3a_a5e4_17d1b3c9d4a0) */
