/** test_zero_obs.c - Explicit zero observations versus blank ones.
 * Copyright 2026 Michael Poole.
 * SPDX-License-Identifier: MIT-Modern-Variant
 *
 * Checks that the RINEX, CRINEX and SRNX parsers report an explicit
 * 0.000 as a present zero and a blank field as INT64_MIN (keeping any
 * LLI/SSI it has), that rinex_load() and rnx2srnx keep the zeros and
 * warn about blank fields with indicators, and that SRNX round trips
 * through srnx2rnx, rnxcmp and rinex-merge keep the zeros.
 */

#include "rinex/rinex.h"
#include "rinex/rinex_load.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tap.h>

#ifndef RNX2SRNX_PATH
# define RNX2SRNX_PATH "./rnx2srnx"
#endif
#ifndef SRNX2RNX_PATH
# define SRNX2RNX_PATH "./srnx2rnx"
#endif
#ifndef RNXCMP_PATH
# define RNXCMP_PATH "./rnxcmp"
#endif
#ifndef RINEX_MERGE_PATH
# define RINEX_MERGE_PATH "./rinex-merge"
#endif

/* A geostationary satellite (C01) whose Doppler is often exactly zero,
 * with blank fields in the middle and at the end of lines, blank
 * fields with an LLI (C11 D2I at 12:00:05) or SSI (C21 S2I at
 * 12:00:06), and zeros with indicators.  Each epoch has twelve
 * fields, so for AVX2 the flagged blanks land in a full batch of eight
 * and in the leftover batch.
 */
static const char zero_rnx[] =
    "     3.04           OBSERVATION DATA    C                   RINEX VERSION / TYPE\n"
    "C    4 C2I L2I D2I S2I                                      SYS / # / OBS TYPES\n"
    "     1.000                                                  INTERVAL\n"
    "                                                            END OF HEADER\n"
    "> 2024 03 01 12 00  0.0000000  0  3\n"
    "C01  37000000.000   192000000.000 8        -0.120          42.000\n"
    "C11  21000000.000   110000000.000 7     -1500.000          45.250\n"
    "C21  23000000.000   120000000.000 6      1000.000          38.500\n"
    "> 2024 03 01 12 00  1.0000000  0  3\n"
    "C01  37000000.010   192000000.050 8         0.000          42.000\n"
    "C11  21000300.000   110001500.000 7     -1499.000          45.250\n"
    "C21  22999800.000   119999000.000 6       999.000          38.500\n"
    "> 2024 03 01 12 00  2.0000000  0  3\n"
    "C01  37000000.020   192000000.100 8         0.000          42.000\n"
    "C11  21000600.000   110003000.000 7     -1498.000          45.250\n"
    "C21  22999600.000   119998000.000 6       998.000          38.500\n"
    "> 2024 03 01 12 00  3.0000000  0  3\n"
    "C01  37000000.030   192000000.150 8                         0.000\n"
    "C11  21000900.000   110004500.000 7     -1497.000          45.250\n"
    "C21  22999400.000   119997000.000 6       997.000          38.500\n"
    "> 2024 03 01 12 00  4.0000000  0  3\n"
    "C01  37000000.040   192000000.200 8         0.000 7        42.000\n"
    "C11  21001200.000   110006000.000 7     -1496.000          45.250\n"
    "C21  22999200.000   119996000.000 6       996.000          38.500\n"
    "> 2024 03 01 12 00  5.0000000  0  3\n"
    "C01  37000000.050   192000000.250 8         0.040          42.000\n"
    "C11  21001500.000   110007500.000 7              1         45.250\n"
    "C21  22999000.000   119995000.000 6       995.000          38.500\n"
    "> 2024 03 01 12 00  6.0000000  0  3\n"
    "C01  37000000.060   192000000.300 8         0.000          42.000\n"
    "C11  21001800.000   110009000.000 7     -1494.000          45.250\n"
    "C21  22998800.000   119994000.000 6       994.000                 5\n"
    "> 2024 03 01 12 00  7.0000000  0  3\n"
    "C01  37000000.070   192000000.350 8        -0.010          42.000\n"
    "C11  21002100.000   110010500.000 7     -1493.000          45.250\n"
    "C21  22998600.000           0.000 6\n"
    "> 2024 03 01 12 00  8.0000000  0  3\n"
    "C01  37000000.080   192000000.400 8         0.000          42.000\n"
    "C11  21002400.000           0.00017     -1492.000          45.250\n"
    "C21  22998400.000   119992000.000 6       992.000          38.500\n"
    "> 2024 03 01 12 00  9.0000000  0  3\n"
    "C01  37000000.090   192000000.450 8         0.000          42.000\n"
    "C11  21002700.000   110013500.000 7     -1491.000          45.250\n"
    "C21  22998200.000   119991000.000 6       991.000          38.500\n"
    ;

