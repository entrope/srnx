/** test/test_event_records.c - TAP tests for RINEX 3 special event
 * records, read from RINEX 3 and CRINEX 3 files.
 * Copyright 2026 Michael Poole.
 * SPDX-License-Identifier: MIT-Modern-Variant
 *
 *  - Special events (flags 2-5) may leave the epoch date and time blank;
 *    the RINEX 3 and CRINEX 3 readers used to reject such events.
 *  - After reading an event, rinex_parser.buffer must hold the epoch
 *    line and then the event's lines; the CRINEX 3 reader used to leave
 *    out the epoch line.
 */

#include "rinex/rinex.h"
#include <stdlib.h>
#include <string.h>
#include <tap.h>

#define HEADER \
    "     3.04           OBSERVATION DATA    G                   RINEX VERSION / TYPE\n" \
    "test                                                        PGM / RUN BY / DATE\n" \
    "G    1 C1C                                                  SYS / # / OBS TYPES\n" \
    "                                                            END OF HEADER\n"
#define UNTIMED_EVENT \
    ">                              4  1\n" \
    "an untimed event                                            COMMENT\n"
#define TIMED_EVENT \
    "> 2020 07 07 00 00 40.0000000  4  1\n" \
    "a timed event                                               COMMENT\n"
#define EMPTY_EVENT \
    "> 2020 07 07 00 00 45.0000000  5  0\n"

static const char ev3_rnx[] =
    HEADER
    "> 2020 07 07 00 00  0.0000000  0  1\n"
    "G01  20000000.000\n"
    UNTIMED_EVENT
    "> 2020 07 07 00 00 30.0000000  0  1\n"
    "G01  20000030.000\n"
    TIMED_EVENT
    EMPTY_EVENT
    "> 2020 07 07 00 01  0.0000000  0  1\n"
    "G01  20000060.000\n";

/* RNX2CRX 4.2.0 output for ev3_rnx; CRX2RNX restores it exactly. */
static const char ev3_crx[] =
    "3.0                 COMPACT RINEX FORMAT                    CRINEX VERS   / TYPE\n"
    "RNX2CRX ver.4.2.0                       10-Oct-26 01:36     CRINEX PROG / DATE\n"
    HEADER
    "> 2020 07 07 00 00  0.0000000  0  1      G01\n"
    "\n"
    "3&20000000000 &&\n"
    UNTIMED_EVENT
    "> 2020 07 07 00 00 30.0000000  0  1      G01\n"
    "\n"
    "3&20000030000 &&\n"
    TIMED_EVENT
    EMPTY_EVENT
    "> 2020 07 07 00 01  0.0000000  0  1      G01\n"
    "\n"
    "3&20000060000 &&\n";

/** Open a parser on a padded copy of \a text; returns the copy. */
static char *open_text(const char *text, struct rinex_stream **p_stream,
    struct rinex_parser **p_parser)
{
    size_t len = strlen(text);
    char *buf = calloc(1, len + 80);

    memcpy(buf, text, len);
    *p_stream = rinex_buffer_stream(buf, (int)len);
    *p_parser = NULL;
    if (rinex_open(p_parser, *p_stream))
        BAIL_OUT("rinex_open() failed");
    return buf;
}

/** Check that the current record of \a p is an event whose buffer holds
 * exactly \a text.
 */
static int is_event(const struct rinex_parser *p, rinex_error_t err,
    const char *text)
{
    return err == RINEX_SUCCESS && p->epoch.flag >= '2'
        && p->epoch.flag <= '5' && p->buffer_len == (int)strlen(text)
        && !memcmp(p->buffer, text, p->buffer_len);
}

static void test_file(const char *name, const char *text)
{
    struct rinex_stream *stream;
    struct rinex_parser *p;
    rinex_error_t err;
    char *buf = open_text(text, &stream, &p);

    err = p->read(p);
    ok(err == RINEX_SUCCESS && p->epoch.flag == '0' && p->epoch.n_sats == 1,
        "%s: first epoch", name);

    err = p->read(p);
    ok(is_event(p, err, UNTIMED_EVENT) && p->epoch.flag == '4'
        && p->epoch.n_sats == 1 && p->epoch.yyyy_mm_dd == 0
        && p->epoch.hh_mm == 0 && p->epoch.sec_e7 == 0,
        "%s: untimed event, with its epoch line and a zero time (%s)",
        name, err == RINEX_SUCCESS ? "ok" : p->errmsg);

    err = p->read(p);
    ok(err == RINEX_SUCCESS && p->epoch.flag == '0'
        && p->epoch.sec_e7 == 300000000 && p->obs[0] == 20000030000LL,
        "%s: epoch after the untimed event", name);

    err = p->read(p);
    ok(is_event(p, err, TIMED_EVENT) && p->epoch.yyyy_mm_dd == 20200707
        && p->epoch.sec_e7 == 400000000,
        "%s: timed event, with its epoch line", name);

    err = p->read(p);
    ok(is_event(p, err, EMPTY_EVENT) && p->epoch.flag == '5'
        && p->epoch.n_sats == 0,
        "%s: event with no lines, with its epoch line", name);

    err = p->read(p);
    ok(err == RINEX_SUCCESS && p->epoch.flag == '0' && p->epoch.hh_mm == 1
        && p->obs[0] == 20000060000LL, "%s: last epoch", name);
    ok(p->read(p) == RINEX_EOF, "%s: then end of file", name);

    p->destroy(p);
    stream->destroy(stream);
    free(buf);
}

/** Check that a file whose only record is \a record is rejected. */
static void test_bad(const char *name, const char *record)
{
    struct rinex_stream *stream;
    struct rinex_parser *p;
    char *text = malloc(sizeof HEADER + strlen(record));
    char *buf;

    strcpy(text, HEADER);
    strcat(text, record);
    buf = open_text(text, &stream, &p);
    ok(p->read(p) == RINEX_ERR_BAD_FORMAT, "%s is rejected (%s)", name,
        p->errmsg);
    p->destroy(p);
    stream->destroy(stream);
    free(buf);
    free(text);
}

int main(void)
{
    struct rinex_parser *p = NULL;

    plan(17);

    test_file("RINEX 3", ev3_rnx);
    test_file("CRINEX 3", ev3_crx);

    /* Only events may omit the time, and then only all of it. */
    test_bad("observation epoch without a time",
        ">                              0  1\nG01  20000000.000\n");
    test_bad("event with a partly blank time",
        "> 2020 07 07 00 00            4  1\n"
        "a comment                                                   COMMENT\n");

    if (rinex_open_records(&p, HEADER, (int)strlen(HEADER)))
        BAIL_OUT("rinex_open_records() failed");
    ok(rinex_parse_record(p, UNTIMED_EVENT, (int)strlen(UNTIMED_EVENT))
        == RINEX_SUCCESS && is_event(p, RINEX_SUCCESS, UNTIMED_EVENT),
        "rinex_parse_record() accepts an untimed event");
    p->destroy(p);

    done_testing();
}
