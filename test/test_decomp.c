/** test/test_decomp.c - Tests for reading compressed RINEX files.
 * Copyright 2026 Michael Poole.
 * SPDX-License-Identifier: MIT-Modern-Variant
 *
 * rinex_open() recognizes gzip, compress, bzip2, bzip3, zstd and xz
 * files by their signatures and reads them through zcat, bzcat, etc.
 * For each compressor installed here, this checks that:
 *  - The decompression filter stream returns exactly the original bytes
 *    for an input big enough to fill the pipes in both directions, for
 *    several request sizes.
 *  - Parsing a compressed RINEX or CRX file gives the same header and
 *    records as parsing the original.
 * With gzip, it also checks that CR LF line endings are still handled,
 * that compressed data in a memory buffer works, that a truncated file
 * or a missing decompressor is reported, and that destroying a parser
 * before the end of its file does not hang.  Finally, it checks that a
 * compressed SRNX file parses like the original, and that the streams
 * rinex_open() creates are freed and their children reaped, both when
 * a parser owns them and when opening fails.
 */

#include "rinex/rnx_priv.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <tap.h>

#if defined(__has_feature)
# if __has_feature(address_sanitizer)
#  define __SANITIZE_ADDRESS__ 1
# endif
#endif

/* ASan quarantines freed memory, so the system allocator still counts
 * it as in use; ask ASan instead.
 */
#if defined(__SANITIZE_ADDRESS__)
# include <sanitizer/allocator_interface.h>
#elif defined(__APPLE__)
# include <malloc/malloc.h>
#elif defined(__GLIBC__) && (__GLIBC__ > 2 || __GLIBC_MINOR__ >= 33)
# include <malloc.h>
# define HAVE_MALLINFO2 1
#endif

#ifndef FIXTURE_PATH
# define FIXTURE_PATH "fixture_v3.rnx"
#endif
#ifndef RNX2SRNX_PATH
# define RNX2SRNX_PATH "./rnx2srnx"
#endif

/* The start of a CRINEX 3 file, as abmf_crx[] and abmf_crx_len. */
#include "bin/abmf00glp_crx.h"

static const char big_path[] = "test_decomp_big.txt";
static const char crx_path[] = "test_decomp.crx";

/** compressors lists how to compress files and which program
 * rinex_open() should use to decompress them.
 */
static const struct
{
    const char *suffix;
    const char *compress;
    const char *decompress;
} compressors[] = {
    { "gz", "gzip -c", "zcat" },
    { "Z", "compress -c", "zcat" },
    { "bz2", "bzip2 -c", "bzcat" },
    { "bz3", "bzip3 -c", "bz3cat" },
    { "zst", "zstd -q -c", "zstdcat" },
    { "xz", "xz -c", "xzcat" },
};

/** Check whether \a program can be found in $PATH. */
static int have_program(const char *program)
{
    char cmd[256];

    snprintf(cmd, sizeof cmd, "command -v %s >/dev/null 2>&1", program);
    return system(cmd) == 0;
}

/** Return the number of bytes of heap in use, or -1 if unknown. */
static long heap_in_use(void)
{
#if defined(__SANITIZE_ADDRESS__)
    return (long)__sanitizer_get_current_allocated_bytes();
#elif defined(__APPLE__)
    malloc_statistics_t stats;

    malloc_zone_statistics(NULL, &stats);
    return (long)stats.size_in_use;
#elif defined(HAVE_MALLINFO2)
    struct mallinfo2 info = mallinfo2();

    return (long)(info.uordblks + info.hblkhd);
#else
    return -1;
#endif
}

/** Check that every child process has been reaped. */
static int no_children(void)
{
    return waitpid(-1, NULL, WNOHANG) < 0 && errno == ECHILD;
}

/** Run \a compress on \a in_path, writing \a out_path. */
static int compress_file(const char *compress, const char *in_path,
    const char *out_path)
{
    char cmd[1024];

    snprintf(cmd, sizeof cmd, "%s < '%s' > '%s'", compress, in_path, out_path);
    return system(cmd);
}