/* RNX2CRX 4.2.0 output for zero_rnx. */
static const char zero_crx[] =
    "3.0                 COMPACT RINEX FORMAT                    CRINEX VERS   / TYPE\n"
    "RNX2CRX ver.4.2.0                       14-Oct-26 22:18     CRINEX PROG / DATE\n"
    "     3.04           OBSERVATION DATA    C                   RINEX VERSION / TYPE\n"
    "C    4 C2I L2I D2I S2I                                      SYS / # / OBS TYPES\n"
    "     1.000                                                  INTERVAL\n"
    "                                                            END OF HEADER\n"
    "> 2024 03 01 12 00  0.0000000  0  3      C01C11C21\n"
    "\n"
    "3&37000000000 3&192000000000 3&-120 3&42000 &&&8&&&&\n"
    "3&21000000000 3&110000000000 3&-1500000 3&45250 &&&7&&&&\n"
    "3&23000000000 3&120000000000 3&1000000 3&38500 &&&6&&&&\n"
    "                    1\n"
    "\n"
    "10 50 120 0\n"
    "300000 1500000 1000 0\n"
    "-200000 -1000000 -1000 0\n"
    "                    2\n"
    "\n"
    "0 0 -120 0\n"
    "0 0 0 0\n"
    "0 0 0 0\n"
    "                    3\n"
    "\n"
    "0 0  -42000\n"
    "0 0 0 0\n"
    "0 0 0 0\n"
    "                    4\n"
    "\n"
    "0 0 3&0 126000      7\n"
    "0 0 0 0\n"
    "0 0 0 0\n"
    "                    5\n"
    "\n"
    "0 0 40 -126000      &\n"
    "0 0  0     1\n"
    "0 0 0 0\n"
    "                    6\n"
    "\n"
    "0 0 -80 42000\n"
    "0 0 3&-1494000 0     &\n"
    "0 0 0         5\n"
    "                    7\n"
    "\n"
    "0 0 110 0\n"
    "0 0 1000 0\n"
    "0 3&0          &\n"
    "                    8\n"
    "\n"
    "0 0 -10 0\n"
    "0 3&0 0 0   1\n"
    "0 3&119992000000 3&992000 3&38500\n"
    "                    9\n"
    "\n"
    "0 0 -30 0\n"
    "0 3&110013500000 0 0   &\n"
    "0 -1000000 -1000 0\n"
    ;

#define N_EPOCHS 10
#define N_SATS 3
#define N_OBS 4

/* The number of explicit zeros and of flagged blanks in zero_rnx. */
#define N_ZEROS 9
#define N_FLAGGED 2

/** One observation, as a RINEX file would show it. */
struct cell
{
    int64_t obs;
    char lli;
    char ssi;
};

static struct cell ref[N_EPOCHS][N_SATS][N_OBS];
static const int sat_num[N_SATS] = { 1, 11, 21 };

static const char *rnx_path = "test_zero_obs.rnx";
static const char *crx_path = "test_zero_obs.crx";
static const char *srnx_path = "test_zero_obs.srnx";
static const char *socd_path = "test_zero_obs_socd.srnx";
static const char *back_path = "test_zero_obs_back.rnx";
static const char *merge_path = "test_zero_obs_merge.rnx";
static const char *log_path = "test_zero_obs.log";

static int write_file(const char *path, const char *text)
{
    FILE *out = fopen(path, "wb");
    size_t len = strlen(text);
    int ok_write;
    if (!out)
        return -1;
    ok_write = fwrite(text, 1, len, out) == len;
    fclose(out);
    return ok_write ? 0 : -1;
}

static int run(const char *fmt, const char *aa, const char *bb)
{
    char cmd[1024];
    snprintf(cmd, sizeof cmd, fmt, aa, bb);
    return system(cmd);
}

/* Fills ref[] from zero_rnx by slicing 16-column fields, independently
 * of the library's parsers.  Returns the number of explicit zeros, and
 * counts blanks with indicators in *p_flagged.
 */
