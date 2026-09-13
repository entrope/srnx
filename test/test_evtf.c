/** Complete exceptional epochs survive EVTF without entering SOCD. */
#include "rinex/rinex.h"
#include "rinex/rinex_load.h"
#include "rinex/srnx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tap.h>
#include "bin/at012000_20d.h"
#include "bin/abmf00glp_crx.h"

static void compare(const char *input, const char *output)
{
    struct rinex_stream *a = rinex_mmap_stream(input), *b = rinex_mmap_stream(output);
    struct rinex_parser *pa = NULL, *pb = NULL;
    int ra, rb, ii, jj, count = 0;
    if (!a || !b || rinex_open(&pa, a) || rinex_open(&pb, b))
        BAIL_OUT("cannot open comparison inputs");
    while ((ra = pa->read(pa)) > 0)
    {
        int equal;
        rb = pb->read(pb);
        equal = rb == ra && pa->epoch.flag == pb->epoch.flag
            && pa->epoch.yyyy_mm_dd == pb->epoch.yyyy_mm_dd
            && pa->epoch.hh_mm == pb->epoch.hh_mm
            && pa->epoch.sec_e7 == pb->epoch.sec_e7
            && pa->epoch.clock_offset == pb->epoch.clock_offset
            && pa->epoch.n_sats == pb->epoch.n_sats;
        ok(equal, "%s record %d: epoch metadata matches", output, count++);
        if (!equal)
        {
            diag("read %d/%d flag %c/%c date %d/%d time %d/%d sec %d/%d sats %d/%d clock %lld/%lld error %s",
                ra, rb, pa->epoch.flag, pb->epoch.flag, pa->epoch.yyyy_mm_dd, pb->epoch.yyyy_mm_dd,
                pa->epoch.hh_mm, pb->epoch.hh_mm, pa->epoch.sec_e7, pb->epoch.sec_e7,
                pa->epoch.n_sats, pb->epoch.n_sats, (long long)pa->epoch.clock_offset,
                (long long)pb->epoch.clock_offset, pb->errmsg);
            break;
        }
        if (pa->epoch.flag >= '2' && pa->epoch.flag <= '5')
        {
            ok(pa->buffer_len == pb->buffer_len
                && !memcmp(pa->buffer, pb->buffer, pa->buffer_len), "special event text matches");
            continue;
        }
        for (ii = 0; ii < pa->epoch.n_sats; ++ii)
        {
            int match = -1, n_obs = pa->n_obs[pa->sats[ii].system & 31];
            if (!n_obs) n_obs = pa->n_obs[0];
            for (jj = 0; jj < pb->epoch.n_sats; ++jj)
                if (pa->sats[ii].system == pb->sats[jj].system
                    && pa->sats[ii].number == pb->sats[jj].number) match = jj;
            if (match < 0) { equal = 0; break; }
            for (jj = 0; jj < n_obs; ++jj)
            {
                int x = pa->sats[ii].obs_0 + jj, y = pb->sats[match].obs_0 + jj;
                /* Ordinary SOCD omits missing fields; EVTF keeps their indicators. */
                if (pa->epoch.flag == '0'
                    && (pa->obs[x] == 0 || pa->obs[x] == INT64_MIN)
                    && pa->lli[x] == ' '
                    && (pb->obs[y] == 0 || pb->obs[y] == INT64_MIN)
                    && pb->lli[y] == ' ') continue;
                if (pa->obs[x] != pb->obs[y] || pa->lli[x] != pb->lli[y]
                    || pa->ssi[x] != pb->ssi[y]) equal = 0;
            }
        }
        ok(equal, "%s: satellite values and indicators match", output);
    }
    ok(ra == RINEX_EOF && pb->read(pb) == RINEX_EOF, "both inputs end at the same record");
    pa->destroy(pa); pb->destroy(pb); a->destroy(a); b->destroy(b);
}

static void roundtrip(const char *input, int expected_epochs, int expected_events)
{
    const char *srnx = "evtf_test.srnx", *rnx = "evtf_test_out.rnx";
    struct rinex_data data;
    char cmd[2048];
    int result;
    snprintf(cmd, sizeof cmd, "\"%s\" %s %s", RNX2SRNX_PATH, input, srnx);
    result = system(cmd);
    ok(result == 0, "convert %s to SRNX", input);
    if (result) BAIL_OUT("conversion failed");
    result = srnx_load(srnx, &data);
    ok(result == 0, "load SRNX columns and events");
    if (result) BAIL_OUT("SRNX load failed");
    ok(data.epoch_used == expected_epochs && data.event_used == expected_events,
        "only flag-0 epochs enter EPOC (%d); EVTF holds %d records",
        data.epoch_used, data.event_used);
    free_rinex_data(&data);
    compare(input, srnx);
    snprintf(cmd, sizeof cmd, "\"%s\" %s %s", SRNX2RNX_PATH, srnx, rnx);
    if (system(cmd)) BAIL_OUT("text export failed");
    compare(input, rnx);
    /* Loading through the streaming SRNX parser must preserve the same partition. */
    {
        struct rinex_stream *stream = rinex_mmap_stream(srnx);
        const char *err = rinex_load(stream, &data);
        ok(!err && data.epoch_used == expected_epochs && data.event_used == expected_events,
            "streaming SRNX whole-file load preserves EVTF partition");
        if (!err) free_rinex_data(&data);
        stream->destroy(stream);
    }
    snprintf(cmd, sizeof cmd, "\"%s\" --tuple=8 %s >evtf_search.out",
        RNX2SRNX_SEARCH_PATH, input);
    ok(system(cmd) == 0, "search accepts exceptional epochs");
    if (expected_epochs == 0)
    {
        snprintf(cmd, sizeof cmd, "\"%s\" - %s <%s", RNX2SRNX_PATH, srnx, input);
        ok(system(cmd) == 0, "event-only input also converts through stdin");
    }
    remove(srnx); remove(rnx); remove("evtf_search.out");
}