/** Read the whole file at \a path, with RINEX_EXTRA bytes of zero
 * padding after it.
 */
static char *read_file(const char *path, unsigned int *p_len)
{
    FILE *f;
    char *data;
    long len;

    f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    rewind(f);
    data = calloc(1, len + RINEX_EXTRA);
    if (data && fread(data, 1, len, f) != (size_t)len)
    {
        free(data);
        data = NULL;
    }
    fclose(f);
    *p_len = len;
    return data;
}

/** Write \a len bytes of \a data to \a path. */
static int write_file(const char *path, const char *data, unsigned int len)
{
    FILE *f;
    int res;

    f = fopen(path, "wb");
    if (!f)
        return -1;
    res = fwrite(data, 1, len, f) == len ? 0 : -1;
    if (fclose(f))
        res = -1;
    return res;
}

/** Write a few megabytes of pseudo-random text to #big_path: big enough,
 * and compressing poorly enough, to fill the pipes to and from the
 * decompressor.
 */
static int write_big_file(void)
{
    unsigned int len = 3 << 20, ii, state = 12345;
    char *data;
    int res;

    data = malloc(len);
    if (!data)
        return -1;
    for (ii = 0; ii < len; ++ii)
    {
        state = state * 1103515245 + 12345;
        data[ii] = (ii % 81 == 80) ? '\n' : ' ' + (state >> 16) % 95;
    }
    res = write_file(big_path, data, len);
    free(data);
    return res;
}

/** Read everything from \a stream, \a req bytes at a time. */
static char *drain(struct rinex_stream *stream, unsigned int req,
    unsigned int *p_len, int *p_err)
{
    unsigned int len = 0, alloc = 0;
    char *out = NULL, *new_out;

    *p_err = stream->advance(stream, req, 0);
    while (!*p_err && stream->size > 0)
    {
        if (len + stream->size > alloc)
        {
            alloc = 2 * (len + stream->size);
            new_out = realloc(out, alloc);
            if (!new_out)
            {
                free(out);
                return NULL;
            }
            out = new_out;
        }
        memcpy(out + len, stream->buffer, stream->size);
        len += stream->size;
        *p_err = stream->advance(stream, req, stream->size);
    }

    *p_len = len;
    return out;
}

/** Check the decompression filter on \a path (compressed #big_path). */
static void test_filter(const char *path, const char *program)
{
    static const unsigned int reqs[] = { 80, 65536, BLOCK_SIZE };
    struct rinex_stream *inner, *stream;
    unsigned int exp_len, out_len, ii;
    const char *err;
    char *expected, *out;
    int n_bad = 0, res;

    expected = read_file(big_path, &exp_len);
    for (ii = 0; ii < sizeof reqs / sizeof reqs[0]; ++ii)
    {
        inner = rinex_mmap_stream(path);
        if (!inner || inner->advance(inner, BLOCK_SIZE, 0))
        {
            BAIL_OUT("could not read %s", path);
        }
        if (ii == 0)
        {
            const char *found = rnx_decompressor(inner);

            ok(found && !strcmp(found, program), "%s: detected as %s (%s)",
                path, program, found ? found : "not compressed");
        }

        err = rnx_decomp_stream(&stream, inner, program);
        if (err)
        {
            diag("%s: %s", path, err);
            ++n_bad;
            inner->destroy(inner);
            continue;
        }
        out = drain(stream, reqs[ii], &out_len, &res);
        if (res || !out || !expected || out_len != exp_len
            || memcmp(out, expected, exp_len))
        {
            diag("%s: req %u: got %u bytes, error %d", path, reqs[ii],
                out ? out_len : 0, res);
            ++n_bad;
        }
        free(out);
        stream->destroy(stream);
        inner->destroy(inner);
    }
    ok(n_bad == 0, "%s: filter returns the original data", path);
    free(expected);
}

/** Parse \a s_test and \a s_ref in lockstep, checking that they give
 * the same header and records.  Destroys both streams.
 */
