/** test/test_record.c - TAP tests for rinex_open_records() and
 * rinex_parse_record().
 * Copyright 2026 Michael Poole.
 * SPDX-License-Identifier: MIT-Modern-Variant
 */

#include "rinex/rinex.h"
#include <stdlib.h>
#include <string.h>
#include <tap.h>

#include "bin/abmf00glp_crx.h"

static const char v3_header[] =
    "     3.04           OBSERVATION DATA    M                   RINEX VERSION / TYPE\n"
    "test                                                        PGM / RUN BY / DATE\n"
    "G    2 C1C L1C                                              SYS / # / OBS TYPES\n"
    "E    1 C1X                                                  SYS / # / OBS TYPES\n"
    "                                                            END OF HEADER\n";

static const char v2_header[] =
    "     2.11           OBSERVATION DATA    G (GPS)             RINEX VERSION / TYPE\n"
    "test                                                        PGM / RUN BY / DATE\n"
    "     7    C1    L1    L2    P2    S1    S2    D1            # / TYPES OF OBSERV\n"
    "                                                            END OF HEADER\n";

/** Parse \a text (NUL-terminated) from an exactly-sized heap copy, so
 * that reading past its end would be caught by a memory checker.
 */
static rinex_error_t parse(struct rinex_parser *p, const char *text)
{
    size_t len = strlen(text);
    char *copy = malloc(len);
    rinex_error_t res;

    memcpy(copy, text, len);
    res = rinex_parse_record(p, copy, (int)len);
    free(copy);
    return res;
}

/** Value of observation \a idx of the \a sat'th satellite in \a p. */
static int64_t obs_of(const struct rinex_parser *p, int sat, int idx)
{
    return p->obs[p->sats[sat].obs_0 + idx];
}

static void test_v3(void)
{
    struct rinex_parser *p = NULL;
    const char *err;

    err = rinex_open_records(&p, v3_header, sizeof v3_header - 1);
    ok(err == NULL && p != NULL, "v3: rinex_open_records succeeds");
    if (!p)
        BAIL_OUT("cannot continue without a record parser");
    ok(p->buffer_len == (int)sizeof v3_header - 1
        && !memcmp(p->buffer, v3_header, p->buffer_len),
        "v3: parser buffer holds the header");
    ok(p->n_obs['G' & 31] == 2 && p->n_obs['E' & 31] == 1,
        "v3: observation counts come from the header");

    ok(parse(p, "> 2020 07 07 00 00 30.0000000  1  2\n"
        "G01  20000030.000 7 105000030.00017\n"
        "E05  23000000.000\n") == RINEX_SUCCESS,
        "v3: flag-1 record parses");
    ok(p->epoch.flag == '1' && p->epoch.n_sats == 2
        && p->epoch.yyyy_mm_dd == 20200707 && p->epoch.sec_e7 == 300000000,
        "v3: flag-1 epoch fields");
    ok(p->sats[0].system == 'G' && p->sats[0].number == 1
        && obs_of(p, 0, 0) == 20000030000LL
        && obs_of(p, 0, 1) == 105000030000LL
        && p->lli[p->sats[0].obs_0 + 1] == '1'
        && p->ssi[p->sats[0].obs_0 + 1] == '7'
        && p->sats[1].system == 'E' && obs_of(p, 1, 0) == 23000000000LL,
        "v3: flag-1 observations");

    ok(parse(p, "> 2020 07 07 00 01  0.0000000  6  1       0.000000123456\n"
        "G02  20000060.000 7\n") == RINEX_SUCCESS
        && p->epoch.flag == '6' && p->epoch.n_sats == 1
        && p->epoch.clock_offset == 123456
        && p->sats[0].number == 2 && obs_of(p, 0, 0) == 20000060000LL,
        "v3: parser is reusable for a flag-6 record with clock offset");

    ok(parse(p, "> 2020 07 07 00 01 30.0000000  0  1\n"
        "G03  20000090.000\n") == RINEX_SUCCESS
        && p->epoch.flag == '0' && p->sats[0].number == 3,
        "v3: flag-0 record parses");

    ok(parse(p, "> 2020 07 07 00 02  0.0000000  4  1\n"
        "a comment                                                   COMMENT\n")
        == RINEX_SUCCESS && p->epoch.flag == '4'
        && p->buffer_len > 0 && !memcmp(p->buffer + p->buffer_len - 8, "COMMENT\n", 8),
        "v3: special event record parses into the buffer");

    ok(parse(p, "> 2020 07 07 00 02 30.0000000  1  2\n"
        "G01  20000030.000\n") != RINEX_SUCCESS
        && strstr(p->errmsg, "incomplete") != NULL,
        "v3: incomplete record is rejected");
    ok(parse(p, "> 2020 07 07 00 03  0.0000000  1  1\n"
        "G01  20000030.000\n"
        "> 2020 07 07 00 03 30.0000000  0  1\n"
        "G01  20000030.000\n") != RINEX_SUCCESS
        && strstr(p->errmsg, "extra text") != NULL,
        "v3: text after the record is rejected");
    ok(rinex_parse_record(p, "", 0) != RINEX_SUCCESS,
        "v3: empty record is rejected");

    /* Short records in exactly-sized buffers: error messages quote the
     * record, and must not read past its end. */
    {
        static const char *const shorts[] = { ">", "> 2020 07", "G01" };
        char *buf;
        int ii, n_ok = 0;

        for (ii = 0; ii < 3; ++ii)
        {
            size_t len = strlen(shorts[ii]);

            buf = malloc(len);
            memcpy(buf, shorts[ii], len);
            if (rinex_parse_record(p, buf, (int)len) != RINEX_SUCCESS
                && strstr(p->errmsg, "incomplete record: ")
                && !strcmp(strstr(p->errmsg, ": ") + 2, shorts[ii]))
            {
                ++n_ok;
            }
            free(buf);
        }
        ok(n_ok == 3, "v3: short unterminated records are rejected and"
            " quoted exactly");
    }

    /* Quoting stops at the end of the first line. */
    ok(parse(p, "> 2020 07 07 00 04  0.0000000  1  1\n"
        "G01  20000030.000\n"
        "G02\n") != RINEX_SUCCESS
        && !strcmp(strstr(p->errmsg, "extra text after record: ") + 25, "G02"),
        "v3: extra text is quoted up to its line break");

    p->destroy(p);
}

