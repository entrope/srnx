/** test/test_empty_records.c - TAP tests for records with no lines after
 * their epoch line: observation epochs with no satellites, and special
 * events with no records.
 * Copyright 2026 Michael Poole.
 * SPDX-License-Identifier: MIT-Modern-Variant
 *
 * The readers used to report end of file at such a record, silently
 * dropping the rest of the file (RINEX 2 and 3, CRINEX 1 epochs, and
 * CRINEX 3 events), because asking for zero lines looked like asking
 * for lines that were not there.
 */

#include "rinex/rinex.h"
#include <stdlib.h>
#include <string.h>
#include <tap.h>

static const char v3_rnx[] =
    "     3.04           OBSERVATION DATA    G                   RINEX VERSION / TYPE\n"
    "test                                                        PGM / RUN BY / DATE\n"
    "G    1 C1C                                                  SYS / # / OBS TYPES\n"
    "                                                            END OF HEADER\n"
    "> 2020 07 07 00 00  0.0000000  0  1\n"
    "G01  20000000.000\n"
    "> 2020 07 07 00 00 30.0000000  0  0\n"
    "> 2020 07 07 00 00 45.0000000  5  0\n"
    "> 2020 07 07 00 01  0.0000000  0  1\n"
    "G01  20000060.000\n";

static const char v2_rnx[] =
    "     2.11           OBSERVATION DATA    G (GPS)             RINEX VERSION / TYPE\n"
    "test                                                        PGM / RUN BY / DATE\n"
    "     1    C1                                                # / TYPES OF OBSERV\n"
    "                                                            END OF HEADER\n"
    " 20  7  7  0  0  0.0000000  0  1G01\n"
    "  20000000.000\n"
    " 20  7  7  0  0 30.0000000  0  0\n"
    " 20  7  7  0  0 45.0000000  5  0\n"
    " 20  7  7  0  1  0.0000000  0  1G01\n"
    "  20000060.000\n";

/* RNX2CRX 4.2.0 output for v3_rnx and v2_rnx; CRX2RNX restores them. */
static const char v3_crx[] =
    "3.0                 COMPACT RINEX FORMAT                    CRINEX VERS   / TYPE\n"
    "RNX2CRX ver.4.2.0                       10-Oct-26 01:27     CRINEX PROG / DATE\n"
    "     3.04           OBSERVATION DATA    G                   RINEX VERSION / TYPE\n"
    "test                                                        PGM / RUN BY / DATE\n"
    "G    1 C1C                                                  SYS / # / OBS TYPES\n"
    "                                                            END OF HEADER\n"
    "> 2020 07 07 00 00  0.0000000  0  1      G01\n"
    "\n"
    "3&20000000000 &&\n"
    "                   3              0      &&&\n"
    "\n"
    "> 2020 07 07 00 00 45.0000000  5  0\n"
    "> 2020 07 07 00 01  0.0000000  0  1      G01\n"
    "\n"
    "3&20000060000 &&\n";

static const char v2_crx[] =
    "1.0                 COMPACT RINEX FORMAT                    CRINEX VERS   / TYPE\n"
    "RNX2CRX ver.4.2.0                       10-Oct-26 01:27     CRINEX PROG / DATE\n"
    "     2.11           OBSERVATION DATA    G (GPS)             RINEX VERSION / TYPE\n"
    "test                                                        PGM / RUN BY / DATE\n"
    "     1    C1                                                # / TYPES OF OBSERV\n"
    "                                                            END OF HEADER\n"
    "&20  7  7  0  0  0.0000000  0  1G01\n"
    "\n"
    "3&20000000000\n"
    "                3              0&&&\n"
    "\n"
    "&20  7  7  0  0 45.0000000  5  0\n"
    "&20  7  7  0  1  0.0000000  0  1G01\n"
    "\n"
    "3&20000060000\n";

/** Read \a text and check that it holds: an epoch with G01, an epoch
 * with no satellites, a flag-5 event with no records, an epoch with
 * G01, then end of file.
 */
static void test_file(const char *name, const char *text)
{
    static const struct
    {
        char flag;
        int n_sats, hh_mm, sec_e7;
        int64_t obs;
    } want[] = {
        { '0', 1, 0, 0, 20000000000LL },
        { '0', 0, 0, 300000000, 0 },
        { '5', 0, 0, 450000000, 0 },
        { '0', 1, 1, 0, 20000060000LL },
    };
    struct rinex_stream *stream;
    struct rinex_parser *p = NULL;
    size_t len = strlen(text);
    char *buf = calloc(1, len + 80);
    rinex_error_t err = RINEX_SUCCESS;
    int ii, n_good = 0;

    memcpy(buf, text, len);
    stream = rinex_buffer_stream(buf, (int)len);
    if (rinex_open(&p, stream))
        BAIL_OUT("%s: rinex_open() failed", name);

    for (ii = 0; ii < 4; ++ii)
    {
        err = p->read(p);
        if (err == RINEX_SUCCESS && p->epoch.flag == want[ii].flag
            && p->epoch.n_sats == want[ii].n_sats
            && p->epoch.hh_mm == want[ii].hh_mm
            && p->epoch.sec_e7 == want[ii].sec_e7
            && (want[ii].n_sats == 0 || p->obs[p->sats[0].obs_0] == want[ii].obs))
        {
            ++n_good;
        }
        else
        {
            diag("%s: record %d: status %d, flag %c, %d sats (%s)", name, ii,
                err, p->epoch.flag, p->epoch.n_sats, p->errmsg);
        }
    }
    ok(n_good == 4, "%s: empty epoch and empty event are read", name);
    ok(p->read(p) == RINEX_EOF, "%s: then end of file", name);

    p->destroy(p);
    stream->destroy(stream);
    free(buf);
}

/** Check that rinex_parse_record() accepts a record with no lines. */
static void test_record(const char *name, const char *header,
    const char *record, char flag)
{
    struct rinex_parser *p = NULL;
    rinex_error_t err;

    if (rinex_open_records(&p, header, (int)strlen(header)))
        BAIL_OUT("%s: rinex_open_records() failed", name);
    err = rinex_parse_record(p, record, (int)strlen(record));
    ok(err == RINEX_SUCCESS && p->epoch.flag == flag && p->epoch.n_sats == 0,
        "%s: rinex_parse_record() accepts it (%s)", name,
        err == RINEX_SUCCESS ? "ok" : p->errmsg);
    p->destroy(p);
}

int main(void)
{
    plan(11);

    test_file("RINEX 3", v3_rnx);
    test_file("RINEX 2", v2_rnx);
    test_file("CRINEX 3", v3_crx);
    test_file("CRINEX 1", v2_crx);

    test_record("RINEX 3 empty epoch", v3_rnx,
        "> 2020 07 07 00 00 30.0000000  0  0\n", '0');
    test_record("RINEX 2 empty epoch", v2_rnx,
        " 20  7  7  0  0 30.0000000  0  0\n", '0');
    test_record("RINEX 3 empty event", v3_rnx,
        "> 2020 07 07 00 00 45.0000000  5  0\n", '5');

    done_testing();
}
