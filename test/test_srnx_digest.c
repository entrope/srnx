/** test_srnx_digest.c - Round-trip tests for SRNX per-chunk and file-level
 *   digests.
 * Copyright 2026 Michael Poole.
 * SPDX-License-Identifier: MIT-Modern-Variant
 *
 * The test shells out to rnx2srnx to convert a RINEX fixture using
 * various digest identifiers, then opens the resulting SRNX file to
 * confirm that both per-chunk verification (inside srnx_open) and
 * srnx_verify_file_digest() succeed.  A negative case bit-flips the
 * file's final byte and confirms that the file-level digest catches it.
 */

#include "rinex/srnx.h"
#include "rinex/digest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <tap.h>

#ifndef RNX2SRNX_PATH
# define RNX2SRNX_PATH "./rnx2srnx"
#endif
#ifndef FIXTURE_PATH
# define FIXTURE_PATH "../test/test_02.rnx"
#endif

/* Reader struct is opaque in the public header; srnx_open returns a
 * pointer to a heap-allocated instance.  The test does not need to
 * iterate chunks — it just needs to open/verify — but we still need to
 * release the mapping and the struct.  Do it via the library.
 */

static int run_rnx2srnx(int chunk_id, int file_id, const char *out)
{
    char cmd[1024];
    int res;

    snprintf(cmd, sizeof cmd,
        "%s --chunk-digest=%d --file-digest=%d %s %s",
        RNX2SRNX_PATH, chunk_id, file_id, FIXTURE_PATH, out);
    res = system(cmd);
    return res;
}

/* srnx.h exposes srnx_open but no symmetric close.  The internals
 * munmap(data, data_mapped) and free() the struct on shutdown.  To keep
 * the test self-contained without depending on those internals, we
 * simply leak per iteration — this is a test binary, and the OS
 * reclaims on exit.
 */

static void test_pair(int chunk_id, int file_id)
{
    char path[64];
    struct srnx_reader *rdr = NULL;
    int res;

    snprintf(path, sizeof path, "rt_c%d_f%d.srnx", chunk_id, file_id);
    res = run_rnx2srnx(chunk_id, file_id, path);
    ok(res == 0, "rnx2srnx chunk=%d file=%d", chunk_id, file_id);

    res = srnx_open(&rdr, path);
    ok(res == 0, "srnx_open chunk=%d file=%d (res=%d)",
        chunk_id, file_id, res);

    res = srnx_verify_file_digest(rdr);
    ok(res == 0, "srnx_verify_file_digest chunk=%d file=%d (res=%d)",
        chunk_id, file_id, res);

    remove(path);
}

/* Flip one byte in file at \a offset_from_end bytes before EOF.
 * Returns 0 on success.
 */
static int flip_byte(const char *path, long offset_from_end)
{
    FILE *fp;
    int ch;

    fp = fopen(path, "r+b");
    if (!fp)
        return -1;
    if (fseek(fp, -offset_from_end, SEEK_END) != 0)
    {
        fclose(fp);
        return -1;
    }
    ch = fgetc(fp);
    if (ch == EOF)
    {
        fclose(fp);
        return -1;
    }
    if (fseek(fp, -1, SEEK_CUR) != 0)
    {
        fclose(fp);
        return -1;
    }
    fputc(ch ^ 0xFF, fp);
    fclose(fp);
    return 0;
}

int main(void)
{
    struct stat st;

    if (stat(FIXTURE_PATH, &st) != 0)
        BAIL_OUT("fixture " FIXTURE_PATH " not present");

    plan(19);

    /* Round-trip for every supported chunk-digest id, with the default
     * file digest of 0 (none).  4 ids × 3 assertions = 12.
     */
    test_pair(0, 0);
    test_pair(2, 0);
    test_pair(4, 0);
    test_pair(5, 0);

    /* Round-trip a representative file-digest pair. */
    test_pair(2, 5);

    /* Negative test: bit-flip a byte in the file-level digest of a
     * CRC32C-chunk / BLAKE3-256-file file and confirm
     * srnx_verify_file_digest flags it.
     */
    {
        const char *path = "rt_c2_f5_flip.srnx";
        struct srnx_reader *rdr = NULL;
        int res;

        ok(run_rnx2srnx(2, 5, path) == 0, "rnx2srnx for flip test");

        /* Flip the very last byte (inside the BLAKE3-256 file digest). */
        ok(flip_byte(path, 1) == 0, "flip last byte of file digest");

        /* srnx_open itself may still succeed — the per-chunk digests
         * and non-digest bytes are intact.
         */
        res = srnx_open(&rdr, path);
        ok(res == 0, "srnx_open after file-digest flip still ok");

        res = srnx_verify_file_digest(rdr);
        ok(res == -11 /* SRNX_BAD_DIGEST */,
            "srnx_verify_file_digest detects corrupted file digest (res=%d)",
            res);

        remove(path);
    }

    done_testing();
}