static void test_v2(void)
{
    struct rinex_parser *p = NULL;
    const char *err;

    err = rinex_open_records(&p, v2_header, sizeof v2_header - 1);
    ok(err == NULL && p != NULL, "v2: rinex_open_records succeeds");
    if (!p)
        return;

    /* Seven observations wrap onto a second line per satellite. */
    ok(parse(p, " 20  7  7  0  0 30.0000000  1  2G01G02\n"
        "  20000030.000 7 105000030.00017  81000030.000 6  20000031.000 7"
        "        45.000  \n"
        "        38.000        1234.500  \n"
        "  21000030.000   110000030.000\n"
        "\n") == RINEX_SUCCESS,
        "v2: flag-1 record with continuation lines parses");
    ok(p->epoch.flag == '1' && p->epoch.n_sats == 2
        && p->epoch.yyyy_mm_dd == 20200707
        && obs_of(p, 0, 0) == 20000030000LL && obs_of(p, 0, 4) == 45000
        && obs_of(p, 0, 5) == 38000 && obs_of(p, 0, 6) == 1234500
        && p->sats[1].number == 2 && obs_of(p, 1, 1) == 110000030000LL,
        "v2: observations on continuation lines");

    p->destroy(p);
}

static void test_open(void)
{
    struct rinex_parser *p = NULL, *other = NULL;
    struct rinex_stream *stream;
    unsigned int hdr_len;
    const char *end, *err;
    char *padded;

    /* A CRINEX header: the CRINEX lines are skipped. */
    end = strstr((const char *)abmf_crx, "END OF HEADER\n");
    hdr_len = (unsigned int)(end + 14 - (const char *)abmf_crx);
    err = rinex_open_records(&p, (const char *)abmf_crx, (int)hdr_len);
    ok(err == NULL && p != NULL
        && !memcmp(p->buffer + 60, "RINEX VERSION / TYPE", 20)
        && p->n_obs['G' & 31] == 18,
        "CRINEX header: CRINEX lines are skipped");
    if (p)
        p->destroy(p);
    p = NULL;

    err = rinex_open_records(&p, "not a header\n", 13);
    ok(err != NULL && p == NULL, "header without RINEX VERSION / TYPE fails");

    /* A parser from rinex_open() is not a record parser. */
    padded = calloc(1, sizeof v3_header + 200);
    memcpy(padded, v3_header, sizeof v3_header - 1);
    stream = rinex_buffer_stream(padded, sizeof v3_header - 1);
    err = rinex_open(&other, stream);
    ok(err == NULL && other
        && rinex_parse_record(other, "> 2020 07 07 00 00  0.0000000  0  0\n", 36)
            != RINEX_SUCCESS
        && strstr(other->errmsg, "rinex_open_records") != NULL,
        "rinex_parse_record rejects a parser from rinex_open()");
    if (other)
        other->destroy(other);
    stream->destroy(stream);
    free(padded);
}

int main(void)
{
    plan(20);
    test_v3();
    test_v2();
    test_open();
    done_testing();
}
