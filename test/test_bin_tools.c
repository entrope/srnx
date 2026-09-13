/** test/test_bin_tools.c
 *
 * Regression tests for the bin/ command-line tools.
 *
 *  - rinex_n_obs must not crash on a valid RINEX v3 file (previously it
 *    mis-parsed the right-justified observation-count field in the header
 *    and stored into a negative array index, SIGBUS on real v3/v4 data)
 *    and must report the per-system observation maxima.
 *
 *  - rnx2srnx must exit non-zero and leave no output file when a
 *    conversion fails (previously it always returned success), and must
 *    exit zero and create the output file when it succeeds.
 *
 *  - srnx2rnx's format_observation() must format observation values
 *    correctly at the F14.3 boundary.  The sign position is mandatory
 *    (space for positive, '-' for negative), so a legal value has at
 *    most nine integer digits; the function right-justifies the value
 *    and places the sign at the leftmost digit.  The source is compiled
 *    in directly so its static helper can be unit-tested.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <tap.h>

/* Tool paths and fixtures, supplied by CMake. */
#ifndef RNX2SRNX_PATH
# define RNX2SRNX_PATH "rnx2srnx"
#endif
#ifndef RINEX_N_OBS_PATH
# define RINEX_N_OBS_PATH "rinex_n_obs"
#endif
#ifndef SRNX2RNX_PATH
# define SRNX2RNX_PATH "srnx2rnx"
#endif
#ifndef FIXTURE_V3_PATH
# define FIXTURE_V3_PATH "fixture_v3.rnx"
#endif
#ifndef FIXTURE_BAD_PATH
# define FIXTURE_BAD_PATH "fixture_invalid.rnx"
#endif

/* Compile in srnx2rnx.c so its static format_observation() is reachable.
 * main is renamed to avoid clashing with this test's own main().
 */
#define main srnx2rnx_main
#include "bin/srnx2rnx.c"
#undef main

/** Run "prog arg > out 2>&1" in the shell; return the system() status. */
static int run_capture(const char *prog, const char *arg, const char *out)
{
    char cmd[1024];

    snprintf(cmd, sizeof cmd, "%s %s > %s 2>&1", prog, arg, out);
    return system(cmd);
}

/** Return the size of \a path in bytes, or -1 if it does not exist. */
static long file_size(const char *path)
{
    struct stat st;

    if (stat(path, &st) != 0)
        return -1;
    return (long)st.st_size;
}

/** Read a whole file into a NUL-terminated malloc'd buffer. */
static char *slurp(const char *path)
{
    FILE *f;
    long len;
    char *buf;

    f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0)
    {
        fclose(f);
        return NULL;
    }
    len = ftell(f);
    if (len < 0)
    {
        fclose(f);
        return NULL;
    }
    rewind(f);
    buf = malloc((size_t)len + 1);
    if (!buf)
    {
        fclose(f);
        return NULL;
    }
    if (len > 0 && fread(buf, 1, (size_t)len, f) != (size_t)len)
    {
        free(buf);
        fclose(f);
        return NULL;
    }
    buf[len] = '\0';
    fclose(f);
    return buf;
}