static void test_parse(const char *name, struct rinex_stream *s_test,
    struct rinex_stream *s_ref)
{
    struct rinex_parser *p_test = NULL, *p_ref = NULL;
    rinex_error_t r_test, r_ref;
    const char *err;
    int n_records = 0, n_bad = 0, n_obs, ii;

    err = rinex_open(&p_test, s_test);
    ok(err == NULL, "%s: rinex_open() succeeds (%s)", name, err ? err : "ok");
    if (rinex_open(&p_ref, s_ref))
    {
        BAIL_OUT("%s: could not open reference file", name);
    }
    if (err)
        goto out;

    ok(p_test->buffer_len == p_ref->buffer_len
        && !memcmp(p_test->buffer, p_ref->buffer, p_ref->buffer_len)
        && !memcmp(p_test->n_obs, p_ref->n_obs, sizeof p_ref->n_obs),
        "%s: header matches", name);

    do
    {
        r_test = p_test->read(p_test);
        r_ref = p_ref->read(p_ref);
        if (r_test != r_ref
            || memcmp(&p_test->epoch, &p_ref->epoch, sizeof p_ref->epoch))
        {
            diag("%s: record %d: status %d vs %d", name, n_records,
                r_test, r_ref);
            ++n_bad;
            break;
        }
        if (r_ref != RINEX_SUCCESS)
            break;
        ++n_records;

        if (p_ref->epoch.flag > '1' && p_ref->epoch.flag != '6')
        {
            n_bad += p_test->buffer_len != p_ref->buffer_len
                || memcmp(p_test->buffer, p_ref->buffer, p_ref->buffer_len);
            continue;
        }

        n_obs = 0;
        for (ii = 0; ii < p_ref->epoch.n_sats; ++ii)
        {
            n_obs += p_ref->n_obs[p_ref->sats[ii].system & 31];
        }
        n_bad += memcmp(p_test->sats, p_ref->sats,
                p_ref->epoch.n_sats * sizeof *p_ref->sats)
            || memcmp(p_test->obs, p_ref->obs, n_obs * sizeof *p_ref->obs)
            || memcmp(p_test->lli, p_ref->lli, n_obs)
            || memcmp(p_test->ssi, p_ref->ssi, n_obs);
    } while (n_bad == 0);

    ok(n_bad == 0 && n_records > 0, "%s: %d records match", name, n_records);

out:
    if (p_test)
        p_test->destroy(p_test);
    if (p_ref)
        p_ref->destroy(p_ref);
    s_test->destroy(s_test);
    s_ref->destroy(s_ref);
}

