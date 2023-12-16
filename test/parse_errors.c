/** parse_errors - TAP harness for RINEX v3 parsing in librinex.
 * Copyright 2023 Michael Poole.
 * SPDX-License-Identifier: MIT-Modern-Variant
 */

#include "rinex.h"
#include <stdlib.h>
#include <tap.h>

#define NUL_PAD "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"

/* Based on the first epoch from at012000.20o. */
const unsigned char parse_errors_v2[] =
    "     2.11           OBSERVATION DATA    M (MIXED)           RINEX VERSION / TYPE\n"
    "teqc  2019Feb25     NOAA/NOS/NGS/CORS   20200720 01:36:16UTCPGM / RUN BY / DATE\n"
    "     1     1                                                WAVELENGTH FACT L1/2\n"
    "    20    L1    L2    C1    P2    P1    S1    S2    C2    L5# / TYPES OF OBSERV\n"
    "          C5    S5    L6    C6    S6    L7    C7    S7    L8# / TYPES OF OBSERV\n"
    "          C8    S8                                          # / TYPES OF OBSERV\n"
    "     1.0000                                                 INTERVAL\n"
    "  2020     7    18     0     0    0.0000000     GPS         TIME OF FIRST OBS\n"
    "    18                                                      LEAP SECONDS\n"
    "                                                            END OF HEADER\n"
    " 20\n" /* first line of observation is truncated */
    " 20  7 18  0  0  2.0000000  0  1G13\n" /* valid observation */
    " 128250890.432 5  99935750.10842  24405331.298    24405326.454    24405330.632\n"
    "        32.250          13.750\n\n\n"
    " 20  7 18  0  0  3.0000000  0 29G13R10E12E01R03E26G16G11R02G08E31G271\n" /* invalid length */
    " 20  7 18  0  0  4.0000000  0  1G13\n"
    " 128250890.432 5  99935750.10842  24405331.298    24405326.454    24405330.632\n"
    "        33.250          13.750\n\n\n" /* valid observation */
    " 20  7 18  0  0  5.0000000  0  1G13\n"
    " 128250890.432 5  99935750.10842  24405331.298    24405326.454    24405330.632\n"
    "        34.250          a3.750\n\n\n" /* invalid input character */
    " 20  7 18  0  0  6.0000000  0  1G13\n"
    " 128250890.432 5  99935750.10842  24405331.298    24405326.454    24405330.632\n"
    "        35.250          13.750\n\n\n" /* valid observation */
    " 20  7 18  0  0  7.0000000  0  2G13R10\n"
    " 128250890.432 5  99935750.10842  24405331.298    24405326.454    24405330.632\n"
    "        36.250          13.750\n" /* EOF before end of observation data */
    NUL_PAD;

/* Based on the header of at012000.20o. */
const unsigned char parse_errors_v2_b[] =
    "     2.11           OBSERVATION DATA    M (MIXED)           RINEX VERSION / TYPE\n"
    "teqc  2019Feb25     NOAA/NOS/NGS/CORS   20200720 01:36:16UTCPGM / RUN BY / DATE\n"
    "     1     1                                                WAVELENGTH FACT L1/2\n"
    "    20    L1    L2    C1    P2    P1    S1    S2    C2    L5# / TYPES OF OBSERV\n"
    "          C5    S5    L6    C6    S6    L7    C7    S7    L8# / TYPES OF OBSERV\n"
    "          C8    S8                                          # / TYPES OF OBSERV\n"
    "     1.0000                                                 INTERVAL\n"
    "  2020     7    18     0     0    0.0000000     GPS         TIME OF FIRST OBS\n"
    "    18                                                      LEAP SECONDS\n"
    "                                                            END OF HEADER\n"
    " 20  7 18  0  0  0.0000000  2  2\n"
    "EOF without all lines of a non-observation record\n"
    NUL_PAD;