int main(void)
{
    /* --- rinex_n_obs: no SIGBUS on valid v3, correct maxima. --- */
    {
        char out_path[256];
        char *content;
        int res;

        snprintf(out_path, sizeof out_path, "test_bin_nobs.out");
        res = run_capture(RINEX_N_OBS_PATH, FIXTURE_V3_PATH, out_path);
        ok(res == 0, "rinex_n_obs exits 0 on valid v3 (was SIGBUS)");

        content = slurp(out_path);
        ok(content != NULL, "rinex_n_obs produced readable output");
        if (content)
        {
            ok(strstr(content, "Maxima: C: 12") != NULL,
                "rinex_n_obs: C: 12");
            ok(strstr(content, "E: 20") != NULL, "rinex_n_obs: E: 20");
            ok(strstr(content, "G: 18") != NULL, "rinex_n_obs: G: 18");
            ok(strstr(content, "R: 20") != NULL, "rinex_n_obs: R: 20");
            ok(strstr(content, "S: 8") != NULL, "rinex_n_obs: S: 8");
            free(content);
        }
        remove(out_path);
    }

    /* --- rnx2srnx: exit status and output-file cleanup. --- */
    {
        char cmd[1024];
        char out_path[256];
        int res;

        /* Failure: a file that cannot be parsed must exit non-zero and
         * leave no (empty) output file behind. */
        snprintf(out_path, sizeof out_path, "test_bin_bad.srnx");
        remove(out_path);
        snprintf(cmd, sizeof cmd, "%s %s %s", RNX2SRNX_PATH,
            FIXTURE_BAD_PATH, out_path);
        res = system(cmd);
        ok(res != 0, "rnx2srnx exits non-zero on failed conversion");
        ok(file_size(out_path) == -1,
            "rnx2srnx leaves no output file on failure");
        remove(out_path);

        /* Success: a valid file must exit zero and create the output. */
        snprintf(out_path, sizeof out_path, "test_bin_good.srnx");
        remove(out_path);
        snprintf(cmd, sizeof cmd, "%s %s %s", RNX2SRNX_PATH,
            FIXTURE_V3_PATH, out_path);
        res = system(cmd);
        ok(res == 0, "rnx2srnx exits 0 on successful conversion");
        ok(file_size(out_path) > 0,
            "rnx2srnx creates a non-empty output file on success");
        remove(out_path);
    }

    /* --- srnx2rnx: format_observation() at the F14.3 boundary. --- */
    {
        char storage[64];
        char *buf = storage + 32;   /* 32 guard bytes precede buf */

        /* Legal maximum: nine integer digits, sign position a space. */
        memset(storage, 0, sizeof storage);
        format_observation(buf, 999999999999LL, ' ', ' ');
        ok(memcmp(buf, " 999999999.999  ", 16) == 0,
            "format_observation: F14.3 max ' 999999999.999'");

        /* Normal value with an LLI indicator. */
        memset(storage, 0, sizeof storage);
        format_observation(buf, 28541439844LL, '5', ' ');
        ok(memcmp(buf, "  28541439.8445 ", 16) == 0,
            "format_observation: normal value with LLI");

        /* Negative value: sign at the leftmost digit, rest right-justified. */
        memset(storage, 0, sizeof storage);
        format_observation(buf, -1567275LL, ' ', ' ');
        ok(memcmp(buf, "     -1567.275  ", 16) == 0,
            "format_observation: negative sign placement");
    }

    /* --- Corrupted input: an out-of-range observation must reach
     *     format_observation's abort().
     *
     * A ten-digit integer value (9999999999.999) exceeds the F14.3 range
     * (max 999999999.999).  The lenient text parser still accepts and
     * stores it, so rnx2srnx succeeds; decoding it with srnx2rnx must
     * trip format_observation()'s range abort(). */
    {
        const char *corrupt_rnx[] = {
            "     3.04           OBSERVATION DATA    G                   RINEX VERSION / TYPE\n",
            "test                                                        PGM / RUN BY / DATE\n",
            "G    1 C1C                                                  SYS / # / OBS TYPES\n",
            "                                                            END OF HEADER\n",
            "> 2020 07 07 00 00  0.0000000  0  1\n",
            /* Ten-digit integer part: out of the F14.3 range. */
            "G01" "9999999999.999" "  \n",
        };
        char cmd[1024];
        char rnx_path[256], srnx_path[256], out_path[256];
        FILE *f;
        int res, ii, aborted;

        snprintf(rnx_path, sizeof rnx_path, "test_bin_corrupt.rnx");
        f = fopen(rnx_path, "w");
        ok(f != NULL, "corrupted RINEX temp file created");
        if (f)
        {
            for (ii = 0; ii < (int)(sizeof corrupt_rnx / sizeof corrupt_rnx[0]); ++ii)
                fputs(corrupt_rnx[ii], f);
            fclose(f);
        }

        /* The lenient parser accepts the out-of-range value into the SRNX. */
        snprintf(srnx_path, sizeof srnx_path, "test_bin_corrupt.srnx");
        remove(srnx_path);
        snprintf(cmd, sizeof cmd, "%s %s %s >/dev/null 2>&1",
            RNX2SRNX_PATH, rnx_path, srnx_path);
        res = system(cmd);
        ok(res == 0, "rnx2srnx accepts out-of-range value (lenient parser)");

        /* Decoding must abort() in format_observation(). */
        snprintf(out_path, sizeof out_path, "test_bin_corrupt_out.rnx");
        snprintf(cmd, sizeof cmd, "%s %s %s >/dev/null 2>&1",
            SRNX2RNX_PATH, srnx_path, out_path);
        res = system(cmd);
        aborted = (WIFSIGNALED(res) && WTERMSIG(res) == SIGABRT)
            || (WIFEXITED(res) && WEXITSTATUS(res) == 128 + SIGABRT);
        ok(aborted, "srnx2rnx aborts on out-of-range observation");

        remove(rnx_path);
        remove(srnx_path);
        remove(out_path);
    }

    done_testing();
    return exit_status();
}