/** Test reading files compressed with gzip in unusual ways. */
static void test_gzip(void)
{
    static const char crlf_path[] = "test_decomp_crlf.rnx";
    static const char crlf_gz[] = "test_decomp_crlf.rnx.gz";
    static const char gz_path[] = "test_decomp.rnx.gz";
    static const char trunc_path[] = "test_decomp_trunc.rnx.gz";
    struct rinex_stream *stream;
    struct rinex_parser *p = NULL;
    const char *err, *old_path;
    char *text, *crlf, *gz, *path_copy;
    unsigned int len, crlf_len, gz_len, ii;
    rinex_error_t res;

    /* CR LF line endings inside a compressed file. */
    text = read_file(FIXTURE_PATH, &len);
    crlf = malloc(2 * len);
    if (!text || !crlf)
    {
        BAIL_OUT("could not read %s", FIXTURE_PATH);
    }
    for (ii = crlf_len = 0; ii < len; ++ii)
    {
        if (text[ii] == '\n')
            crlf[crlf_len++] = '\r';
        crlf[crlf_len++] = text[ii];
    }
    if (write_file(crlf_path, crlf, crlf_len)
        || compress_file("gzip -c", crlf_path, crlf_gz))
    {
        BAIL_OUT("could not write %s", crlf_gz);
    }
    test_parse("gzip with CR LF", rinex_mmap_stream(crlf_gz),
        rinex_mmap_stream(FIXTURE_PATH));
    free(crlf);
    free(text);

    /* Compressed data in a memory buffer, not a file. */
    gz = read_file(gz_path, &gz_len);
    if (!gz)
    {
        BAIL_OUT("could not read %s", gz_path);
    }
    test_parse("gzip from buffer", rinex_buffer_stream(gz, gz_len),
        rinex_mmap_stream(FIXTURE_PATH));

    /* A truncated file must give an error, not a silent EOF. */
    if (write_file(trunc_path, gz, gz_len / 2))
    {
        BAIL_OUT("could not write %s", trunc_path);
    }
    stream = rinex_mmap_stream(trunc_path);
    err = rinex_open(&p, stream);
    res = RINEX_SUCCESS;
    while (!err && res == RINEX_SUCCESS)
    {
        res = p->read(p);
    }
    ok(err || res < RINEX_EOF, "truncated gzip file is an error (%s)",
        err ? err : p->errmsg);
    if (p)
    {
        p->destroy(p);
        p = NULL;
    }
    stream->destroy(stream);
    free(gz);

    /* Stopping before the end of the file. */
    stream = rinex_mmap_stream(gz_path);
    err = rinex_open(&p, stream);
    ok(!err && p->read(p) == RINEX_SUCCESS, "read first record of %s",
        gz_path);
    if (p)
    {
        p->destroy(p);
        p = NULL;
    }
    stream->destroy(stream);
    pass("destroying a parser early does not hang");

    /* No decompression program. */
    old_path = getenv("PATH");
    path_copy = old_path ? strdup(old_path) : NULL;
    setenv("PATH", "/nonexistent", 1);
    stream = rinex_mmap_stream(gz_path);
    err = rinex_open(&p, stream);
    ok(err && strstr(err, "zcat") && !p,
        "missing decompressor is reported (%s)", err ? err : "no error");
    stream->destroy(stream);
    if (path_copy)
    {
        setenv("PATH", path_copy, 1);
        free(path_copy);
    }
}

/** Open \a path with rinex_open(), read up to \a n_reads records, then
 * destroy the parser and stream.  Returns rinex_open()'s result.
 */
static const char *open_read_close(const char *path, int n_reads)
{
    struct rinex_stream *stream;
    struct rinex_parser *p = NULL;
    const char *err;

    stream = rinex_mmap_stream(path);
    if (!stream)
    {
        BAIL_OUT("could not open %s", path);
    }
    err = rinex_open(&p, stream);
    while (!err && n_reads-- > 0 && p->read(p) == RINEX_SUCCESS)
    {
        continue;
    }
    if (p)
    {
        p->destroy(p);
    }
    stream->destroy(stream);
    return err;
}

/** Test reading compressed SRNX files, and that rinex_open()'s streams
 * are cleaned up however the parser ends.
 */