static int build_reference(int *p_flagged)
{
    const char *line = strstr(zero_rnx, "END OF HEADER\n") + 14;
    int ep = -1, n_zeros = 0, ss, kk, len;

    *p_flagged = 0;
    for (; *line; line += len + 1)
    {
        len = (int)(strchr(line, '\n') - line);
        if (line[0] == '>')
        {
            ++ep;
            continue;
        }
        ss = (atoi(line + 1) == 1) ? 0 : (atoi(line + 1) == 11) ? 1 : 2;
        for (kk = 0; kk < N_OBS; ++kk)
        {
            char field[17];
            int start = 3 + 16 * kk, jj;

            for (jj = 0; jj < 16; ++jj)
                field[jj] = (start + jj < len) ? line[start + jj] : ' ';
            field[16] = '\0';
            ref[ep][ss][kk].lli = field[14];
            ref[ep][ss][kk].ssi = field[15];
            field[14] = '\0';
            if (strspn(field, " ") == 14)
            {
                ref[ep][ss][kk].obs = INT64_MIN;
                *p_flagged += ref[ep][ss][kk].lli != ' '
                    || ref[ep][ss][kk].ssi != ' ';
            }
            else
            {
                ref[ep][ss][kk].obs = llround(strtod(field, NULL) * 1000);
                n_zeros += ref[ep][ss][kk].obs == 0;
            }
        }
    }
    return n_zeros;
}

/* Reads \a path with rinex_open() and compares every observation with
 * ref[].  With \a drop_flags, a blank field is expected to have blank
 * indicators (as after a trip through SRNX).
 */
static void check_parser(const char *path, int drop_flags, const char *what)
{
    struct rinex_stream *stream = rinex_mmap_stream(path);
    struct rinex_parser *p = NULL;
    const char *err;
    rinex_error_t res;
    int ep, ii, kk, bad = 0, n_zeros = 0;

    err = stream ? rinex_open(&p, stream) : "no stream";
    ok(err == NULL, "%s: open (%s)", what, err ? err : "ok");
    if (err)
    {
        if (stream)
            stream->destroy(stream);
        return;
    }
    for (ep = 0; (res = p->read(p)) == RINEX_SUCCESS; ++ep)
    {
        if (ep >= N_EPOCHS || p->epoch.n_sats != N_SATS)
        {
            diag("%s: epoch %d has %d satellites", what, ep, p->epoch.n_sats);
            bad++;
            continue;
        }
        for (ii = 0; ii < N_SATS; ++ii)
        {
            const struct rinex_sv_info *sv = &p->sats[ii];
            int ss;

            for (ss = 0; ss < N_SATS && sat_num[ss] != sv->number; ++ss) {}
            if (sv->system != 'C' || ss == N_SATS)
            {
                diag("%s: epoch %d: unexpected satellite %c%02d", what,
                    ep, sv->system, sv->number);
                bad++;
                continue;
            }
            for (kk = 0; kk < N_OBS; ++kk)
            {
                const struct cell *want = &ref[ep][ss][kk];
                int64_t obs = p->obs[sv->obs_0 + kk];
                char lli = p->lli[sv->obs_0 + kk];
                char ssi = p->ssi[sv->obs_0 + kk];
                int blank_flags = drop_flags && want->obs == INT64_MIN;
                char want_lli = blank_flags ? ' ' : want->lli;
                char want_ssi = blank_flags ? ' ' : want->ssi;

                n_zeros += obs == 0;
                if (obs != want->obs || lli != want_lli || ssi != want_ssi)
                {
                    diag("%s: epoch %d C%02d obs %d: %lld '%c%c', want %lld '%c%c'",
                        what, ep, sat_num[ss], kk, (long long)obs, lli, ssi,
                        (long long)want->obs, want_lli, want_ssi);
                    bad++;
                }
            }
        }
    }
    ok(res == RINEX_EOF && ep == N_EPOCHS, "%s: read %d epochs to EOF", what, ep);
    ok(bad == 0, "%s: observations match the reference", what);
    cmp_ok(n_zeros, "==", N_ZEROS, "%s: all explicit zeros are present", what);
    p->destroy(p);
    stream->destroy(stream);
}

