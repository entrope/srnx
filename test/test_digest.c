/** test_digest.c - TAP test for the rnx_digest abstraction.
 * Copyright 2026 Michael Poole.
 * SPDX-License-Identifier: MIT-Modern-Variant
 */

#include "rinex/digest.h"

#include <blake3.h>
#include <stdio.h>
#include <string.h>
#include <tap.h>

static uint32_t crc32c_of(const void *data, size_t len)
{
    unsigned char out[4];

    if (rnx_digest(2, data, len, out) != 0)
        BAIL_OUT("unable to compute CRC32C");
    return (uint32_t)out[0]
        | ((uint32_t)out[1] << 8)
        | ((uint32_t)out[2] << 16)
        | ((uint32_t)out[3] << 24);
}

static void digest_of(int id, const void *data, size_t len,
    unsigned char out[])
{
    if (rnx_digest(id, data, len, out) != 0)
        BAIL_OUT("unable to compute digest id=%d", id);
}

static int hex_eq(const unsigned char *bytes, size_t len, const char *hex)
{
    size_t ii;
    for (ii = 0; ii < len; ++ii)
    {
        char buf[3];
        unsigned v;
        buf[0] = hex[2 * ii];
        buf[1] = hex[2 * ii + 1];
        buf[2] = '\0';
        if (sscanf(buf, "%x", &v) != 1)
            return 0;
        if (bytes[ii] != (unsigned char)v)
            return 0;
    }
    return 1;
}

int main(void)
{
    unsigned char out[64];
    unsigned char buffer[32];
    size_t ii;

    plan(16);

    /* rnx_digest_length: supported ids. */
    ok(rnx_digest_length(0) == 0, "length id=0 is 0");
    ok(rnx_digest_length(2) == 4, "length id=2 (CRC32C) is 4");
    ok(rnx_digest_length(4) == 16, "length id=4 (BLAKE3-128) is 16");
    ok(rnx_digest_length(5) == 32, "length id=5 (BLAKE3-256) is 32");
    ok(rnx_digest_length(1) < 0, "length unknown id=1 is negative");

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

    /* BLAKE3 test vectors. */
    digest_of(5, "", 0, out);
    ok(hex_eq(out, 32,
        "af1349b9f5f9a1a6a0404dea36dcc949"
        "9bcb25c9adc112b7cc9a93cae41f3262"),
        "BLAKE3-256 of empty string");
    digest_of(5, "abc", 3, out);
    ok(hex_eq(out, 32,
        "6437b3ac38465133ffb63b75273a8db5"
        "48c558465d79db03fd359c6cd5bd9d85"),
        "BLAKE3-256 of \"abc\"");
    digest_of(4, "abc", 3, out);
    ok(hex_eq(out, 16, "6437b3ac38465133ffb63b75273a8db5"),
        "BLAKE3-128 of \"abc\"");

    /* Null digest writes no bytes; a canary should remain untouched. */
    {
        unsigned char buf[4] = { 0xAA, 0xBB, 0xCC, 0xDD };

        ok(rnx_digest(0, "whatever", 8, buf) == 0,
            "rnx_digest id=0 returns 0");
        ok(buf[0] == 0xAA && buf[1] == 0xBB
            && buf[2] == 0xCC && buf[3] == 0xDD,
            "null digest does not write output");
    }

    done_testing();
}
