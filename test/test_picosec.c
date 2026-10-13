/** test_picosec.c - RINEX 4.02 picosecond epochs and SRNX EPO2 chunks.
 * Copyright 2026 Michael Poole.
 * SPDX-License-Identifier: MIT-Modern-Variant
 *
 * Checks that the RINEX and CRINEX 3.1 parsers read the optional extra
 * digits of an epoch's seconds, that rnx2srnx writes an EPO2 chunk (and
 * SRNX 1.2) only when some epoch has them, that SRNX readers and srnx2rnx
 * give them back, that SOC2 rate predictors count picoseconds with EPO2,
 * and that the EPO2 decoder checks its input.
 */

#include "rinex/rnx_priv.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <tap.h>

#ifndef RNX2SRNX_PATH
# define RNX2SRNX_PATH "./rnx2srnx"
#endif
#ifndef RNX2SRNX_SEARCH_PATH
# define RNX2SRNX_SEARCH_PATH "./rnx2srnx_search"
#endif
#ifndef SRNX2RNX_PATH
# define SRNX2RNX_PATH "./srnx2rnx"
#endif
#ifndef RNXCMP_PATH
# define RNXCMP_PATH "./rnxcmp"
#endif

#define HEADER \
    "     4.02           OBSERVATION DATA    M                   RINEX VERSION / TYPE\n" \
    "G    4 C1C L1C D1C S1C                                      SYS / # / OBS TYPES\n" \
    "E    2 C1C L1C                                              SYS / # / OBS TYPES\n" \
    "                                                            END OF HEADER\n"

/* Epochs with and without a clock offset and picoseconds, and an event. */
static const char ps_rinex[] = HEADER
    "> 2024 10 01 00 00  0.0000000  0  2      -0.123456789012 12345\n"
    "G05  23629347.915   124212345.123 7      -525.500          44.250\n"
    "E11  25000000.125   130000000.250 8\n"
    "> 2024 10 01 00 00  1.0000000  0  2                      23456\n"
    "G05  23629447.915   124212870.623 7      -525.400          44.250\n"
    "E11  25000050.125   130000262.250 8\n"
    "> 2024 10 01 00 00  2.0000000  0  2      -0.123456789210\n"
    "G05  23629547.915   124213396.123 7      -525.300          44.250\n"
    "E11  25000100.125   130000524.250 8\n"
    "> 2024 10 01 00 00  3.0000000  0  2\n"
    "G05  23629647.915   124213921.623 7      -525.200          44.250\n"
    "E11  25000150.125   130000786.250 8\n"
    "> 2024 10 01 00 00  4.0000001  0  2                      00000\n"
    "G05  23629747.915   124214447.123 7      -525.100          44.250\n"
    "E11  25000200.125   130001048.250 8\n"
    "> 2024 10 01 00 00  4.5000000  5  0                      50000\n"
    "> 2024 10 01 00 00  5.0000000  0  2      -0.123456789876 99999\n"
    "G05  23629847.915   124214972.623 7      -525.000          44.250\n"
    "E11  25000250.125   130001310.250 8\n";

/* RNX2CRX 4.2.0 output for epochs like those above, edited by hand to
 * add clock offsets (RNX2CRX 4.2.0 crashes on them on macOS) and to
 * change the second epoch's picoseconds by a partial difference.  As in
 * CRX2RNX 4.2.0, picoseconds persist across empty differences.
 */
static const char ps_crinex[] =
    "3.1                 COMPACT RINEX FORMAT                    CRINEX VERS   / TYPE\n"
    "RNX2CRX ver.4.2.0                       13-Oct-26 13:00     CRINEX PROG / DATE\n"
    HEADER
    "> 2024 10 01 00 00  0.0000000  0  2      G05E11\n"
    "3&-123456789012 12345\n"
    "3&23629347915 3&124212345123 3&-525500 3&44250 &&&7&&&&\n"
    "3&25000000125 3&130000000250 &&&8\n"
    "                    1\n"
    "    99\n"
    "100000 525500 100 0\n"
    "50000 262000\n"
    "                    2\n"
    "\n"
    "0 0 0 0\n"
    "0 0\n"
    "                    3\n"
    "\n"
    "0 0 0 0\n"
    "0 0\n"
    "                    4       1\n"
    " 00000\n"
    "0 0 0 0\n"
    "0 0\n"
    "> 2024 10 01 00 00  4.5000000  5  0                      50000\n"
    "> 2024 10 01 00 00  5.0000000  0  2      G05E11\n"
    "3&-123456789876 99999\n"
    "3&23629847915 3&124214972623 3&-525000 3&44250 &&&7&&&&\n"
    "3&25000250125 3&130001310250 &&&8\n";

