/** test/crx_v2 - TAP harness for CRX v1 (Hatanaka RINEX v2) decompression.
 * Copyright 2025 Michael Poole.
 * SPDX-License-Identifier: MIT-Modern-Variant
 */

#include "rinex/rinex.h"
#include <stdlib.h>
#include <tap.h>

/* Generated from rnx2crx output of at012000.20o (first 3 epochs). */
#include "bin/at012000_20d.h"

/* Tests comparing CRX v1 decompression against known RINEX v2 values. */
void test_at012000_crx(void)
{
    struct rinex_stream *crx;
    struct rinex_parser *p;
    const char *msg;
    rinex_error_t err;
    unsigned int ofs, lim, ii;

    /* 2 tests: open */
    p = NULL;
    crx = rinex_buffer_stream(at012000_20d, at012000_20d_len);
    msg = rinex_open(&p, crx);
    ok(msg == NULL, "rinex_open() for CRX v1 succeeded");
    if (!p) BAIL_OUT();
    lim = p->n_obs[0];
    ok(lim == 20, "20 observables");

    /* 10 tests: first epoch metadata (same as rinex_v2 test) */
    err = p->read(p);
    ok(err == RINEX_SUCCESS, "CRX read() #1 succeeded");
    ok(p->epoch.yyyy_mm_dd == 20200718, "date is 2020-07-18");
    ok(p->epoch.hh_mm == 0, "time-of-day is 00:00");
    ok(p->epoch.flag == '0', "epoch flag is 0");
    ok(p->epoch.sec_e7 == 0, "seconds is 0.0000000");
    ok(p->epoch.n_sats == 29, "first epoch has 29 sats");
    ok(p->epoch.clock_offset == 0, "clock offset is zero");
    ok(p->sats[0].system == 'G' && p->sats[0].number == 13, "first sat is G13");
    ok(p->sats[1].system == 'R' && p->sats[1].number == 10, "second sat is R10");
    ok(p->sats[28].system == 'R' && p->sats[28].number == 1, "last sat is R01");

    /* 22 tests: first satellite (G13) observations */
    ofs = p->sats[0].obs_0;
    ok(p->obs[ofs+0] == 128250890432, "obs 1 is 128250890.432");
    ok(p->lli[ofs+0] == ' ', "first LLI is blank");
    ok(p->ssi[ofs+0] == '5', "first SSI is 5");
    ok(p->obs[ofs+1] == 99935750108, "obs 2 is 99935750.108");
    ok(p->obs[ofs+2] == 24405331298, "obs 3 is 24405331.298");
    ok(p->obs[ofs+3] == 24405326454, "obs 4 is 24405326.454");
    ok(p->obs[ofs+4] == 24405330632, "obs 5 is 24405330.632");
    ok(p->obs[ofs+5] == 35250, "obs 6 is 35.250");
    ok(p->obs[ofs+6] == 13750, "obs 7 is 13.750");
    for (ii = 7; ii < lim; ++ii)
        ok(p->obs[ofs+ii] == 0, "obs N is zero");

    /* 20 tests: second satellite (R10) observations */
    ofs = p->sats[1].obs_0;
    ok(p->obs[ofs+0] == 117123934241, "obs 2,1 is 117123934.241");
    ok(p->obs[ofs+1] == 0, "obs 2,2 is zero");
    ok(p->obs[ofs+2] == 21972148800, "obs 2,3 is 21972148.800");
    ok(p->obs[ofs+3] == 0, "obs 2,4 is zero");
    ok(p->obs[ofs+4] == 0, "obs 2,5 is zero");
    ok(p->obs[ofs+5] == 46000, "obs 2,6 is 46.000");
    for (ii = 6; ii < lim; ++ii)
        ok(p->obs[ofs+ii] == 0, "obs 2,N is zero");

    /* 8 tests: second epoch (delta decompression) */
    err = p->read(p);
    ok(err == RINEX_SUCCESS, "CRX read() #2 succeeded");
    ok(p->epoch.sec_e7 == 10000000, "epoch 2 seconds is 1.0000000");
    ok(p->epoch.n_sats == 29, "epoch 2 has 29 sats");
    ok(p->sats[0].system == 'G' && p->sats[0].number == 13, "epoch 2 first sat is G13");
    ofs = p->sats[0].obs_0;
    ok(p->obs[ofs+0] == 128253149648, "G13 epoch2 obs 1 is 128253149.648");
    ok(p->obs[ofs+1] == 99937510556, "G13 epoch2 obs 2 is 99937510.556");
    ok(p->ssi[ofs+0] == '5', "G13 epoch2 SSI is 5");
    ok(p->lli[ofs+0] == ' ', "G13 epoch2 LLI is blank");

    /* 2 tests: third epoch */
    err = p->read(p);
    ok(err == RINEX_SUCCESS, "CRX read() #3 succeeded");
    ok(p->epoch.sec_e7 == 20000000, "epoch 3 seconds is 2.0000000");

    p->destroy(p);
    crx->destroy(crx);
}

int main(int argc, char *argv[])
{
    plan(64);

    test_at012000_crx();

    done_testing(); (void)argc; (void)argv;
}
