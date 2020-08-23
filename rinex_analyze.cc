/** rinex_analyze.c - RINEX observation file analysis utility.
 * Copyright 2020 Michael Poole.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "driver.h"
#include <cstdio>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>
#include <x86intrin.h>

/* Discussion of the hash function:
 * rinex_signal_t has sv[4] and obs[4] fields.  sv[3] and obs[3] == 0.
 * sv[0] identifies the satellite system, one of GRSECJI, with blanks
 * synonymous with G (for RINEX 2.xx single-system observable files).
 * sv[1] and sv[2] are digits, most often 01 through 36 or so.
 *
 * For RINEX 2.xx, obs[0] is one of CLDS with obs[1] 125678, or
 * obs[0,1] is P1 or P2; sv[0] is restricted to GRSE.
 * For RINEX 3.xx, obs[0] is one of CLDS, obs[1] is one of 1-9, and
 * obs[2] is one of ABCDEILMNPQSWXYZ.
 *
 * In binary, ASCII digits are 0011_wxyz where wxyz is the binary
 * representation of the digit's value.  Capital letters are 010v_wxyz,
 * starting at 0100_0001 (A) and ending with 0101_1010 (Z).  We want a
 * hash function that evenly distributes the identifers' bits across the
 * low-order bits of the output.
 *
 * The CRC-32C instruction is on the Pareto frontier of distribution
 * quality and speed, and it is dead simple, so use it for now.
 *
 * For remapping considerations:
 * ' ' = 0010_0000, 'C' = 0100_0011, 'E' = 0100_0101, 'G' = 0100_0111,
 * 'I' = 0100_1001, 'J' = 0100_1010, 'R' = 0101_0010, 'S' = 0101_0011,
 * so the four LSBS only collide for 'C' and 'S'.  This can be resolved
 * by shifting the bit with value 16 right by four positions, or by
 * considering the five LSBs.
 */

template<>
struct std::hash<rinex_signal_t>
{
    std::size_t operator()(rinex_signal_t sig) const
    {
        return _mm_crc32_u64(~uint64_t(0), sig.u64);
    }
};

/// signal_run represents a single set of observations, from a single
/// satellite number/observation code pair, over a contiguous span
/// (run) of epochs from the input file.
struct signal_run
{
    /// base_epoch is the index of the last epoch in this run.
    size_t base_epoch;

    /// obs holds the parsed observation values for the run.
    std::vector<int64_t> obs;

    /// lli holds the loss-of-lock indicators for the run.
    std::vector<char> lli;

    /// ssi holds the signal strength indicators for the run.
    std::vector<char> ssi;

    /// next points to the previous (older) data for this signal.
    std::unique_ptr<signal_run> next;
};

static int l_ubase128(uint64_t val)
{
    if (!(val >> 7)) return 1;
    if (!(val >> 14)) return 2;
    if (!(val >> 21)) return 3;
    if (!(val >> 28)) return 4;
    if (!(val >> 35)) return 5;
    if (!(val >> 42)) return 6;
    if (!(val >> 49)) return 7;
    printf(" Bueller! "); // nothing so large is ever expected
    if (!(val >> 56)) return 8;
    if (!(val >> 63)) return 9;
    return 10;
}

static int l_sbase128(int64_t val)
{
    return l_ubase128((val << 1) ^ (val >> 63));
}

[[gnu::noinline]]
void analyze_obs(const std::vector<int64_t> &obs, int &l0,
    int &l1, int &l2, int &l3, int &l4, int &l5)
{
    int64_t d[6], p[5];
    int tmp;

    d[0] = obs[0];
    l0 = l1 = l2 = l3 = l4 = l5 = l_sbase128(d[0]);

    if (obs.size() < 2) return;
    p[0] = d[0], d[0] = obs[1];
    d[1] = d[0] - p[0];
    l0 += l_sbase128(d[0]);
    tmp = l_sbase128(d[1]);
    l1 += tmp; l2 += tmp; l3 += tmp; l4 += tmp; l5 += tmp;

    if (obs.size() < 3) return;
    p[0] = d[0], d[0] = obs[2];
    p[1] = d[1], d[1] = d[0] - p[0];
    d[2] = d[1] - p[1];
    l0 += l_sbase128(d[0]);
    l1 += l_sbase128(d[1]);
    tmp = l_sbase128(d[1]);
    l2 += tmp; l3 += tmp; l4 += tmp; l5 += tmp;

    if (obs.size() < 4) return;
    p[0] = d[0], d[0] = obs[3];
    p[1] = d[1], d[1] = d[0] - p[0];
    p[2] = d[2], d[2] = d[1] - p[1];
    d[3] = d[2] - p[2];
    l0 += l_sbase128(d[0]);
    l1 += l_sbase128(d[1]);
    l2 += l_sbase128(d[2]);
    tmp = l_sbase128(d[3]);
    l3 += tmp; l4 += tmp; l5 += tmp;

    if (obs.size() < 5) return;
    p[0] = d[0], d[0] = obs[4];
    p[1] = d[1], d[1] = d[0] - p[0];
    p[2] = d[2], d[2] = d[1] - p[1];
    p[3] = d[3], d[3] = d[2] - p[2];
    d[4] = d[3] - p[3];
    l0 += l_sbase128(d[0]);
    l1 += l_sbase128(d[1]);
    l2 += l_sbase128(d[2]);
    l3 += l_sbase128(d[3]);
    tmp = l_sbase128(d[4]);
    l4 += tmp; l5 += tmp;

    for (size_t ii = 5; ii < obs.size(); ++ii)
    {
        p[0] = d[0], d[0] = obs[ii];
        p[1] = d[1], d[1] = d[0] - p[0];
        p[2] = d[2], d[2] = d[1] - p[1];
        p[3] = d[3], d[3] = d[2] - p[2];
        p[4] = d[4], d[4] = d[3] - p[3];
        d[5] = d[4] - p[4];

        l0 += l_sbase128(d[0]);
        l1 += l_sbase128(d[1]);
        l2 += l_sbase128(d[2]);
        l3 += l_sbase128(d[3]);
        l4 += l_sbase128(d[4]);
        l5 += l_sbase128(d[5]);
    }
}

