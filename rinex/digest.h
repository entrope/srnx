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

/** Incremental digest state.
 *
 * \a state holds digest-specific context.  Callers treat it as opaque;
 * the union is sized to accommodate the largest supported digest state.
 */
struct rnx_digest
{
    int id;
    int len;
    union {
        uint32_t crc32c;
        /* Reserved for libsodium hash states added in a later step.
         * crypto_hash_sha256_state is ~104 bytes,
         * crypto_generichash_state is ~384 bytes.
         */
        unsigned char reserved[512];
    } state;
};

/** Returns the output length in bytes for digest identifier \a id.
 *
 * \returns Zero for id 0 (null digest), a positive size for supported
 *   digests, or a negative value if \a id is unsupported or unknown.
 */
int rnx_digest_length(int id);

/** Initializes \a d for digest identifier \a id.
 *
 * \returns 0 on success, -1 if \a id is unsupported by the build.
 */
int rnx_digest_init(struct rnx_digest *d, int id);

/** Feeds \a len bytes from \a buf into digest \a d. */
void rnx_digest_update(struct rnx_digest *d, const void *buf, size_t len);

/** Finalizes \a d and writes \a d->len bytes to \a out. */
void rnx_digest_final(struct rnx_digest *d, unsigned char out[]);

#if defined(__cplusplus)
}
#endif

#endif /* !defined(DIGEST_H_07f2f4b1_a38d_4a3a_a5e4_17d1b3c9d4a0) */