static void test_srnx(void)
{
    static const char srnx_path[] = "test_decomp.srnx";
    static const char srnx_gz[] = "test_decomp.srnx.gz";
    static const char trunc_path[] = "test_decomp_trunc.srnx";
    static const char trunc_gz[] = "test_decomp_trunc.srnx.gz";
    static const char junk_path[] = "test_decomp_junk.txt";
    static const char junk_gz[] = "test_decomp_junk.txt.gz";
    static const char junk[] = "This is not a RINEX file.\n";
    static const char *const paths[] = {
        srnx_gz, trunc_gz, junk_gz, "test_decomp.rnx.gz",
        "test_decomp_crlf.rnx.gz"
    };
    char cmd[1024], *srnx;
    const char *err;
    unsigned int len, ii, jj;
    long before, after;

    snprintf(cmd, sizeof cmd, "%s '%s' '%s'", RNX2SRNX_PATH, FIXTURE_PATH,
        srnx_path);
    if (system(cmd) || compress_file("gzip -c", srnx_path, srnx_gz))
    {
        BAIL_OUT("could not write %s", srnx_gz);
    }
    srnx = read_file(srnx_path, &len);
    if (!srnx || write_file(trunc_path, srnx, len / 2)
        || compress_file("gzip -c", trunc_path, trunc_gz)
        || write_file(junk_path, junk, sizeof junk - 1)
        || compress_file("gzip -c", junk_path, junk_gz))
    {
        BAIL_OUT("could not write test files");
    }
    free(srnx);

    /* The SRNX parser owns the decompression stream. */
    test_parse("gzip SRNX", rinex_mmap_stream(srnx_gz),
        rinex_mmap_stream(srnx_path));

    /* The SRNX parser is created, so owns the stream, but fails. */
    err = open_read_close(trunc_gz, 0);
    ok(err != NULL, "truncated SRNX inside gzip is an error (%s)",
        err ? err : "no error");

    /* No parser is created, so rinex_open() must free the stream. */
    err = open_read_close(junk_gz, 0);
    ok(err && strstr(err, "Unrecognized"),
        "unrecognized data inside gzip is an error (%s)",
        err ? err : "no error");

    ok(no_children(), "every decompressor has been reaped");

    /* Each leaked stream would hold a buffer of at least BLOCK_SIZE.
     * The first round lets one-time allocations happen.
     */
    if (heap_in_use() < 0)
    {
        tap_skip(1, "no heap statistics on this platform");
        return;
    }
    before = 0;
    for (ii = 0; ii < 5; ++ii)
    {
        if (ii == 1)
            before = heap_in_use();
        for (jj = 0; jj < sizeof paths / sizeof paths[0]; ++jj)
        {
            open_read_close(paths[jj], 1);
        }
    }
    after = heap_in_use();
    ok(after - before < BLOCK_SIZE, "filter streams are freed (heap grew"
        " by %ld bytes over 4 rounds)", after - before);
    ok(no_children(), "every decompressor has been reaped after early"
        " destroys");
}

int main(void)
{
    const char *samples[2] = { FIXTURE_PATH, crx_path };
    char path[64], name[sizeof path + 16];
    unsigned int ii, jj;
    int n_tested = 0;

    plan(NO_PLAN);

    if (write_big_file())
    {
        BAIL_OUT("could not write %s", big_path);
    }
    if (write_file(crx_path, (const char *)abmf_crx, abmf_crx_len))
    {
        BAIL_OUT("could not write %s", crx_path);
    }

    for (ii = 0; ii < sizeof compressors / sizeof compressors[0]; ++ii)
    {
        char compress_prog[32];

        sscanf(compressors[ii].compress, "%31s", compress_prog);
        if (!have_program(compress_prog)
            || !have_program(compressors[ii].decompress))
        {
            tap_skip(1, "%s or %s not installed", compress_prog,
                compressors[ii].decompress);
            continue;
        }

        snprintf(path, sizeof path, "%s.%s", big_path, compressors[ii].suffix);
        if (compress_file(compressors[ii].compress, big_path, path))
        {
            BAIL_OUT("could not write %s", path);
        }
        test_filter(path, compressors[ii].decompress);

        for (jj = 0; jj < sizeof samples / sizeof samples[0]; ++jj)
        {
            snprintf(path, sizeof path, "test_decomp.%s.%s",
                jj ? "crx" : "rnx", compressors[ii].suffix);
            if (compress_file(compressors[ii].compress, samples[jj], path))
            {
                BAIL_OUT("could not write %s", path);
            }
            snprintf(name, sizeof name, "%s (stdio)", path);
            test_parse(name, rinex_stdio_stream(path),
                rinex_mmap_stream(samples[jj]));
            snprintf(name, sizeof name, "%s (mmap)", path);
            test_parse(name, rinex_mmap_stream(path),
                rinex_mmap_stream(samples[jj]));
        }
        ++n_tested;
    }

    if (have_program("gzip") && have_program("zcat"))
    {
        test_gzip();
        test_srnx();
    }
    else
    {
        tap_skip(1, "gzip not installed");
    }

    ok(n_tested > 0, "tested %d compression formats", n_tested);

    done_testing();
}
