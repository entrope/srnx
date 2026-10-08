/** test_soc2.c - SOC2 (SRNX revision 1.1) round trips and robustness.
 * Copyright 2026 Michael Poole.
 * SPDX-License-Identifier: MIT-Modern-Variant
 *
 * Writes a synthetic dual-frequency RINEX 3 file with smooth dynamics, so
 * that rnx2srnx chooses every SOC2 predictor kind and SSI references,
 * then checks that SOC2 and SOCD conversions load back exactly, that the
 * expected SOC2 features were used, that newer minor versions are
 * rejected, and that corrupted SOC2 payloads fail cleanly.
 */

#include "rinex/rinex.h"
#include "rinex/rinex_load.h"
#include "rinex/srnx.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tap.h>

#ifndef RNX2SRNX_PATH
# define RNX2SRNX_PATH "./rnx2srnx"
#endif

#define N_EPOCHS 90
#define N_SATS 3

static const char *rnx_path = "test_soc2.rnx";
static const char *soc2_path = "test_soc2.srnx";
static const char *socd_path = "test_soc2_socd.srnx";
static const char *raw_path = "test_soc2_raw.srnx";
static const char *bad_path = "test_soc2_bad.srnx";

/* Deterministic noise in [-1, 1). */
static double noise(unsigned *state)
{
    *state = *state * 1103515245u + 12345u;
    return ((*state >> 8) & 0xFFFF) / 32768.0 - 1.0;
}

static void field(FILE *out, double value, int present, char ssi)
{
    if (present)
        fprintf(out, "%14.3f %c", value, ssi);
    else
        fprintf(out, "%16s", "");
}

static void write_rinex(void)
{
    const double c = 299792458.0, f1 = 1575420000.0, f2 = 1227600000.0;
    FILE *out = fopen(rnx_path, "w");
    unsigned rng = 1;
    int ee, ss;

    if (!out)
        BAIL_OUT("cannot write %s", rnx_path);
    fprintf(out, "%-60s%s\n", "     3.04           OBSERVATION DATA    G",
        "RINEX VERSION / TYPE");
    fprintf(out, "%-60s%s\n", "G    8 C1C L1C D1C S1C C2W L2W D2W S2W",
        "SYS / # / OBS TYPES");
    fprintf(out, "%-60s%s\n", "", "END OF HEADER");

    for (ee = 0; ee < N_EPOCHS; ++ee)
    {
        /* Satellite 2 is absent for epochs 40..44. */
        int n_sats = (ee >= 40 && ee < 45) ? N_SATS - 1 : N_SATS;
        fprintf(out, "> 2020 07 07 00 %02d %10.7f  0 %2d\n",
            ee / 60, (double)(ee % 60), n_sats);
        for (ss = 0; ss < N_SATS; ++ss)
        {
            double tt = ee, range, rate, s1, s2;
            int l1_present;

            if (ss == 1 && ee >= 40 && ee < 45)
                continue;
            range = 2.1e7 + 1e6 * ss + (650.0 - 300.0 * ss) * tt + 0.04 * tt * tt;
            rate = (650.0 - 300.0 * ss) + 0.08 * tt;
            s1 = 44.0 + 6.0 * sin(tt / 15.0 + ss);
            s2 = s1 - 4.0;
            /* Satellite 1's L1C is missing for two epochs, so its C1C has
             * no prediction there (an unpredicted-values block). */
            l1_present = !(ss == 0 && (ee == 30 || ee == 31));

            fprintf(out, "G%02d", ss + 1);
            field(out, range + 0.3 * noise(&rng), 1, ' ');
            field(out, range * f1 / c + 1000.0 * (ss + 1), l1_present,
                (char)('0' + (int)(s1 / 6)));
            field(out, -rate * f1 / c + 0.02 * noise(&rng), 1, ' ');
            field(out, s1, 1, ' ');
            field(out, range + 0.4 * noise(&rng), 1, ' ');
            field(out, range * f2 / c - 2000.0 * (ss + 1), 1,
                (char)('0' + (int)(s2 / 6)));
            field(out, -rate * f2 / c + 0.02 * noise(&rng), 1, ' ');
            field(out, s2, 1, ' ');
            fputc('\n', out);
        }
    }
    fclose(out);
}

static int convert(const char *args, const char *output)
{
    char cmd[1024];
    snprintf(cmd, sizeof cmd, "\"%s\" %s %s %s", RNX2SRNX_PATH, args,
        rnx_path, output);
    return system(cmd);
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
    buf = malloc((size_t)len);
    if (buf && fread(buf, 1, (size_t)len, in) != (size_t)len)
    {
        free(buf);
        buf = NULL;
    }
    fclose(in);
    *p_len = (size_t)len;
    return buf;
}