/* Checks the columns that rinex_load_file() makes from \a path. */
static void check_load(const char *path, int n_dropped, const char *what)
{
    struct rinex_data data;
    const struct rinex_satellite_data *sv;
    const struct rinex_system_data *sys;
    const char *err;
    int ep, ss, kk, bad = 0;

    err = rinex_load_file(path, &data);
    ok(err == NULL, "%s: load (%s)", what, err ? err : "ok");
    if (err)
        return;
    cmp_ok(data.n_dropped_flags, "==", n_dropped,
        "%s: counts blank fields with indicators", what);
    if (n_dropped)
    {
        ok(strstr(data.warning, "'1 ' for C11 D2I at 2024-03-01 12:00:05") != NULL,
            "%s: warning names the first one (%s)", what, data.warning);
    }

    sys = &data.sys['C' & 31];
    for (ss = 0; ss < N_SATS; ++ss)
    {
        sv = data.sv[sys->sv.start + sat_num[ss] - 1];
        if (!sv || sv->when_used != 1 || sv->obs_used != N_EPOCHS)
        {
            diag("%s: C%02d is missing or not in every epoch", what, sat_num[ss]);
            bad++;
            continue;
        }
        for (kk = 0; kk < N_OBS; ++kk)
        {
            for (ep = 0; ep < N_EPOCHS; ++ep)
            {
                int64_t obs = data.obs[sv->start[kk] + ep];
                if (obs != ref[ep][ss][kk].obs)
                {
                    diag("%s: epoch %d C%02d obs %d: %lld, want %lld", what,
                        ep, sat_num[ss], kk, (long long)obs,
                        (long long)ref[ep][ss][kk].obs);
                    bad++;
                }
            }
        }
    }
    ok(bad == 0, "%s: columns hold zeros and INT64_MIN as in the file", what);
    free_rinex_data(&data);
}

/* Returns nonzero if \a path contains \a needle. */
static int file_contains(const char *path, const char *needle)
{
    char buf[4096];
    FILE *in = fopen(path, "r");
    size_t len;

    if (!in)
        return 0;
    len = fread(buf, 1, sizeof buf - 1, in);
    fclose(in);
    buf[len] = '\0';
    return strstr(buf, needle) != NULL;
}

int main(void)
{
    int n_flagged, n_zeros;

    plan(NO_PLAN);

    n_zeros = build_reference(&n_flagged);
    cmp_ok(n_zeros, "==", N_ZEROS, "reference has %d explicit zeros", N_ZEROS);
    cmp_ok(n_flagged, "==", N_FLAGGED, "reference has %d flagged blanks", N_FLAGGED);
    ok(write_file(rnx_path, zero_rnx) == 0, "wrote %s", rnx_path);
    ok(write_file(crx_path, zero_crx) == 0, "wrote %s", crx_path);

    /* The text and CRINEX parsers keep zeros, blanks and indicators. */
    check_parser(rnx_path, 0, "RINEX");
    check_parser(crx_path, 0, "CRINEX");

    /* Columns keep zeros, but not indicators of blank fields. */
    check_load(rnx_path, N_FLAGGED, "RINEX load");
    check_load(crx_path, N_FLAGGED, "CRINEX load");

    /* rnx2srnx keeps the zeros, and warns about the indicators. */
    remove(srnx_path);
    ok(run(RNX2SRNX_PATH " %s %s 2>test_zero_obs.log", rnx_path, srnx_path) == 0,
        "rnx2srnx converts RINEX");
    ok(file_contains(log_path, "dropped indicators of 2 blank observations"),
        "rnx2srnx warns about blank fields with indicators");
    check_parser(srnx_path, 1, "SRNX");
    check_load(srnx_path, 0, "SRNX load");
    ok(run(RNXCMP_PATH " %s %s >/dev/null 2>&1", rnx_path, srnx_path) == 0,
        "rnxcmp: RINEX matches SRNX");

    remove(socd_path);
    ok(run(RNX2SRNX_PATH " --socd %s %s 2>/dev/null", crx_path, socd_path) == 0,
        "rnx2srnx --socd converts CRINEX");
    check_parser(socd_path, 1, "SRNX (SOCD)");
    ok(run(RNXCMP_PATH " %s %s >/dev/null 2>&1", crx_path, socd_path) == 0,
        "rnxcmp: CRINEX matches SOCD SRNX");

    /* srnx2rnx writes the zeros back as 0.000. */
    ok(run(SRNX2RNX_PATH " %s %s", srnx_path, back_path) == 0,
        "srnx2rnx converts back to RINEX");
    check_parser(back_path, 1, "SRNX to RINEX");

    /* rinex-merge keeps zeros and blank fields with indicators. */
    ok(run(RINEX_MERGE_PATH " -o %s %s", merge_path, rnx_path) == 0,
        "rinex-merge copies a single input");
    check_parser(merge_path, 0, "rinex-merge");

    remove(rnx_path);
    remove(crx_path);
    remove(srnx_path);
    remove(socd_path);
    remove(back_path);
    remove(merge_path);
    remove(log_path);

    done_testing();
    return exit_status();
}