/* Expected epoch values, in file order. */
struct expect
{
    char flag;
    int sec_e7;
    int has_ps;
    int sec_ps;
    int64_t clock_offset;
};

static const struct expect text_epochs[] = {
    { '0',        0, 1, 12345, -123456789012 },
    { '0', 10000000, 1, 23456, 0 },
    { '0', 20000000, 0,     0, -123456789210 },
    { '0', 30000000, 0,     0, 0 },
    { '0', 40000001, 1,     0, 0 },
    { '5', 45000000, 1, 50000, 0 },
    { '0', 50000000, 1, 99999, -123456789876 },
};

static const struct expect crx_epochs[] = {
    { '0',        0, 1, 12345, -123456789012 },
    { '0', 10000000, 1, 12399, 0 },
    { '0', 20000000, 1, 12399, 0 },
    { '0', 30000000, 1, 12399, 0 },
    { '0', 40000001, 1,     0, 0 },
    { '5', 45000000, 1, 50000, 0 },
    { '0', 50000000, 1, 99999, -123456789876 },
};

#define N_EXPECT (int)(sizeof text_epochs / sizeof text_epochs[0])

static const char *ps_path = "test_picosec.rnx";
static const char *crx_path = "test_picosec.crx";
static const char *plain_path = "test_picosec_plain.rnx";
static const char *srnx_path = "test_picosec.srnx";
static const char *socd_path = "test_picosec_socd.srnx";
static const char *raw_path = "test_picosec_raw.srnx";
static const char *back_path = "test_picosec_back.rnx";
static const char *rate_path = "test_picosec_rate.rnx";
static const char *bad_path = "test_picosec_bad.rnx";

static int write_file(const char *path, const void *buf, size_t len)
{
    FILE *out = fopen(path, "wb");
    int ok_write;
    if (!out)
        return -1;
    ok_write = fwrite(buf, 1, len, out) == len;
    fclose(out);
    return ok_write ? 0 : -1;
}

static unsigned char *read_file(const char *path, size_t *p_len)
{
    FILE *in = fopen(path, "rb");
    unsigned char *buf;
    long len;

    if (!in)
        return NULL;
    fseek(in, 0, SEEK_END);
    len = ftell(in);
    fseek(in, 0, SEEK_SET);
    buf = malloc((size_t)len + 1);
    if (buf && fread(buf, 1, (size_t)len, in) != (size_t)len)
    {
        free(buf);
        buf = NULL;
    }
    if (buf)
        buf[len] = '\0';
    fclose(in);
    *p_len = (size_t)len;
    return buf;
}

static uint64_t uleb(const unsigned char **pp)
{
    uint64_t value = 0;
    int shift = 0;
    unsigned char ch;
    do
    {
        ch = *(*pp)++;
        value |= (uint64_t)(ch & 127) << shift;
        shift += 7;
    } while (ch & 128);
    return value;
}

/* Returns the payload offset of the first \a fourcc chunk in an SRNX
 * file, or 0 if there is none. */
static size_t find_chunk(const unsigned char *buf, size_t len,
    const char *fourcc)
{
    size_t pos = 0, digest_len = 0;

    /* Each chunk is followed by its digest; buf[7] identifies that. */
    if (len > 7 && buf[7])
        digest_len = (size_t)1 << (buf[7] & 15);

    while (pos + 5 < len)
    {
        const unsigned char *pp = buf + pos + 4;
        uint64_t plen = uleb(&pp);
        size_t payload = (size_t)(pp - buf);
        if (payload + plen > len)
            break;
        if (!memcmp(buf + pos, fourcc, 4))
            return payload;
        pos = payload + (size_t)plen + digest_len;
    }
    return 0;
}