static int write_file(const char *path, const unsigned char *buf, size_t len)
{
    FILE *out = fopen(path, "wb");
    int ok_write;
    if (!out)
        return -1;
    ok_write = fwrite(buf, 1, len, out) == len;
    fclose(out);
    return ok_write ? 0 : -1;
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

/* Walks the chunks of a digest-free SRNX file. */
struct chunk_info
{
    size_t start;     /* offset of the FOURCC */
    size_t payload;   /* offset of the payload */
    size_t length;    /* payload length */
};

static int list_chunks(const unsigned char *buf, size_t len, const char *fourcc,
    struct chunk_info *out, int max)
{
    size_t pos = 0;
    int count = 0;
    while (pos + 5 < len && count < max)
    {
        const unsigned char *pp = buf + pos + 4;
        uint64_t plen = uleb(&pp);
        size_t payload = (size_t)(pp - buf);
        if (payload + plen > len)
            break;
        if (!memcmp(buf + pos, fourcc, 4))
        {
            out[count].start = pos;
            out[count].payload = payload;
            out[count].length = (size_t)plen;
            count++;
        }
        pos = payload + (size_t)plen;
    }
    return count;
}

static void check_modes(void)
{
    struct chunk_info chunks[64];
    unsigned char *buf;
    size_t len;
    int n, ii, kinds[4] = {0, 0, 0, 0}, ssi_refs = 0;

    buf = read_file(raw_path, &len);
    ok(buf != NULL, "read digest-free SOC2 file");
    if (!buf)
        return;
    ok(len > 7 && buf[5] == 1 && buf[6] == 1, "SOC2 file has version 1.1");
    n = list_chunks(buf, len, "SOC2", chunks, 64);
    ok(n == N_SATS * 8, "one SOC2 chunk per signal (%d)", n);
    for (ii = 0; ii < n; ++ii)
    {
        const unsigned char *pp = buf + chunks[ii].payload + 8;
        uint64_t mode;
        uleb(&pp);              /* count - 1 */
        pp += uleb(&pp);        /* LLIs */
        mode = uleb(&pp);
        kinds[mode & 3]++;
        ssi_refs += (mode >> 2) != 0;
    }
    ok(kinds[1] > 0, "value predictors used (%d chunks)", kinds[1]);
    ok(kinds[2] > 0, "rate predictors used (%d chunks)", kinds[2]);
    ok(ssi_refs > 0, "SSI references used (%d chunks)", ssi_refs);
    free(buf);
}

static void check_minor_version(void)
{
    struct rinex_data data;
    unsigned char *buf;
    size_t len;
    int res;

    buf = read_file(raw_path, &len);
    if (!buf)
        BAIL_OUT("cannot read %s", raw_path);
    buf[6] = 2; /* SRNX payload: major, minor, ... */
    write_file(bad_path, buf, len);
    res = srnx_load(bad_path, &data);
    ok(res != 0, "minor version 2 is rejected");
    free_rinex_data(&data);
    free(buf);
}

/* Mutates SOC2 payload bytes; loading must fail or succeed, never crash. */
static void fuzz_soc2(int iterations)
{
    struct chunk_info chunks[64];
    unsigned char *orig, *buf;
    unsigned rng = 12345;
    size_t len;
    int n, ii, failures = 0;

    orig = read_file(raw_path, &len);
    buf = malloc(len);
    if (!orig || !buf)
        BAIL_OUT("cannot read %s", raw_path);
    n = list_chunks(orig, len, "SOC2", chunks, 64);
    for (ii = 0; ii < iterations; ++ii)
    {
        const struct chunk_info *ch;
        struct rinex_data data;
        int flips, jj;

        memcpy(buf, orig, len);
        rng = rng * 1103515245u + 12345u;
        ch = &chunks[(rng >> 8) % (unsigned)n];
        flips = 1 + (int)((rng >> 4) % 3u);
        for (jj = 0; jj < flips; ++jj)
        {
            rng = rng * 1103515245u + 12345u;
            buf[ch->payload + 8 + (rng >> 8) % (ch->length - 8)] ^=
                (unsigned char)(1u << ((rng >> 3) & 7));
        }
        write_file(bad_path, buf, len);
        if (srnx_load(bad_path, &data))
            failures++;
        free_rinex_data(&data);
    }
    ok(1, "survived %d SOC2 mutations (%d rejected)", iterations, failures);
    free(orig);
    free(buf);
}

int main(void)
{
    struct rinex_data text, soc2, socd;
    const char *err;
    int res;

    write_rinex();
    err = rinex_load_file(rnx_path, &text);
    ok(err == NULL, "load synthetic RINEX (%s)", err ? err : "ok");
    if (err)
        BAIL_OUT("synthetic RINEX did not load");

    ok(convert("", soc2_path) == 0, "convert to SOC2");
    ok(convert("--socd", socd_path) == 0, "convert to SOCD with --socd");
    ok(convert("--chunk-digest=0 --file-digest=0", raw_path) == 0,
        "convert to SOC2 without digests");

    res = srnx_load(soc2_path, &soc2);
    ok(res == 0, "load SOC2 file (%s)", res ? soc2.error : "ok");
    ok(res == 0 && same_data(&text, &soc2), "SOC2 values, LLIs and SSIs match the text");
    res = srnx_load(socd_path, &socd);
    ok(res == 0, "load SOCD file (%s)", res ? socd.error : "ok");
    ok(res == 0 && same_data(&text, &socd), "SOCD values, LLIs and SSIs match the text");
    free_rinex_data(&soc2);
    free_rinex_data(&socd);

    /* The streaming SRNX parser must agree with the text. */
    {
        struct rinex_stream *stream = rinex_mmap_stream(soc2_path);
        struct rinex_data streamed;
        err = stream ? rinex_load(stream, &streamed) : "no stream";
        ok(err == NULL && same_data(&text, &streamed),
            "streaming parser reads SOC2 like the text (%s)", err ? err : "ok");
        if (!err)
            free_rinex_data(&streamed);
        if (stream)
            stream->destroy(stream);
    }

    check_modes();
    check_minor_version();
    fuzz_soc2(3000);

    free_rinex_data(&text);
    remove(rnx_path);
    remove(soc2_path);
    remove(socd_path);
    remove(raw_path);
    remove(bad_path);
    done_testing();
    return exit_status();
}
