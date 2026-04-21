/** test_digest.c - TAP test for the rnx_digest abstraction.
 * Copyright 2026 Michael Poole.
 * SPDX-License-Identifier: MIT-Modern-Variant
 */

#include "rinex/digest.h"

#include <stdio.h>
#include <string.h>
#include <tap.h>

static uint32_t crc32c_of(const void *data, size_t len)
{
    struct rnx_digest d;
    unsigned char out[4];

    if (rnx_digest_init(&d, 2) != 0)
        BAIL_OUT("unable to init CRC32C");
    rnx_digest_update(&d, data, len);
    rnx_digest_final(&d, out);
    return (uint32_t)out[0]
        | ((uint32_t)out[1] << 8)
        | ((uint32_t)out[2] << 16)
        | ((uint32_t)out[3] << 24);
}

int main(void)
{
    struct rnx_digest d;
    unsigned char buffer[32];
    size_t ii;

    plan(20);

    /* rnx_digest_length: supported ids. */
    ok(rnx_digest_length(0) == 0, "length id=0 is 0");
    ok(rnx_digest_length(2) == 4, "length id=2 (CRC32C) is 4");
    ok(rnx_digest_length(6) == 32, "length id=6 (SHA-256) is 32");
    ok(rnx_digest_length(7) == 64, "length id=7 (SHA3-512) is 64");
    ok(rnx_digest_length(20) == 16, "length id=20 (BLAKE2b-128) is 16");
    ok(rnx_digest_length(21) == 32, "length id=21 (BLAKE2b-256) is 32");
    ok(rnx_digest_length(22) == 64, "length id=22 (BLAKE2b-512) is 64");
    ok(rnx_digest_length(1) < 0, "length unknown id=1 is negative");

    /* Init: id 0 and 2 supported; others not yet. */
    ok(rnx_digest_init(&d, 0) == 0, "init id=0 (null) ok");
    ok(rnx_digest_init(&d, 2) == 0, "init id=2 (CRC32C) ok");
    ok(rnx_digest_init(&d, 6) == -1, "init id=6 (SHA-256) not yet supported");
    ok(rnx_digest_init(&d, 7) == -1, "init id=7 (SHA3-512) not supported");

    /* CRC32C test vectors (Castagnoli polynomial 0x1EDC6F41).
     * The canonical RFC 3720 vector is CRC32C("123456789") = 0xE3069283.
     * Empty input yields 0.
     */
    ok(crc32c_of("", 0) == 0x00000000,
        "CRC32C of empty string is 0");
    ok(crc32c_of("123456789", 9) == 0xE3069283,
        "CRC32C of \"123456789\" is 0xE3069283");
    for (ii = 0; ii < sizeof buffer; ii++)
        buffer[ii] = 0;
    ok(crc32c_of(buffer, sizeof buffer) == 0x8a9136aa,
        "CRC32C of 32 zero bytes is 0x8a9136aa");
    for (ii = 0; ii < sizeof buffer; ii++)
        buffer[ii] = 0xFF;
    ok(crc32c_of(buffer, sizeof buffer) == 0x62a8ab43,
        "CRC32C of 32 bytes of 0xFF is 0x62a8ab43");
    for (ii = 0; ii < sizeof buffer; ii++)
        buffer[ii] = (unsigned char)ii;
    ok(crc32c_of(buffer, sizeof buffer) == 0x46dd794e,
        "CRC32C of 32 bytes incrementing 0..0x1f is 0x46dd794e");
    for (ii = 0; ii < sizeof buffer; ii++)
        buffer[ii] = (unsigned char)(0x1f - ii);
    ok(crc32c_of(buffer, sizeof buffer) == 0x113fdb5c,
        "CRC32C of 32 bytes decrementing 0x1f..0 is 0x113fdb5c");

    /* Split update should match a single-shot update. */
    {
        struct rnx_digest a, b;
        unsigned char oa[4], ob[4];

        rnx_digest_init(&a, 2);
        rnx_digest_update(&a, "123456789", 9);
        rnx_digest_final(&a, oa);

        rnx_digest_init(&b, 2);
        rnx_digest_update(&b, "1234", 4);
        rnx_digest_update(&b, "5", 1);
        rnx_digest_update(&b, "6789", 4);
        rnx_digest_final(&b, ob);

        ok(memcmp(oa, ob, 4) == 0, "split update matches single-shot update");
    }

    /* Null digest writes no bytes; a canary should remain untouched. */
    {
        unsigned char buf[4] = { 0xAA, 0xBB, 0xCC, 0xDD };

        rnx_digest_init(&d, 0);
        rnx_digest_update(&d, "whatever", 8);
        rnx_digest_final(&d, buf);
        ok(buf[0] == 0xAA && buf[1] == 0xBB
            && buf[2] == 0xCC && buf[3] == 0xDD,
            "null digest final does not write output");
    }

    done_testing();
}