[[gnu::noinline]]
int analyze_rle(const std::vector<char> &v)
{
    size_t ii = 0, start = 0;
    int len = 0;
    char curr = v[0];

    for (ii = 1; ii < v.size(); ++ii)
    {
        if (v[ii] != curr)
        {
            len += 1 + l_ubase128(ii - start - 1);
            start = ii;
            curr = v[ii];
        }
    }
    len += 1 + l_ubase128(ii - start - 1);

    return len;
}

void process_file(struct rinex_parser *p, const char filename[])
{
    std::vector<rinex_epoch> epochs;
    std::unordered_map<rinex_signal_t, std::unique_ptr<signal_run>> sigs;
    int64_t grand_total, n_runs = 0;
    int res;

    // Need to save the header.
    grand_total = p->buffer_len;

    // Read the data into memory.
    while ((res = p->read(p)) > 0)
    {
        if (p->epoch.flag != '0')
        {
            printf("Warning: epoch flag %d for %d %d %d\n",
                p->epoch.flag, p->epoch.yyyy_mm_dd, p->epoch.hh_mm,
                p->epoch.sec_e7);
            continue;
        }

        const size_t idx = epochs.size();
        epochs.push_back(p->epoch);

        for (int ii = 0; ii < p->signal_len; ++ii)
        {
            auto &s_ptr = sigs[p->signal[ii]];
            if (!s_ptr || idx > s_ptr->base_epoch + s_ptr->obs.size())
            {
                auto new_run = std::make_unique<signal_run>();
                new_run->base_epoch = idx;
                new_run->obs.reserve(2000);
                new_run->lli.reserve(2000);
                new_run->ssi.reserve(2000);
                new_run->next = std::move(s_ptr);
                s_ptr = std::move(new_run);
            }

            const char *obs = p->buffer + ii * 16;
            s_ptr->obs.push_back(rinex_parse_obs(obs));
            s_ptr->lli.push_back(obs[14]);
            s_ptr->ssi.push_back(obs[15]);
        }
    }
    if (res < 0)
    {
        printf("Failure %d reading %s\n", res, filename);
    }

    // TODO: Count space for storing the epochs. (Probably small.)

    // Analyze each run for each signal.
    int64_t tot_ssi = 0;
    for (const auto & p : sigs)
    {
        int count = 0;
        if (verbose) printf("%s_%s:", p.first.id.sv, p.first.id.obs);
        for (auto ptr = p.second.get(); ptr; ptr = ptr->next.get())
        {
            int l0, l1, l2, l3, l4, l5, l_lli, l_ssi, total;
            analyze_obs(ptr->obs, l0, l1, l2, l3, l4, l5);
            l_lli = analyze_rle(ptr->lli);
            l_ssi = analyze_rle(ptr->ssi);
            tot_ssi += l_ssi;
            // Need accessory counts: run length and (if run size has
            // more than one element) delta level.
            total = l_ubase128(ptr->obs.size())
                + (ptr->obs.size() > 1 ? 1 : 0) + l_lli + l_ssi
                + std::min(std::min(std::min(l0, l1), std::min(l2, l3)),
                    std::min(l4, l5));
            if (verbose)
            {
                printf(" %zu@%zu:(%d|%d|%d|%d|%d|%d)+%d+%d=%d",
                    ptr->obs.size(), ptr->base_epoch, l0, l1, l2, l3, l4, l5,
                    l_lli, l_ssi, total);
            }
            grand_total += total;
            ++count;
            ++n_runs;
        }
        if (verbose) printf(" (%d runs)\n", count);
        grand_total += l_ubase128(count);
    }

    printf("%s: %ld runs of %zu signals in %zu epochs: %ld bytes (%ld SSI)\n",
        filename, n_runs, sigs.size(), epochs.size(), grand_total, tot_ssi);
}