static int run(const char *fmt, const char *aa, const char *bb)
{
    char cmd[1024];
    snprintf(cmd, sizeof cmd, fmt, aa, bb);
    return system(cmd);
}

/* Reads every record of \a path, comparing epochs with \a want. */
static void check_parse(const char *path, const struct expect *want,
    const char *what)
{
    struct rinex_stream *stream = rinex_mmap_stream(path);
    struct rinex_parser *p = NULL;
    const char *err;
    rinex_error_t res;
    int ii, bad = 0;

    err = stream ? rinex_open(&p, stream) : "no stream";
    ok(err == NULL, "%s: open (%s)", what, err ? err : "ok");
    if (err)
    {
        if (stream)
            stream->destroy(stream);
        return;
    }
    for (ii = 0; (res = p->read(p)) == RINEX_SUCCESS; ++ii)
    {
        const struct rinex_epoch *ep = &p->epoch;
        if (ii >= N_EXPECT)
        {
            bad++;
            continue;
        }
        if (ep->flag != want[ii].flag || ep->sec_e7 != want[ii].sec_e7
            || !ep->has_ps != !want[ii].has_ps || ep->sec_ps != want[ii].sec_ps
            || ep->clock_offset != want[ii].clock_offset)
        {
            diag("%s: record %d: flag %c sec_e7 %d has_ps %d sec_ps %d clock %lld",
                what, ii, ep->flag, ep->sec_e7, ep->has_ps, ep->sec_ps,
                (long long)ep->clock_offset);
            bad++;
        }
        if (ep->flag == '0' && ii == N_EXPECT - 1
            && p->obs[p->sats[0].obs_0] != 23629847915)
        {
            diag("%s: last epoch's first observation is %lld", what,
                (long long)p->obs[p->sats[0].obs_0]);
            bad++;
        }
    }
    ok(res == RINEX_EOF && ii == N_EXPECT && bad == 0,
        "%s: %d records with expected times (%d bad, res %d)", what, ii, bad, res);
    p->destroy(p);
    stream->destroy(stream);
}

/* Expects the first epoch of a file with the given epoch line to fail. */
static void check_bad_epoch(const char *line, const char *what)
{
    char text[1024];
    struct rinex_stream *stream;
    struct rinex_parser *p = NULL;
    rinex_error_t res = RINEX_SUCCESS;

    snprintf(text, sizeof text, "%s%s\nG05  23629347.915\n", HEADER, line);
    write_file(bad_path, text, strlen(text));
    stream = rinex_mmap_stream(bad_path);
    if (stream && !rinex_open(&p, stream))
    {
        res = p->read(p);
        p->destroy(p);
    }
    ok(res == RINEX_ERR_BAD_FORMAT, "%s is rejected (%d)", what, res);
    if (stream)
        stream->destroy(stream);
}

static void test_parsers(void)
{
    char crx[sizeof ps_crinex], *pos;

    write_file(ps_path, ps_rinex, sizeof ps_rinex - 1);
    check_parse(ps_path, text_epochs, "RINEX 4.02");
    write_file(crx_path, ps_crinex, sizeof ps_crinex - 1);
    check_parse(crx_path, crx_epochs, "CRINEX 3.1");

    check_bad_epoch("> 2024 10 01 00 00  0.0000000  0  1                      1234",
        "four picosecond digits");
    check_bad_epoch("> 2024 10 01 00 00  0.0000000  0  1                      12a45",
        "non-digit picoseconds");
    check_bad_epoch("> 2024 10 01 00 00  0.0000000  0  1      -0.123456789012123456",
        "picoseconds without a separating space");

    /* Clearing a picosecond digit leaves an invalid field. */
    memcpy(crx, ps_crinex, sizeof crx);
    pos = strstr(crx, "\n    99\n");
    memcpy(pos + 1, "   &99", 6);
    write_file(bad_path, crx, sizeof crx - 1);
    {
        struct rinex_stream *stream = rinex_mmap_stream(bad_path);
        struct rinex_parser *p = NULL;
        rinex_error_t res = RINEX_SUCCESS;
        if (stream && !rinex_open(&p, stream))
        {
            res = p->read(p);
            if (res == RINEX_SUCCESS)
                res = p->read(p);
            p->destroy(p);
        }
        ok(res == RINEX_ERR_BAD_FORMAT, "CRINEX picoseconds cleared to a space are rejected");
        if (stream)
            stream->destroy(stream);
    }
}

