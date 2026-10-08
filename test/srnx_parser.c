/** test/srnx_parser.c - Lockstep comparison of the text and SRNX
 *   rinex_parser backends.
 * Copyright 2026 Michael Poole.
 * SPDX-License-Identifier: MIT-Modern-Variant
 *
 * Converts a RINEX fixture to SRNX with rnx2srnx, then walks the text
 * parser (rinex_open() over the .rnx) and the SRNX parser (rinex_open()
 * over the resulting .srnx) in lockstep, asserting identical epochs,
 * satellite sets, and observation/LLI/SSI values.
 *
 * Satellites are matched by name rather than array position, since the
 * SRNX backend lists satellites in a different (alphabetical) order
 * than the original file.  Missing-observation comparisons use the
 * documented rinex.h contract (missing = obs 0 with blank LLI, or a
 * value outside the legal range) rather than requiring byte-identical
 * slots, since e.g. a 0.000 field with a non-blank SSI is legitimately
 * dropped by the SRNX conversion.
 */

#include "rinex/rinex.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tap.h>

#ifndef RNX2SRNX_PATH
# define RNX2SRNX_PATH "./rnx2srnx"
#endif
#ifndef FIXTURE_PATH
# define FIXTURE_PATH "fixture_v3.rnx"
#endif

static int find_sat(struct rinex_parser *p, char sys, int num)
{
    int ii;

    for (ii = 0; ii < p->epoch.n_sats; ++ii)
    {
        if (p->sats[ii].system == sys && p->sats[ii].number == num)
        {
            return ii;
        }
    }
    return -1;
}

static int is_missing(int64_t obs, char lli)
{
    return (obs < RINEX_MIN_OBS) || (obs == 0 && lli == ' ');
}

int main(int argc, char *argv[])
{
    char cmd[1024];
    const char *srnx_path = "test_srnx_parser.srnx";
    struct rinex_stream *s1, *s2;
    struct rinex_parser *p1 = NULL, *p2 = NULL;
    const char *err;
    int res, ii, epoch_no;

    /* An optional argument passes extra options (such as --socd) to rnx2srnx. */
    snprintf(cmd, sizeof cmd, "%s %s %s %s", RNX2SRNX_PATH,
        argc > 1 ? argv[1] : "", FIXTURE_PATH, srnx_path);
    res = system(cmd);
    ok(res == 0, "rnx2srnx converted %s", FIXTURE_PATH);
    if (res != 0)
    {
        BAIL_OUT("rnx2srnx failed");
    }

    s1 = rinex_mmap_stream(FIXTURE_PATH);
    err = rinex_open(&p1, s1);
    ok(err == NULL, "rinex_open on text fixture");
    if (!p1)
    {
        BAIL_OUT("could not open text fixture");
    }

    s2 = rinex_mmap_stream(srnx_path);
    err = rinex_open(&p2, s2);
    ok(err == NULL, "rinex_open on SRNX fixture (%s)", err ? err : "");
    if (!p2)
    {
        BAIL_OUT("could not open SRNX fixture");
    }

    {
        int n_obs_match = 1;

        for (ii = 0; ii < 32; ++ii)
        {
            if (p1->n_obs[ii] != p2->n_obs[ii])
            {
                diag("n_obs[%d] differs: %d vs %d", ii, p1->n_obs[ii], p2->n_obs[ii]);
                n_obs_match = 0;
            }
        }
        ok(n_obs_match, "n_obs[] matches between text and SRNX parsers");
    }

    for (epoch_no = 0; ; )
    {
        int r1 = p1->read(p1);
        int r2 = p2->read(p2);
        int si;

        ok(r1 == r2, "epoch %d: read() result matches (%d vs %d)", epoch_no, r1, r2);
        if (r1 != r2 || r1 <= 0)
        {
            break;
        }
        ++epoch_no;

        {
            int header_ok = (p1->epoch.flag == p2->epoch.flag)
                && (p1->epoch.yyyy_mm_dd == p2->epoch.yyyy_mm_dd)
                && (p1->epoch.hh_mm == p2->epoch.hh_mm)
                && (p1->epoch.sec_e7 == p2->epoch.sec_e7)
                && (p1->epoch.n_sats == p2->epoch.n_sats);

            if (!header_ok)
            {
                diag("epoch %d: flag %c/%c date %d/%d time %d/%d sec %d/%d n_sats %d/%d",
                    epoch_no, p1->epoch.flag, p2->epoch.flag,
                    p1->epoch.yyyy_mm_dd, p2->epoch.yyyy_mm_dd,
                    p1->epoch.hh_mm, p2->epoch.hh_mm,
                    p1->epoch.sec_e7, p2->epoch.sec_e7,
                    p1->epoch.n_sats, p2->epoch.n_sats);
            }
            ok(header_ok, "epoch %d: header matches", epoch_no);
        }

        if (p1->epoch.flag != '0' && p1->epoch.flag != '1' && p1->epoch.flag != '6')
        {
            /* Special event: header already compared above; SRNX
             * cannot always reconstruct the original event text (see
             * BUGS.md's note on rnx_read_v34()'s pre-existing
             * header-line bug), so the raw text is not compared here.
             */
            continue;
        }

        {
            int sats_ok = 1, obs_ok = 1;

            for (si = 0; si < p1->epoch.n_sats; ++si)
            {
                char sys = p1->sats[si].system;
                int num = p1->sats[si].number;
                int sj = find_sat(p2, sys, num);
                int n_obs, oi, base1, base2;

                if (sj < 0)
                {
                    diag("epoch %d: satellite %c%02d missing from SRNX parser",
                        epoch_no, sys, num);
                    sats_ok = 0;
                    continue;
                }

                n_obs = p1->n_obs[sys & 31];
                base1 = p1->sats[si].obs_0;
                base2 = p2->sats[sj].obs_0;
                for (oi = 0; oi < n_obs; ++oi)
                {
                    int64_t v1 = p1->obs[base1 + oi], v2 = p2->obs[base2 + oi];
                    char l1 = p1->lli[base1 + oi], l2 = p2->lli[base2 + oi];
                    char ss1 = p1->ssi[base1 + oi], ss2 = p2->ssi[base2 + oi];

                    if (is_missing(v1, l1) && is_missing(v2, l2))
                    {
                        continue;
                    }
                    if (v1 != v2 || l1 != l2 || ss1 != ss2)
                    {
                        diag("epoch %d sat %c%02d obs[%d]: %lld(lli='%c',ssi='%c') vs "
                            "%lld(lli='%c',ssi='%c')",
                            epoch_no, sys, num, oi,
                            (long long)v1, l1, ss1, (long long)v2, l2, ss2);
                        obs_ok = 0;
                    }
                }
            }
            ok(sats_ok, "epoch %d: satellite set matches", epoch_no);
            ok(obs_ok, "epoch %d: observation/LLI/SSI values match", epoch_no);
        }
    }

    ok(epoch_no > 0, "at least one epoch was compared");

    p1->destroy(p1);
    p2->destroy(p2);
    s1->destroy(s1);
    s2->destroy(s2);
    remove(srnx_path);

    done_testing();
    return exit_status();
}