/* Based on the first epoch from ABMF00GLP_R_20201890000_01D_30S_MO.rnx. */
const unsigned char parse_errors_v3[] =
    "     3.04           OBSERVATION DATA    M                   RINEX VERSION / TYPE\n"
    "G   18 C1C L1C D1C S1C C1W S1W C2W L2W D2W S2W C2L L2L D2L  SYS / # / OBS TYPES\n"
    "       S2L C5Q L5Q D5Q S5Q                                  SYS / # / OBS TYPES\n"
    "E   20 C1C L1C D1C S1C C6C L6C D6C S6C C5Q L5Q D5Q S5Q C7Q  SYS / # / OBS TYPES\n"
    "       L7Q D7Q S7Q C8Q L8Q D8Q S8Q                          SYS / # / OBS TYPES\n"
    "S    8 C1C L1C D1C S1C C5I L5I D5I S5I                      SYS / # / OBS TYPES\n"
    "R   20 C1C L1C D1C S1C C1P L1P D1P S1P C2P L2P D2P S2P C2C  SYS / # / OBS TYPES\n"
    "       L2C D2C S2C C3Q L3Q D3Q S3Q                          SYS / # / OBS TYPES\n"
    "C   12 C2I L2I D2I S2I C7I L7I D7I S7I C6I L6I D6I S6I      SYS / # / OBS TYPES\n"
    "J   12 C1C L1C D1C S1C C2L L2L D2L S2L C5Q L5Q D5Q S5Q      SYS / # / OBS TYPES\n"
    "I    4 C5A L5A D5A S5A                                      SYS / # / OBS TYPES\n"
    "                                                            END OF HEADER\n"
    "> 2020 07 07 00 00  1.0000000  0 \n"   /* header line is truncated */
    "> 2020 07 07 00 00  2.0000000  0  1\n" /* valid line */
    "E04  28541439.844 5 149986288.81705      1175.101 5        35.700    "
    "28541441.120 6 121742127.93806       953.962 6        40.164    "
    "28541441.048 6 112002756.21106       877.350 6        39.658    "
    "28541440.314 6 114924561.30906       900.319 6        40.849    "
    "28541440.985 7 113463662.32907       888.852 7        43.227\n"
    "> 2020 07 07 00 00  3.0000000  0  1\n" /* excess text after observations */
    "C16  21517820.927 7 114944364.28507       720.881 7        42.512  "
    "  21517820.883 6 114944376.29006       720.886 6        41.654  "
    "  21517823.338 6  89401209.10306       560.568 6        41.833  "
    "excess junk at end of line\n"
    "> 2020 07 07 00 00  4.0000000  0  1\n" /* valid line*/
    "I01  28541439.844 5 149986288.81705      1175.101 5        35.700    28541441.120 6\n"
    NUL_PAD;

void
test_parse_errors_v2(void)
{
    struct rinex_stream *v2;
    struct rinex_parser *p;
    const char *msg;
    rinex_error_t err;

    p = NULL;
    v2 = rinex_buffer_stream(parse_errors_v2, sizeof parse_errors_v2 - sizeof NUL_PAD);
    msg = rinex_open(&p, v2);
    if (msg || !p)
        BAIL_OUT();

    /* 8 tests */
    err = p->read(p);
    cmp_ok(err, "==", RINEX_ERR_BAD_FORMAT, "rinex v2 read() #1 failed");
    err = p->read(p);
    ok(err == RINEX_SUCCESS && p->epoch.sec_e7 == 20000000, "rinex v2 read() #2 succeeded");
    err = p->read(p);
    cmp_ok(err, "==", RINEX_ERR_BAD_FORMAT, "rinex v2 read() #3 failed");
    err = p->read(p);
    ok(err == RINEX_SUCCESS && p->epoch.sec_e7 == 40000000, "rinex v2 read() #4 succeeded");
    err = p->read(p);
    cmp_ok(err, "==", RINEX_ERR_BAD_FORMAT, "rinex v2 read() #5 failed");
    err = p->read(p);
    ok(err == RINEX_SUCCESS && p->epoch.sec_e7 == 60000000, "rinex v2 read() #6 succeeded");
    err = p->read(p);
    cmp_ok(err, "==", RINEX_ERR_BAD_FORMAT, "rinex v2 read() #7 failed");
    err = p->read(p);
    cmp_ok(err, "==", RINEX_EOF, "rinex v2 read() after EOF is stable");
}

void test_parse_errors_v2_b(void)
{
    struct rinex_stream *v2;
    struct rinex_parser *p;
    const char *msg;
    rinex_error_t err;

    p = NULL;
    v2 = rinex_buffer_stream(parse_errors_v2_b, sizeof parse_errors_v2_b - sizeof NUL_PAD);
    msg = rinex_open(&p, v2);
    if (msg || !p)
        BAIL_OUT();

    /* 1 test */
    err = p->read(p);
    cmp_ok(err, "==", RINEX_ERR_BAD_FORMAT, "rinex v2_b read() past EOF failed");
}

void test_parse_errors_v3(void)
{
    struct rinex_stream *v3;
    struct rinex_parser *p;
    const char *msg;
    rinex_error_t err;

    p = NULL;
    v3 = rinex_buffer_stream(parse_errors_v3, sizeof parse_errors_v3 - sizeof NUL_PAD);
    msg = rinex_open(&p, v3);
    if (msg || !p)
        BAIL_OUT();

    /* 5 tests */
    err = p->read(p);
    cmp_ok(err, "==", RINEX_ERR_BAD_FORMAT, "rinex v3 read() #1 failed");
    err = p->read(p);
    ok(err == RINEX_SUCCESS && p->epoch.sec_e7 == 20000000, "rinex v3 read() #2 succeeded");
    err = p->read(p);
    cmp_ok(err, "==", RINEX_ERR_BAD_FORMAT, "rinex v3 read() #3 failed");
    err = p->read(p);
    ok(err == RINEX_SUCCESS && p->epoch.sec_e7 == 40000000, "rinex v3 read() #4 succeeded");
    err = p->read(p);
    cmp_ok(err, "==", RINEX_EOF, "rinex v3 read() after EOF is stable");
}

int main(int argc, char *argv[])
{
    plan(8+1+5);

    test_parse_errors_v2();
    test_parse_errors_v2_b();
    test_parse_errors_v3();

    done_testing(); (void)argc; (void)argv;
}