/* Compares every observation, LLI and SSI of two loaded files. */
static int same_data(const struct rinex_data *aa, const struct rinex_data *bb)
{
    int sys, sv, jj, kk, diffs = 0;

    if (aa->epoch_used != bb->epoch_used)
        return 0;
    for (sys = 0; sys < 32; ++sys)
    {
        const struct rinex_system_data *pa = &aa->sys[sys], *pb = &bb->sys[sys];
        if (pa->n_obs != pb->n_obs)
            return 0;
        for (sv = 0; sv < pa->sv.end - pa->sv.start; ++sv)
        {
            const struct rinex_satellite_data *sa = aa->sv[pa->sv.start + sv];
            const struct rinex_satellite_data *sb = bb->sv[pb->sv.start + sv];
            if (!sa || !sb || sa->obs_used == 0)
            {
                diffs += (sa && sa->obs_used) != (sb && sb->obs_used);
                continue;
            }
            if (sa->obs_used != sb->obs_used)
                return 0;
            for (jj = 0; jj < pa->n_obs; ++jj)
            {
                if ((sa->start[jj] < 0) != (sb->start[jj] < 0))
                    return 0;
                if (sa->start[jj] < 0)
                    continue;
                for (kk = 0; kk < sa->obs_used; ++kk)
                {
                    int ia = sa->start[jj] + kk, ib = sb->start[jj] + kk;
                    diffs += aa->obs[ia] != bb->obs[ib]
                        || aa->lli[ia] != bb->lli[ib]
                        || aa->ssi[ia] != bb->ssi[ib];
                }
            }
        }
    }
    return diffs == 0;
}

/* Compares epoch times (and clock offsets) of two loaded files; with
 * \a all_ps, \a bb must have picoseconds for every epoch. */
static int same_epochs(const struct rinex_data *aa, const struct rinex_data *bb,
    int all_ps)
{
    int ii;

    if (aa->epoch_used != bb->epoch_used)
        return 0;
    for (ii = 0; ii < aa->epoch_used; ++ii)
    {
        const struct rinex_epoch *ea = &aa->epoch[ii], *eb = &bb->epoch[ii];
        if (ea->yyyy_mm_dd != eb->yyyy_mm_dd || ea->hh_mm != eb->hh_mm
            || ea->sec_e7 != eb->sec_e7 || ea->sec_ps != eb->sec_ps
            || ea->clock_offset != eb->clock_offset
            || (all_ps ? !eb->has_ps : eb->has_ps))
        {
            diag("epoch %d differs: %d.%07d %05d (%d) vs %d.%07d %05d (%d)", ii,
                ea->hh_mm, ea->sec_e7, ea->sec_ps, ea->has_ps,
                eb->hh_mm, eb->sec_e7, eb->sec_ps, eb->has_ps);
            return 0;
        }
    }
    return 1;
}

/* Checks the SRNX minor version and epoch chunk of \a path. */
static void check_layout(const char *path, int minor, const char *fourcc,
    const char *what)
{
    unsigned char *buf;
    size_t len;
    int other;

    buf = read_file(path, &len);
    if (!buf)
    {
        ok(0, "%s: read %s", what, path);
        return;
    }
    other = !strcmp(fourcc, "EPO2")
        ? find_chunk(buf, len, "EPOC") != 0
        : find_chunk(buf, len, "EPO2") != 0;
    ok(len > 7 && buf[5] == 1 && buf[6] == minor
        && find_chunk(buf, len, fourcc) && !other,
        "%s: SRNX 1.%d with %s", what, minor, fourcc);
    free(buf);
}

