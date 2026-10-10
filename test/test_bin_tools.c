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
 *
 *  - rnxcmp must compare epochs and special events by timestamp when the
 *    two files start at different epochs (previously it indexed the
 *    second file's epochs, satellites and events with the first file's
 *    epoch index, so every later epoch was compared with the wrong one).
 *    Events with timestamps must match by their own time, and events
 *    without one by a neighbouring epoch, even if an epoch next to them
 *    is missing from one file.
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

    done_testing();
    return exit_status();
}
