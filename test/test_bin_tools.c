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
 *  - rnx2srnx must re-encode older SRNX input (in place by default,
 *    keeping the file's permissions), but leave SRNX input that is
 *    already the version it would write, or newer (even a version it
 *    cannot decode), alone with exit status 2, unless given --recompress.
 *
 *  - srnx2rnx's format_observation() must format observation values
 *    correctly at the F14.3 boundary.  The sign position is mandatory
 *    (space for positive, '-' for negative), so a legal value has at
 *    most nine integer digits; the function right-justifies the value
 *    and places the sign at the leftmost digit.  The source is compiled
 *    in directly so its static helper can be unit-tested.
 *
 *  - rnxcmp must compare epochs and special events by timestamp when the
 *    two files start at different epochs (previously it indexed the
 *    second file's epochs, satellites and events with the first file's
 *    epoch index, so every later epoch was compared with the wrong one).
 *    Events with timestamps must match by their own time, and events
 *    without one by a neighbouring epoch, even if an epoch next to them
 *    is missing from one file.
 *
 *  - rnxcmp must compare observations by code, not by position, so files
 *    that list the same codes in a different order compare equal, and a
 *    code that only one file lists is compared as missing in the other.
 *    That includes flag-1 and flag-6 records, which the loader keeps as
 *    event text rather than in the observation columns.
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
#ifndef RNXCMP_PATH
# define RNXCMP_PATH "rnxcmp"
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

/** Put the directory of the rnxcmp just built at the front of $PATH, so
 * "rnx2srnx --check" (which runs rnxcmp by name) uses it rather than an
 * installed copy or none at all.
 */
static void prepend_rnxcmp_dir(void)
{
    const char *slash = strrchr(RNXCMP_PATH, '/');
    const char *old_path = getenv("PATH");
    char *new_path;
    size_t dir_len, len;

    if (!slash)
        return;
    dir_len = slash - RNXCMP_PATH;
    len = dir_len + 2 + (old_path ? strlen(old_path) : 0);
    new_path = malloc(len);
    if (!new_path)
        BAIL_OUT("out of memory");
    snprintf(new_path, len, "%.*s%s%s", (int)dir_len, RNXCMP_PATH,
        old_path ? ":" : "", old_path ? old_path : "");
    if (setenv("PATH", new_path, 1))
        BAIL_OUT("unable to set PATH");
    free(new_path);
}

/** Write the NULL-terminated list of \a lines to \a path. */
static void write_lines(const char *path, const char *const *lines)
{
    FILE *f = fopen(path, "w");

    if (!f)
        BAIL_OUT("unable to create %s", path);
    for (; *lines; ++lines)
        fputs(*lines, f);
    fclose(f);
}

/** Count the lines of \a text that contain \a needle. */
static int count_lines_with(const char *text, const char *needle)
{
    const char *line, *eol;
    int count = 0;
    size_t len = strlen(needle);

    for (line = text; *line; line = eol + 1)
    {
        eol = strchr(line, '\n');
        if (!eol)
            eol = line + strlen(line) - 1;
        for (const char *pos = line; pos + len <= eol + 1; ++pos)
        {
            if (!memcmp(pos, needle, len))
            {
                ++count;
                break;
            }
        }
    }
    return count;
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
    prepend_rnxcmp_dir();

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

    /* --- rnx2srnx: SRNX input. --- */
    {
        static const char v12_header[] = "SRNX\x10\x01\x02";
        char cmd[1024];
        struct stat st;
        long old_size;
        FILE *f;
        int res;

        remove("test_bin_v10.srnx");
        remove("test_bin_v11.srnx");
        snprintf(cmd, sizeof cmd, "%s --socd %s test_bin_v10.srnx",
            RNX2SRNX_PATH, FIXTURE_V3_PATH);
        res = system(cmd);
        ok(res == 0, "rnx2srnx --socd writes SRNX 1.0");

        /* Already current: exit status 2, file untouched. */
        old_size = file_size("test_bin_v10.srnx");
        snprintf(cmd, sizeof cmd, "%s --socd test_bin_v10.srnx"
            " 2>/dev/null", RNX2SRNX_PATH);
        res = system(cmd);
        ok(WIFEXITED(res) && WEXITSTATUS(res) == 2,
            "rnx2srnx exits 2 for SRNX input of the version it writes");
        ok(file_size("test_bin_v10.srnx") == old_size,
            "rnx2srnx leaves an already-current SRNX input alone");

        /* Older version: re-encoded to another name, then in place. */
        snprintf(cmd, sizeof cmd, "%s --check test_bin_v10.srnx"
            " test_bin_v11.srnx", RNX2SRNX_PATH);
        res = system(cmd);
        ok(res == 0 && file_size("test_bin_v11.srnx") > 0,
            "rnx2srnx re-encodes SRNX 1.0 input to a named output");
        chmod("test_bin_v10.srnx", 0600);
        snprintf(cmd, sizeof cmd, "%s --check test_bin_v10.srnx",
            RNX2SRNX_PATH);
        res = system(cmd);
        ok(res == 0, "rnx2srnx re-encodes SRNX 1.0 input in place");
        ok(file_size("test_bin_v10.srnx") == file_size("test_bin_v11.srnx"),
            "rnx2srnx replaced the SRNX 1.0 input with SRNX 1.1");
        ok(stat("test_bin_v10.srnx", &st) == 0
            && (st.st_mode & 07777) == 0600,
            "rnx2srnx keeps the permissions of a replaced input");
        res = system("ls test_bin_v10.srnx.* >/dev/null 2>&1");
        ok(res != 0, "rnx2srnx leaves no temporary file behind");
        snprintf(cmd, sizeof cmd, "%s test_bin_v10.srnx 2>/dev/null",
            RNX2SRNX_PATH);
        res = system(cmd);
        ok(WIFEXITED(res) && WEXITSTATUS(res) == 2,
            "rnx2srnx exits 2 for the re-encoded file");

        /* Skipping creates no temporary file, so works read-only. */
        mkdir("test_bin_ro", 0755);
        snprintf(cmd, sizeof cmd, "cp test_bin_v10.srnx test_bin_ro/"
            " && chmod 555 test_bin_ro && %s test_bin_ro/test_bin_v10.srnx"
            " 2>/dev/null", RNX2SRNX_PATH);
        res = system(cmd);
        ok(geteuid() == 0 || (WIFEXITED(res) && WEXITSTATUS(res) == 2),
            "rnx2srnx exits 2 for current SRNX in a read-only directory");
        chmod("test_bin_ro", 0755);
        remove("test_bin_ro/test_bin_v10.srnx");
        rmdir("test_bin_ro");

        /* Piped input is recognized as SRNX too. */
        snprintf(cmd, sizeof cmd, "%s - test_bin_v11b.srnx"
            " < test_bin_v11.srnx 2>/dev/null", RNX2SRNX_PATH);
        res = system(cmd);
        ok(WIFEXITED(res) && WEXITSTATUS(res) == 2
            && file_size("test_bin_v11b.srnx") == -1,
            "rnx2srnx exits 2 for current SRNX on stdin");

        /* --recompress overrides the check; "--" ends the options. */
        snprintf(cmd, sizeof cmd, "%s --recompress --check --"
            " test_bin_v10.srnx", RNX2SRNX_PATH);
        res = system(cmd);
        ok(res == 0, "rnx2srnx --recompress re-encodes current SRNX");
        ok(file_size("test_bin_v10.srnx") == file_size("test_bin_v11.srnx"),
            "rnx2srnx --recompress keeps the same encoding");

        /* A newer version than the library decodes is still current. */
        f = fopen("test_bin_v12.srnx", "wb");
        if (f)
        {
            fwrite(v12_header, 1, sizeof v12_header - 1, f);
            fclose(f);
        }
        snprintf(cmd, sizeof cmd, "%s test_bin_v12.srnx 2>/dev/null",
            RNX2SRNX_PATH);
        res = system(cmd);
        ok(WIFEXITED(res) && WEXITSTATUS(res) == 2,
            "rnx2srnx exits 2 for SRNX newer than it decodes");
        snprintf(cmd, sizeof cmd, "%s - test_bin_v12b.srnx"
            " < test_bin_v12.srnx 2>/dev/null", RNX2SRNX_PATH);
        res = system(cmd);
        ok(WIFEXITED(res) && WEXITSTATUS(res) == 2,
            "rnx2srnx exits 2 for newer SRNX on stdin");
        snprintf(cmd, sizeof cmd, "%s --recompress test_bin_v12.srnx"
            " 2>/dev/null", RNX2SRNX_PATH);
        res = system(cmd);
        ok(WIFEXITED(res) && WEXITSTATUS(res) == 1
            && file_size("test_bin_v12.srnx") == sizeof v12_header - 1,
            "rnx2srnx --recompress fails on newer SRNX, keeping the input");

        remove("test_bin_v10.srnx");
        remove("test_bin_v11.srnx");
        remove("test_bin_v12.srnx");
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
     * (max 999999999.999).  The text parser rejects it, but the CRX
     * parser does not range-check values, so rnx2srnx accepts it from
     * CRX input; decoding that with srnx2rnx must trip
     * format_observation()'s range abort(). */
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
        const char *corrupt_crx[] = {
            "3.0                 COMPACT RINEX FORMAT                    CRINEX VERS   / TYPE\n",
            "RNX2CRX ver.4.2.0                       10-Oct-26 01:10     CRINEX PROG / DATE\n",
            "     3.04           OBSERVATION DATA    G                   RINEX VERSION / TYPE\n",
            "test                                                        PGM / RUN BY / DATE\n",
            "G    1 C1C                                                  SYS / # / OBS TYPES\n",
            "                                                            END OF HEADER\n",
            "> 2020 07 07 00 00  0.0000000  0  1      G01\n",
            "\n",
            /* The same value, which CRX writes without a decimal point. */
            "3&9999999999999\n",
        };
        char cmd[1024];
        char rnx_path[256], crx_path[256], srnx_path[256], out_path[256];
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

        /* The text parser rejects the out-of-range value. */
        snprintf(srnx_path, sizeof srnx_path, "test_bin_corrupt.srnx");
        remove(srnx_path);
        snprintf(cmd, sizeof cmd, "%s %s %s >/dev/null 2>&1",
            RNX2SRNX_PATH, rnx_path, srnx_path);
        res = system(cmd);
        ok(res != 0, "rnx2srnx rejects out-of-range RINEX value");

        snprintf(crx_path, sizeof crx_path, "test_bin_corrupt.crx");
        f = fopen(crx_path, "w");
        ok(f != NULL, "corrupted CRX temp file created");
        if (f)
        {
            for (ii = 0; ii < (int)(sizeof corrupt_crx / sizeof corrupt_crx[0]); ++ii)
                fputs(corrupt_crx[ii], f);
            fclose(f);
        }

        /* The CRX parser accepts it into the SRNX. */
        remove(srnx_path);
        snprintf(cmd, sizeof cmd, "%s %s %s >/dev/null 2>&1",
            RNX2SRNX_PATH, crx_path, srnx_path);
        res = system(cmd);
        ok(res == 0, "rnx2srnx accepts out-of-range CRX value");

        /* Decoding must abort() in format_observation(). */
        snprintf(out_path, sizeof out_path, "test_bin_corrupt_out.rnx");
        snprintf(cmd, sizeof cmd, "%s %s %s >/dev/null 2>&1",
            SRNX2RNX_PATH, srnx_path, out_path);
        res = system(cmd);
        aborted = (WIFSIGNALED(res) && WTERMSIG(res) == SIGABRT)
            || (WIFEXITED(res) && WEXITSTATUS(res) == 128 + SIGABRT);
        ok(aborted, "srnx2rnx aborts on out-of-range observation");

        remove(rnx_path);
        remove(crx_path);
        remove(srnx_path);
        remove(out_path);
    }

    /* --- rnxcmp: files that start at different epochs. --- */
    {
#define CMP_HEADER \
    "     3.04           OBSERVATION DATA    G                   RINEX VERSION / TYPE\n", \
    "test                                                        PGM / RUN BY / DATE\n", \
    "G    1 C1C                                                  SYS / # / OBS TYPES\n", \
    "                                                            END OF HEADER\n"
#define CMP_EPOCH_0 \
    "> 2020 07 07 00 00  0.0000000  0  1\n", "G01  20000000.000  \n"
#define CMP_EVENT \
    "> 2020 07 07 00 00 30.0000000  4  1\n", \
    "an event before the second epoch                            COMMENT\n"
        static const char *const cmp_a[] = { CMP_HEADER, CMP_EPOCH_0,
            CMP_EVENT,
            "> 2020 07 07 00 00 30.0000000  0  1\n", "G01  20000030.000  \n",
            "> 2020 07 07 00 01  0.0000000  0  1\n", "G01  20000060.000  \n",
            NULL };
        static const char *const cmp_b[] = { CMP_HEADER, CMP_EVENT,
            "> 2020 07 07 00 00 30.0000000  0  1\n", "G01  20000030.000  \n",
            "> 2020 07 07 00 01  0.0000000  0  1\n", "G01  20000060.000  \n",
            NULL };
        static const char *const cmp_c[] = { CMP_HEADER, CMP_EVENT,
            "> 2020 07 07 00 00 30.0000000  0  1\n", "G01  20000031.000  \n",
            "> 2020 07 07 00 01  0.0000000  0  1\n", "G01  20000060.000  \n",
            NULL };
#undef CMP_HEADER
#undef CMP_EPOCH_0
#undef CMP_EVENT
        const char *out_path = "test_bin_cmp_out.txt";
        char *output;
        int res;

        write_lines("test_bin_cmp_a.rnx", cmp_a);
        write_lines("test_bin_cmp_b.rnx", cmp_b);
        write_lines("test_bin_cmp_c.rnx", cmp_c);

        /* B is A without its first epoch: only that epoch differs. */
        res = run_capture(RNXCMP_PATH,
            "--full test_bin_cmp_a.rnx test_bin_cmp_b.rnx", out_path);
        output = slurp(out_path);
        ok(res != 0 && output
            && count_lines_with(output, "") == 1
            && count_lines_with(output, "0000 0.0000000 only in file1") == 1,
            "rnxcmp: offset start reports only the missing epoch");
        free(output);

        res = run_capture(RNXCMP_PATH,
            "--full test_bin_cmp_b.rnx test_bin_cmp_a.rnx", out_path);
        output = slurp(out_path);
        ok(res != 0 && output
            && count_lines_with(output, "") == 1
            && count_lines_with(output, "0000 0.0000000 only in file2") == 1,
            "rnxcmp: offset start, files swapped");
        free(output);

        /* C also changes the value at 00:00:30: that must be reported
         * at the right epoch, comparing the right values. */
        res = run_capture(RNXCMP_PATH,
            "--full test_bin_cmp_a.rnx test_bin_cmp_c.rnx", out_path);
        output = slurp(out_path);
        ok(res != 0 && output
            && count_lines_with(output, "obs differ") == 1
            && count_lines_with(output, "0000 30.0000000 sat G01 obs differ") == 1
            && count_lines_with(output, "20000030000(") == 1
            && count_lines_with(output, "vs 20000031000(") == 1
            && count_lines_with(output, "event") == 0
            && count_lines_with(output, "clock_offset") == 0,
            "rnxcmp: offset start still finds a real difference");
        free(output);

        remove("test_bin_cmp_a.rnx");
        remove("test_bin_cmp_b.rnx");
        remove("test_bin_cmp_c.rnx");
        remove(out_path);
    }

    /* --- rnxcmp: events next to an epoch missing from one file. --- */
    {
#define V3_HEADER \
    "     3.04           OBSERVATION DATA    G                   RINEX VERSION / TYPE\n", \
    "test                                                        PGM / RUN BY / DATE\n", \
    "G    1 C1C                                                  SYS / # / OBS TYPES\n", \
    "                                                            END OF HEADER\n"
#define V3_E0 "> 2020 07 07 00 00  0.0000000  0  1\n", "G01  20000000.000  \n"
#define V3_E30 "> 2020 07 07 00 00 30.0000000  0  1\n", "G01  20000030.000  \n"
#define V3_E60 "> 2020 07 07 00 01  0.0000000  0  1\n", "G01  20000060.000  \n"
#define V3_EV15 "> 2020 07 07 00 00 15.0000000  4  1\n", \
    "an event at 00:00:15                                        COMMENT\n"
#define V2_HEADER \
    "     2.11           OBSERVATION DATA    G (GPS)             RINEX VERSION / TYPE\n", \
    "test                                                        PGM / RUN BY / DATE\n", \
    "     1    C1                                                # / TYPES OF OBSERV\n", \
    "                                                            END OF HEADER\n"
#define V2_E0 " 20  7  7  0  0  0.0000000  0  1G01\n", "  20000000.000\n"
#define V2_E30 " 20  7  7  0  0 30.0000000  0  1G01\n", "  20000030.000\n"
#define V2_E60 " 20  7  7  0  1  0.0000000  0  1G01\n", "  20000060.000\n"
#define V2_E90 " 20  7  7  0  1 30.0000000  0  1G01\n", "  20000090.000\n"
#define V2_UEV "                            4  1\n", \
    "an untimed event                                            COMMENT\n"
#define V2_EV10 " 20  7  7  0  0 10.0000000  4  1\n", \
    "an event at 00:00:10                                        COMMENT\n"
#define V2_EV15 " 20  7  7  0  0 15.0000000  4  1\n", \
    "an event at 00:00:15                                        COMMENT\n"
        static const char *const ev_a[] = { V3_HEADER, V3_E0, V3_EV15,
            V3_E30, V3_E60, NULL };
        static const char *const ev_b[] = { V3_HEADER, V3_E0, V3_EV15,
            V3_E60, NULL };
        static const char *const ev_c[] = { V3_HEADER, V3_E0,
            "> 2020 07 07 00 00 15.0000000  4  1\n",
            "a different event at 00:00:15                               COMMENT\n",
            V3_E30, V3_E60, NULL };
        static const char *const uev_a[] = { V2_HEADER, V2_E0, V2_E30,
            V2_UEV, V2_E60, V2_E90, NULL };
        static const char *const uev_b[] = { V2_HEADER, V2_E0,
            V2_UEV, V2_E60, V2_E90, NULL };
        static const char *const uev_c[] = { V2_HEADER, V2_E0, V2_E30,
            V2_UEV, V2_E90, NULL };
        static const char *const mix_a[] = { V2_HEADER, V2_E0, V2_EV10,
            V2_UEV, V2_EV15, V2_E30, NULL };
        static const char *const mix_b[] = { V2_HEADER, V2_E0,
            V2_UEV, V2_EV15, V2_E30, NULL };
#undef V3_HEADER
#undef V3_E0
#undef V3_E30
#undef V3_E60
#undef V3_EV15
#undef V2_HEADER
#undef V2_E0
#undef V2_E30
#undef V2_E60
#undef V2_E90
#undef V2_UEV
#undef V2_EV10
#undef V2_EV15
        const char *out_path = "test_bin_cmp_out.txt";
        char *output;
        int res;

        write_lines("test_bin_ev_a.rnx", ev_a);
        write_lines("test_bin_ev_b.rnx", ev_b);
        write_lines("test_bin_ev_c.rnx", ev_c);
        write_lines("test_bin_uev_a.rnx", uev_a);
        write_lines("test_bin_uev_b.rnx", uev_b);
        write_lines("test_bin_uev_c.rnx", uev_c);
        write_lines("test_bin_mix_a.rnx", mix_a);
        write_lines("test_bin_mix_b.rnx", mix_b);

        /* The event at 00:00:15 precedes 00:00:30 in A but 00:01:00 in
         * B; it must still match by its own timestamp. */
        res = run_capture(RNXCMP_PATH,
            "--full test_bin_ev_a.rnx test_bin_ev_b.rnx", out_path);
        output = slurp(out_path);
        ok(res != 0 && output
            && count_lines_with(output, "") == 1
            && count_lines_with(output, "0000 30.0000000 only in file1") == 1,
            "rnxcmp: timed event matches across a missing epoch");
        free(output);

        res = run_capture(RNXCMP_PATH,
            "--full test_bin_ev_b.rnx test_bin_ev_a.rnx", out_path);
        output = slurp(out_path);
        ok(res != 0 && output
            && count_lines_with(output, "") == 1
            && count_lines_with(output, "0000 30.0000000 only in file2") == 1,
            "rnxcmp: timed event matches across a missing epoch, swapped");
        free(output);

        /* Different event text at the same time is still reported. */
        res = run_capture(RNXCMP_PATH,
            "--full test_bin_ev_a.rnx test_bin_ev_c.rnx", out_path);
        output = slurp(out_path);
        ok(res != 0 && output
            && count_lines_with(output, "") == 1
            && count_lines_with(output, "event at 20200707 0000 15.0000000"
                " text differs") == 1,
            "rnxcmp: timed event text difference is reported");
        free(output);

        /* An untimed event whose preceding epoch is missing in B. */
        res = run_capture(RNXCMP_PATH,
            "--full test_bin_uev_a.rnx test_bin_uev_b.rnx", out_path);
        output = slurp(out_path);
        ok(res != 0 && output
            && count_lines_with(output, "") == 1
            && count_lines_with(output, "0000 30.0000000 only in file1") == 1,
            "rnxcmp: untimed event matches by its following epoch");
        free(output);

        /* An untimed event whose following epoch is missing in C. */
        res = run_capture(RNXCMP_PATH,
            "--full test_bin_uev_a.rnx test_bin_uev_c.rnx", out_path);
        output = slurp(out_path);
        ok(res != 0 && output
            && count_lines_with(output, "") == 1
            && count_lines_with(output, "0001 0.0000000 only in file1") == 1,
            "rnxcmp: untimed event matches by its preceding epoch");
        free(output);

        remove("test_bin_ev_a.rnx");
        remove("test_bin_ev_b.rnx");
        remove("test_bin_ev_c.rnx");
        remove("test_bin_uev_a.rnx");
        remove("test_bin_uev_b.rnx");
        /* Timed and untimed events in one interval, with one timed
         * event missing from B: only that event differs. */
        res = run_capture(RNXCMP_PATH,
            "--full test_bin_mix_a.rnx test_bin_mix_b.rnx", out_path);
        output = slurp(out_path);
        ok(res != 0 && output
            && count_lines_with(output, "") == 1
            && count_lines_with(output, "event at 20200707 0000 10.0000000"
                " only in file1") == 1,
            "rnxcmp: untimed event among timed events stays matched");
        free(output);

        res = run_capture(RNXCMP_PATH,
            "--full test_bin_mix_b.rnx test_bin_mix_a.rnx", out_path);
        output = slurp(out_path);
        ok(res != 0 && output
            && count_lines_with(output, "") == 1
            && count_lines_with(output, "event at 20200707 0000 10.0000000"
                " only in file2") == 1,
            "rnxcmp: untimed event among timed events, swapped");
        free(output);

        remove("test_bin_uev_c.rnx");
        remove("test_bin_mix_a.rnx");
        remove("test_bin_mix_b.rnx");
        remove(out_path);
    }

    /* --- rnxcmp: observation codes in different orders. --- */
    {
#define ORD_V3(types) \
    "     3.04           OBSERVATION DATA    G                   RINEX VERSION / TYPE\n", \
    "test                                                        PGM / RUN BY / DATE\n", \
    types, \
    "                                                            END OF HEADER\n", \
    "> 2020 07 07 00 00  0.0000000  0  1\n"
#define ORD_V2(types) \
    "     2.11           OBSERVATION DATA    G (GPS)             RINEX VERSION / TYPE\n", \
    "test                                                        PGM / RUN BY / DATE\n", \
    types, \
    "                                                            END OF HEADER\n", \
    " 20  7  7  0  0  0.0000000  0  1G01\n"
        static const char *const ord_a[] = { ORD_V3(
            "G    2 C1C L1C                                              SYS / # / OBS TYPES\n"),
            "G01  20000000.000 7 105000000.00017\n", NULL };
        /* Same data, codes reversed. */
        static const char *const ord_b[] = { ORD_V3(
            "G    2 L1C C1C                                              SYS / # / OBS TYPES\n"),
            "G01 105000000.00017  20000000.000 7\n", NULL };
        /* Reversed, plus a code that is never observed. */
        static const char *const ord_c[] = { ORD_V3(
            "G    3 L1C S1C C1C                                          SYS / # / OBS TYPES\n"),
            "G01 105000000.00017                  20000000.000 7\n", NULL };
        /* Reversed, plus a code that is observed. */
        static const char *const ord_d[] = { ORD_V3(
            "G    3 L1C S1C C1C                                          SYS / # / OBS TYPES\n"),
            "G01 105000000.00017        45.000    20000000.000 7\n", NULL };
        /* Reversed, with a different C1C value. */
        static const char *const ord_e[] = { ORD_V3(
            "G    2 L1C C1C                                              SYS / # / OBS TYPES\n"),
            "G01 105000000.00017  20000000.001 7\n", NULL };
        static const char *const ord_v2a[] = { ORD_V2(
            "     2    C1    L1                                          # / TYPES OF OBSERV\n"),
            "  20000000.000 7 105000000.00017\n", NULL };
        static const char *const ord_v2b[] = { ORD_V2(
            "     2    L1    C1                                          # / TYPES OF OBSERV\n"),
            " 105000000.00017  20000000.000 7\n", NULL };
#undef ORD_V3
#undef ORD_V2
        const char *out_path = "test_bin_cmp_out.txt";
        char *output;
        int res;

        write_lines("test_bin_ord_a.rnx", ord_a);
        write_lines("test_bin_ord_b.rnx", ord_b);
        write_lines("test_bin_ord_c.rnx", ord_c);
        write_lines("test_bin_ord_d.rnx", ord_d);
        write_lines("test_bin_ord_e.rnx", ord_e);
        write_lines("test_bin_ord_v2a.rnx", ord_v2a);
        write_lines("test_bin_ord_v2b.rnx", ord_v2b);

        res = run_capture(RNXCMP_PATH,
            "--full test_bin_ord_a.rnx test_bin_ord_b.rnx", out_path);
        output = slurp(out_path);
        ok(res == 0 && output && !*output,
            "rnxcmp: reordered observation codes compare equal");
        free(output);

        res = run_capture(RNXCMP_PATH,
            "--full test_bin_ord_a.rnx test_bin_ord_c.rnx", out_path);
        output = slurp(out_path);
        ok(res == 0 && output && !*output,
            "rnxcmp: an extra, never-observed code compares equal");
        free(output);

        res = run_capture(RNXCMP_PATH,
            "--full test_bin_ord_a.rnx test_bin_ord_d.rnx", out_path);
        output = slurp(out_path);
        ok(res != 0 && output
            && count_lines_with(output, "") == 2
            && count_lines_with(output, "  S1C: -9223372036854775808(") == 1
            && count_lines_with(output, "vs 45000(") == 1,
            "rnxcmp: observations of a code in only one file are reported");
        free(output);

        res = run_capture(RNXCMP_PATH,
            "--full test_bin_ord_a.rnx test_bin_ord_e.rnx", out_path);
        output = slurp(out_path);
        ok(res != 0 && output
            && count_lines_with(output, "") == 2
            && count_lines_with(output, "  C1C: 20000000000(") == 1
            && count_lines_with(output, "vs 20000000001(") == 1,
            "rnxcmp: a changed value is reported under its code");
        free(output);

        res = run_capture(RNXCMP_PATH,
            "--full test_bin_ord_v2a.rnx test_bin_ord_v2b.rnx", out_path);
        output = slurp(out_path);
        ok(res == 0 && output && !*output,
            "rnxcmp: reordered RINEX 2 observation types compare equal");
        free(output);

        remove("test_bin_ord_a.rnx");
        remove("test_bin_ord_b.rnx");
        remove("test_bin_ord_c.rnx");
        remove("test_bin_ord_d.rnx");
        remove("test_bin_ord_e.rnx");
        remove("test_bin_ord_v2a.rnx");
        remove("test_bin_ord_v2b.rnx");
        remove(out_path);
    }

    /* --- rnxcmp: flag-1 and flag-6 records with reordered codes. --- */
    {
#define OEV_V3(types) \
    "     3.04           OBSERVATION DATA    G                   RINEX VERSION / TYPE\n", \
    "test                                                        PGM / RUN BY / DATE\n", \
    types, \
    "                                                            END OF HEADER\n"
#define OEV_AB "G    2 C1C L1C                                              SYS / # / OBS TYPES\n"
#define OEV_BA "G    2 L1C C1C                                              SYS / # / OBS TYPES\n"
        static const char *const oev_a[] = { OEV_V3(OEV_AB),
            "> 2020 07 07 00 00  0.0000000  0  1\n",
            "G01  20000000.000 7 105000000.00017\n",
            "> 2020 07 07 00 00 30.0000000  1  1\n",
            "G01  20000030.000 7 105000030.00017\n",
            "> 2020 07 07 00 01  0.0000000  0  1\n",
            "G01  20000060.000 7 105000060.00017\n",
            "> 2020 07 07 00 01  0.0000000  6  1\n",
            "G01  20000060.000 7 105000060.00017\n", NULL };
        /* Codes reversed, values reordered to match. */
        static const char *const oev_b[] = { OEV_V3(OEV_BA),
            "> 2020 07 07 00 00  0.0000000  0  1\n",
            "G01 105000000.00017  20000000.000 7\n",
            "> 2020 07 07 00 00 30.0000000  1  1\n",
            "G01 105000030.00017  20000030.000 7\n",
            "> 2020 07 07 00 01  0.0000000  0  1\n",
            "G01 105000060.00017  20000060.000 7\n",
            "> 2020 07 07 00 01  0.0000000  6  1\n",
            "G01 105000060.00017  20000060.000 7\n", NULL };
        /* Codes reversed, but the flag-1 and flag-6 values left in A's
         * positions, so they now belong to the wrong codes. */
        static const char *const oev_c[] = { OEV_V3(OEV_BA),
            "> 2020 07 07 00 00  0.0000000  0  1\n",
            "G01 105000000.00017  20000000.000 7\n",
            "> 2020 07 07 00 00 30.0000000  1  1\n",
            "G01  20000030.000 7 105000030.00017\n",
            "> 2020 07 07 00 01  0.0000000  0  1\n",
            "G01 105000060.00017  20000060.000 7\n",
            "> 2020 07 07 00 01  0.0000000  6  1\n",
            "G01  20000060.000 7 105000060.00017\n", NULL };
#undef OEV_V3
#undef OEV_AB
#undef OEV_BA
        const char *out_path = "test_bin_cmp_out.txt";
        char cmd[1024];
        char *output;
        int res;

        write_lines("test_bin_oev_a.rnx", oev_a);
        write_lines("test_bin_oev_b.rnx", oev_b);
        write_lines("test_bin_oev_c.rnx", oev_c);

        res = run_capture(RNXCMP_PATH,
            "--full test_bin_oev_a.rnx test_bin_oev_b.rnx", out_path);
        output = slurp(out_path);
        ok(res == 0 && output && !*output,
            "rnxcmp: reordered flag-1/flag-6 records compare equal");
        free(output);

        res = run_capture(RNXCMP_PATH,
            "--full test_bin_oev_a.rnx test_bin_oev_c.rnx", out_path);
        output = slurp(out_path);
        ok(res != 0 && output
            && count_lines_with(output, "obs differ") == 2
            && count_lines_with(output, "event at 20200707 0000 30.0000000"
                " sat G01 obs differ") == 1
            && count_lines_with(output, "event at 20200707 0001 0.0000000"
                " sat G01 obs differ") == 1
            && count_lines_with(output, "  C1C: ") == 2
            && count_lines_with(output, "  L1C: ") == 2
            && count_lines_with(output, "text differs") == 0,
            "rnxcmp: flag-1/flag-6 values under the wrong codes are reported");
        free(output);

        /* The same comparison with file1 loaded from SRNX. */
        snprintf(cmd, sizeof cmd, "%s test_bin_oev_a.rnx test_bin_oev_a.srnx"
            " >/dev/null 2>&1", RNX2SRNX_PATH);
        res = system(cmd);
        ok(res == 0, "rnx2srnx converts the flag-1/flag-6 file");
        res = run_capture(RNXCMP_PATH,
            "--full test_bin_oev_a.srnx test_bin_oev_b.rnx", out_path);
        output = slurp(out_path);
        ok(res == 0 && output && !*output,
            "rnxcmp: SRNX flag-1/flag-6 records compare by code");
        free(output);

        remove("test_bin_oev_a.rnx");
        remove("test_bin_oev_b.rnx");
        remove("test_bin_oev_c.rnx");
        remove("test_bin_oev_a.srnx");
        remove(out_path);
    }

    done_testing();
    return exit_status();
}