static void write_text(int version, int only_events)
{
    const int mixed[] = {1, 6, 0, 4, 6, 1, 0, 6};
    FILE *file = fopen("evtf_test.rnx", "w");
    int rec, sat, obs, flags = only_events ? 2 : 8;
    if (!file) BAIL_OUT("cannot write fixture");
    fprintf(file, "%9.2f           OBSERVATION DATA    G                   RINEX VERSION / TYPE\n",
        version == 2 ? 2.11 : version == 3 ? 3.04 : 4.00);
    fprintf(file, "%-60s%s\n", version == 2 ? "     6    C1    L1    D1    S1    C2    L2"
        : "G    6 C1C L1C D1C S1C C2W L2W",
        version == 2 ? "# / TYPES OF OBSERV" : "SYS / # / OBS TYPES");
    fprintf(file, "%-60sEND OF HEADER\n", "");
    for (rec = 0; rec < flags; ++rec)
    {
        int flag = mixed[rec], n_sats = flag == 4 ? 1 : 13;
        if (version == 2)
        {
            fprintf(file, " 20  7 18  0  0  %1d.0000000  %d%3d", rec / 4, flag, n_sats);
            if (flag == 4) fputc('\n', file);
            else
            {
                for (sat = 1; sat <= 12; ++sat) fprintf(file, "G%02d", sat);
                fprintf(file, "-0.000123456\n%32sG13\n", "");
            }
        }
        else
        {
            fprintf(file, "> 2020 07 18 00 00  %1d.0000000  %d%3d", rec / 4, flag, n_sats);
            if (flag != 4) fprintf(file, "      -0.123456789012");
            fputc('\n', file);
        }
        if (flag == 4)
        {
            fprintf(file, "%-60sCOMMENT\n", "event between exceptional epochs");
            continue;
        }
        for (sat = 1; sat <= n_sats; ++sat)
        {
            if (version != 2) fprintf(file, "G%02d", sat);
            for (obs = 0; obs < 6; ++obs)
            {
                if (obs == 4) fprintf(file, "%14s", "");
                else fprintf(file, "%14.3f", flag == 6 ? -3.125 : 28000000.125 + sat + obs);
                fprintf(file, "%c%c", flag == 6 || obs == 4 ? ' ' : '1', flag == 6 ? ' ' : '5');
                if (version == 2 && obs == 4) fputc('\n', file);
            }
            fputc('\n', file);
        }
    }
    fclose(file);
}

static void test_crx(unsigned char *bytes, size_t length, int version)
{
    char *copy = calloc(length + 1, 1), *body, *line;
    FILE *file;
    int flag;
    memcpy(copy, bytes, length);
    body = strstr(copy, "END OF HEADER");
    if (!body) BAIL_OUT("missing sample header");
    body = strchr(body, '\n') + 1;
    if (version == 3)
    {
        /* The tracked sample ends with the next epoch header, without its data. */
        char *last = copy + length - 1;
        while (last > body && last[-1] != '\n') --last;
        length = last - copy;
        copy[length] = '\0';
    }
    for (flag = 1; flag <= 6; flag += 5)
    {
        /* Keep all delta headers unchanged: the altered initial flag persists. */
        line = body;
        line[version == 2 ? 28 : 31] = '0' + flag;
        file = fopen("evtf_test.crx", "w");
        if (!file) BAIL_OUT("cannot write CRINEX fixture");
        fwrite(copy, 1, length, file);
        fclose(file);
        roundtrip("evtf_test.crx", 0, version == 2 ? 3 : 4);
    }
    free(copy);
    remove("evtf_test.crx");
}

int main(void)
{
    int version, only;
    for (version = 2; version <= 4; ++version)
        for (only = 0; only <= 1; ++only)
        {
            write_text(version, only);
            roundtrip("evtf_test.rnx", only ? 0 : 2, only ? 2 : 6);
        }
    remove("evtf_test.rnx");
    test_crx(at012000_20d, at012000_20d_len, 2);
    test_crx(abmf_crx, abmf_crx_len, 3);
    done_testing();
    return exit_status();
}