static void test_srnx(void)
{
    struct rinex_data text, loaded, plain_text;
    unsigned char *out;
    char *plain, *line, *eol;
    size_t len;
    const char *err;
    int res, n_ps;

    err = rinex_load_file(ps_path, &text);
    ok(err == NULL, "load RINEX 4.02 text (%s)", err ? err : "ok");
    if (err)
        return;

    /* SOC2 (the default), SOCD and digest-free files all use EPO2. */
    res = run("\"" RNX2SRNX_PATH "\" %s %s", ps_path, srnx_path);
    ok(res == 0, "rnx2srnx converts picosecond epochs");
    check_layout(srnx_path, 2, "EPO2", "SOC2 output");
    res = run("\"" RNX2SRNX_PATH "\" --socd %s %s", ps_path, socd_path);
    ok(res == 0, "rnx2srnx --socd converts picosecond epochs");
    check_layout(socd_path, 2, "EPO2", "SOCD output");
    res = run("\"" RNX2SRNX_PATH "\" --chunk-digest=0 --file-digest=0 %s %s",
        ps_path, raw_path);
    ok(res == 0, "rnx2srnx converts picosecond epochs without digests");

    res = srnx_load(srnx_path, &loaded);
    ok(res == 0, "load EPO2 file (%s)", res ? loaded.error : "ok");
    ok(res == 0 && same_epochs(&text, &loaded, 1),
        "EPO2 epochs match the text, all with picoseconds");
    ok(res == 0 && same_data(&text, &loaded), "EPO2 file's observations match the text");
    ok(res == 0 && loaded.event_used == 1, "EPO2 file keeps the event record");
    free_rinex_data(&loaded);

    res = srnx_load(socd_path, &loaded);
    ok(res == 0 && same_epochs(&text, &loaded, 1) && same_data(&text, &loaded),
        "SOCD file with EPO2 matches the text");
    free_rinex_data(&loaded);

    /* The streaming SRNX parser reports the same picoseconds. */
    {
        struct rinex_stream *stream = rinex_mmap_stream(srnx_path);
        err = stream ? rinex_load(stream, &loaded) : "no stream";
        ok(err == NULL && same_epochs(&text, &loaded, 1) && same_data(&text, &loaded),
            "streaming parser reads EPO2 like the text (%s)", err ? err : "ok");
        if (!err)
            free_rinex_data(&loaded);
        if (stream)
            stream->destroy(stream);
    }

    /* srnx2rnx writes picoseconds for every epoch of an EPO2 file. */
    remove(back_path);
    res = run("\"" SRNX2RNX_PATH "\" %s %s", srnx_path, back_path);
    out = read_file(back_path, &len);
    ok(res == 0 && out
        && strstr((char *)out, "\n> 2024 10 01 00 00  0.0000000  0  2      -0.123456789012 12345\n")
        && strstr((char *)out, "\n> 2024 10 01 00 00  2.0000000  0  2      -0.123456789210 00000\n")
        && strstr((char *)out, "\n> 2024 10 01 00 00  3.0000000  0  2                      00000\n")
        && strstr((char *)out, "\n> 2024 10 01 00 00  4.5000000  5  0                      50000\n"),
        "srnx2rnx writes picoseconds from EPO2");
    free(out);
    res = run("\"" RNXCMP_PATH "\" %s %s >/dev/null", ps_path, back_path);
    ok(res == 0, "rnxcmp finds no difference after the round trip");
    res = run("\"" RNXCMP_PATH "\" %s %s >/dev/null", ps_path, srnx_path);
    ok(res == 0, "rnxcmp finds no difference between text and EPO2");
    res = run("\"" RNX2SRNX_SEARCH_PATH "\" --soc2 --tuple=8 %s >%s",
        ps_path, "/dev/null");
    ok(res == 0, "rnx2srnx_search accepts picosecond epochs");

    /* Without picoseconds, the output keeps EPOC and SRNX 1.1. */
    plain = strdup(ps_rinex);
    for (line = plain; (eol = strchr(line, '\n')) != NULL; line = eol + 1)
    {
        if (line[0] == '>' && eol - line == 62)
        {
            int keep = 56;
            while (keep > 35 && line[keep - 1] == ' ')
                --keep;
            memmove(line + keep, eol, strlen(eol) + 1);
            eol = line + keep;
        }
    }
    write_file(plain_path, plain, strlen(plain));
    free(plain);
    res = run("\"" RNX2SRNX_PATH "\" %s %s", plain_path, srnx_path);
    ok(res == 0, "rnx2srnx converts epochs without picoseconds");
    check_layout(srnx_path, 1, "EPOC", "output without picoseconds");
    err = rinex_load_file(plain_path, &plain_text);
    res = err ? -1 : srnx_load(srnx_path, &loaded);
    ok(res == 0 && same_epochs(&plain_text, &loaded, 0)
        && same_data(&plain_text, &loaded), "EPOC file matches the text");
    if (res == 0)
        free_rinex_data(&loaded);
    if (!err)
        free_rinex_data(&plain_text);
    remove(back_path);
    res = run("\"" SRNX2RNX_PATH "\" %s %s", srnx_path, back_path);
    out = read_file(back_path, &len);
    n_ps = 0;
    for (line = (char *)out; out && (eol = strchr(line, '\n')) != NULL; line = eol + 1)
        n_ps += line[0] == '>' && eol - line > 56;
    ok(res == 0 && out && n_ps == 0, "srnx2rnx writes no picoseconds from EPOC");
    free(out);

    free_rinex_data(&text);
}

/* Deterministic noise in [-1, 1). */
static double noise(unsigned *state)
{
    *state = *state * 1103515245u + 12345u;
    return ((*state >> 8) & 0xFFFF) / 32768.0 - 1.0;
}

/* Writes a satellite with smooth carrier phase and noisy Doppler at
 * epochs a second apart, plus picoseconds that change from epoch to
 * epoch.  (A rate predictor needs a few hundred epochs to pay off.) */
static void write_rate_rinex(void)
{
    const double c = 299792458.0, f1 = 1575420000.0;
    FILE *out = fopen(rate_path, "w");
    unsigned rng = 1;
    int ee;

    if (!out)
        BAIL_OUT("cannot write %s", rate_path);
    fputs(HEADER, out);
    for (ee = 0; ee < 600; ++ee)
    {
        double tt = ee, range = 2.1e7 + 650.0 * tt + 0.04 * tt * tt;
        double rate = 650.0 + 0.08 * tt;
        fprintf(out, "> 2024 10 01 00 %02d %10.7f  0  1%21s %05d\n",
            ee / 60, (double)(ee % 60), "", (ee * 7919) % 100000);
        fprintf(out, "G01%14.3f  %14.3f  %14.3f  %14.3f\n",
            range + 0.3 * noise(&rng), range * f1 / c,
            -rate * f1 / c + 0.02 * noise(&rng), 45.0);
    }
    fclose(out);
}

static void test_rate_predictor(void)
{
    struct rinex_data text, loaded;
    unsigned char *buf;
    size_t len, pos, end;
    const char *err;
    int res, n_rate = 0, good_span = 0;

    write_rate_rinex();
    err = rinex_load_file(rate_path, &text);
    ok(err == NULL, "load jittered picosecond epochs (%s)", err ? err : "ok");
    if (err)
        return;
    res = run("\"" RNX2SRNX_PATH "\" --chunk-digest=0 --file-digest=0 %s %s",
        rate_path, raw_path);
    ok(res == 0, "rnx2srnx converts jittered picosecond epochs");
    res = srnx_load(raw_path, &loaded);
    ok(res == 0 && same_epochs(&text, &loaded, 1) && same_data(&text, &loaded),
        "jittered EPO2 file matches the text");
    if (res == 0)
        free_rinex_data(&loaded);
    free_rinex_data(&text);

    /* Find the Doppler's rate predictor: its max_span (300 s) must be in
     * picoseconds. */
    buf = read_file(raw_path, &len);
    for (pos = 0; buf && pos + 5 < len; pos = end)
    {
        const unsigned char *pp = buf + pos + 4;
        uint64_t plen = uleb(&pp), mode;
        end = (size_t)(pp - buf) + (size_t)plen;
        if (memcmp(buf + pos, "SOC2", 4))
            continue;
        pp += 8;                /* observation name */
        uleb(&pp);              /* count - 1 */
        pp += uleb(&pp);        /* LLIs */
        mode = uleb(&pp);
        pp += uleb(&pp);        /* SSIs */
        if ((mode & 3) != 2)
            continue;
        n_rate++;
        uleb(&pp);              /* reference */
        uleb(&pp);              /* num */
        uleb(&pp);              /* den */
        uleb(&pp);              /* stencil */
        good_span += uleb(&pp) == 300000000000000ULL;
    }
    ok(n_rate > 0 && good_span == n_rate,
        "rate predictors count picoseconds (%d of %d)", good_span, n_rate);
    free(buf);
}

static void put_u64(unsigned char **p, uint64_t value)
{
    do {
        *(*p)++ = (value & 127) | (value > 127 ? 128 : 0);
        value >>= 7;
    } while (value);
}

static void chunk(unsigned char **p, const char *name, const void *data, size_t size)
{
    memcpy(*p, name, 4); *p += 4;
    put_u64(p, size);
    memcpy(*p, data, size); *p += size;
}

/* Decodes a hand-made EPO2 payload. */
static int decode_epo2(const unsigned char *payload, size_t size,
    struct rinex_epoch **p_epochs, size_t *p_count)
{
    static const char header[] =
        "     4.02           OBSERVATION DATA    G                   RINEX VERSION / TYPE\n"
        "G    1 C1C                                                  SYS / # / OBS TYPES\n"
        "                                                            END OF HEADER\n";
    static const unsigned char version[] = {1, 2, 0, 0, 0};
    unsigned char file[1024], *p = file;
    struct srnx_reader *reader = NULL;
    int result;

    chunk(&p, "SRNX", version, sizeof version);
    chunk(&p, "RHDR", header, sizeof header - 1);
    chunk(&p, "EPO2", payload, size);
    write_file(raw_path, file, (size_t)(p - file));
    result = srnx_open(&reader, raw_path, 0);
    if (!result)
        result = srnx_get_epochs(reader, p_epochs, p_count);
    srnx_close(reader);
    return result;
}

static void test_epo2_decoding(void)
{
    unsigned char payload[128], *p = payload;
    struct rinex_epoch *epochs = NULL;
    size_t count = 0;
    int res;

    /* Three epochs 30.000000000001 s apart from 00:59:00.000000099999,
     * rolling over to 01:00:00.000000100001. */
    put_u64(&p, 3);
    put_u64(&p, 2 * 30000000000001ULL);
    put_u64(&p, 2);
    put_u64(&p, 20241001);
    put_u64(&p, 59 * 100000000000000ULL + 99999);
    res = decode_epo2(payload, (size_t)(p - payload), &epochs, &count);
    ok(res == 0 && count == 3
        && epochs[0].hh_mm == 59 && epochs[0].sec_e7 == 0 && epochs[0].sec_ps == 99999
        && epochs[1].hh_mm == 59 && epochs[1].sec_e7 == 300000001 && epochs[1].sec_ps == 0
        && epochs[2].hh_mm == 100 && epochs[2].sec_e7 == 1 && epochs[2].sec_ps == 1
        && epochs[0].has_ps && epochs[2].has_ps,
        "EPO2 spans step in picoseconds and roll over minutes (res %d)", res);

    /* Epochs a year apart span more than 2^63 picoseconds. */
    p = payload;
    put_u64(&p, 2);
    put_u64(&p, 0);
    put_u64(&p, 0);
    put_u64(&p, 20240101);
    put_u64(&p, 0);
    put_u64(&p, 0);
    put_u64(&p, 0);
    put_u64(&p, 20250101);
    put_u64(&p, 0);
    res = decode_epo2(payload, (size_t)(p - payload), &epochs, &count);
    ok(res == SRNX_CORRUPT, "EPO2 epochs a year apart are rejected (res %d)", res);

    /* A time of day past 24:60:61 is invalid. */
    p = payload;
    put_u64(&p, 1);
    put_u64(&p, 0);
    put_u64(&p, 0);
    put_u64(&p, 20241001);
    put_u64(&p, 2460 * 100000000000000ULL + 61000000000001ULL);
    res = decode_epo2(payload, (size_t)(p - payload), &epochs, &count);
    ok(res == SRNX_CORRUPT, "EPO2 time of day past 24:60:61 is rejected (res %d)", res);
    free(epochs);
}

int main(void)
{
    test_parsers();
    test_srnx();
    test_rate_predictor();
    test_epo2_decoding();

    remove(ps_path);
    remove(crx_path);
    remove(plain_path);
    remove(srnx_path);
    remove(socd_path);
    remove(raw_path);
    remove(back_path);
    remove(rate_path);
    remove(bad_path);
    done_testing();
    return exit_status();
}
