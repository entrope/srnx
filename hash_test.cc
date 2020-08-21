/** hash_test.cc - Hash function statistics testing.
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

#include "rinex.h"
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <x86intrin.h>

static int extra_order = 0;

static void h_report(
    const char name[],
    const std::vector<uint64_t> &hash,
    std::vector<int> hist,
    std::vector<int> h_2,
    bool fib_hash
)
{
    // Build the histogram.
    size_t len = hist.size();
    hist.assign(hist.size(), 0);
    if (len & (len - 1))
    {
        for (auto v : hash)
        {
            ++hist[v % len];
        }
    }
    else
    {
        int order = CHAR_BIT * sizeof(unsigned long long) - 1
            - __builtin_clzll(len);

        for (auto v : hash)
        {
            auto x = fib_hash
                ? ((v * 11400714819323198485llu) >> (64 - order))
                : (v & (len - 1));
            ++hist[x];
        }
    }

    // Analyze the basic statistics.
    int colls = 0, max = 0, n = 0;
    for (auto x : hist)
    {
        if (x > 0)
        {
            ++n;
            if (x > 1) colls += x;
            max = std::max(max, x);
        }
    }
    printf("\"%s%s\",%d,%d", name, fib_hash ? " fib" : "", colls, max);

    // Generate a histogram of histogram bin values.
    h_2.assign(std::lround(max) + 1, 0);
    for (auto x : hist)
    {
        ++h_2[x];
    }
    for (auto h : h_2)
    {
        printf(",%d", h);
    }
    printf("\n");
}

static void analyze(
    const char name[],
    const std::vector<rinex_signal_t> &sigs,
    std::vector<uint64_t> &hash,
    std::vector<int> &hist,
    std::vector<int> h_2,
    uint64_t (*h_func)(rinex_signal_t)
)
{
    hash.resize(sigs.size());
    for (size_t ii = 0; ii < sigs.size(); ++ii)
    {
        hash[ii] = h_func(sigs[ii]);
    }

    // Check for hash-code collisions.
    for (size_t ii = 0; ii < sigs.size(); ++ii)
    {
        for (size_t jj = ii + 1; jj < sigs.size(); ++jj)
        {
            if (hash[ii] == hash[jj])
            {
                printf("# %s collision: h(%s %s : %#lx)=h(%s %s : %#lx)=%#lx\n", name,
                    sigs[ii].id.sv, sigs[ii].id.obs, sigs[ii].u64,
                    sigs[jj].id.sv, sigs[jj].id.obs, sigs[jj].u64,
                    hash[ii]);
            }
        }
    }

    h_report(name, hash, hist, h_2, false);
    h_report(name, hash, hist, h_2, true);
}

static void add_sigs
(
    std::vector<rinex_signal_t> &res,
    const std::vector<std::string> &sv_obs,
    char sv_id,
    int n_sv,
    int min_sv = 1
)
{
    rinex_signal_t sig;

    sig.id.sv[0] = sv_id;
    sig.id.sv[3] = '\0';
    sig.id.obs[2] = '\0';
    sig.id.obs[3] = '\0';
    for (; n_sv >= min_sv; --n_sv)
    {
        sig.id.sv[1] = '0' + (n_sv / 10);
        sig.id.sv[2] = '0' + (n_sv % 10);

        for (auto obs : sv_obs)
        {
            std::memcpy(sig.id.obs, obs.data(), obs.size());
            res.push_back(sig);
        }
    }
}

static void xprod
(
    std::vector<std::string> &obs,
    const char freq[],
    const char attr[],
    const char type[] = "CLDS"
)
{
    char c[4];

    c[3] = '\0';
    for (int ii = 0; freq[ii] != '\0'; ++ii)
    {
        c[1] = freq[ii];
        for (int jj = 0; attr[jj] != '\0'; ++jj)
        {
            c[2] = (attr[jj] == '_') ? '\0' : attr[jj];
            for (int kk = 0; type[kk] != '\0'; ++kk)
            {
                c[0] = type[kk];
                obs.push_back(c);
            }
        }
    }
}

static void append(std::vector<std::string> &obs, ...)
{
    va_list args;
    const char *name;
    char id[4];

    id[3] = '\0';
    va_start(args, obs);
    while ((name = va_arg(args, const char *)) != nullptr)
    {
        id[0] = name[0];
        id[1] = name[1];
        id[2] = name[2];
        obs.push_back(id);
    }
    va_end(args);
}

static std::vector<rinex_signal_t> build_v2_sigs()
{
    std::vector<rinex_signal_t> res;
    std::vector<std::string> obs;

    xprod(obs, "12", "_");
    add_sigs(res, obs, 'S', 58, 20);

    append(obs, "P1", "P2", NULL);
    add_sigs(res, obs, 'R', 24);

    xprod(obs, "5", "_");
    add_sigs(res, obs, 'G', 32);

    obs.clear();
    xprod(obs, "15678", "_");
    add_sigs(res, obs, 'E', 36);

    return res;
}

static std::vector<rinex_signal_t> build_v3_sigs()
{
    std::vector<rinex_signal_t> res;
    std::vector<std::string> obs;

    // From RINEX Version 3.04 Table 4:
    obs.clear();
    xprod(obs, "12", "CSLXPWYM");
    xprod(obs, "12", "N", "LDS");
    xprod(obs, "2", "D");
    xprod(obs, "5", "IQX");
    add_sigs(res, obs, 'G', 32);

    // From RINEX Version 3.04 Table 5:
    obs.clear();
    xprod(obs, "12", "CP");
    xprod(obs, "46", "ABX");
    xprod(obs, "3", "IQX");
    add_sigs(res, obs, 'R', 24);

    // From RINEX Version 3.04 Table 6:
    obs.clear();
    xprod(obs, "16", "ABCXZ");
    xprod(obs, "578", "IQX");
    add_sigs(res, obs, 'E', 36);

    // From RINEX Version 3.04 Table 7:
    obs.clear();
    xprod(obs, "1", "C");
    xprod(obs, "5", "IQX");
    add_sigs(res, obs, 'S', 58, 20);

    // From RINEX Version 3.04 Table 8:
    obs.clear();
    xprod(obs, "1", "CSLXZ");
    xprod(obs, "2", "SLX");
    xprod(obs, "5", "IQXDPZ");
    xprod(obs, "6", "SLXEZ");
    add_sigs(res, obs, 'J', 9);

    // From RINEX Version 3.04 Table 9:
    obs.clear();
    xprod(obs, "2", "IQX");
    xprod(obs, "1", "DPXA");
    xprod(obs, "1", "N", "LDS");
    xprod(obs, "5", "DPX");
    xprod(obs, "7", "IQXDPZ");
    xprod(obs, "8", "DPX");
    xprod(obs, "6", "QXA");
    add_sigs(res, obs, 'B', 63);

    // From RINEX Version 3.04 Table 10:
    obs.clear();
    xprod(obs, "59", "ABCX");
    add_sigs(res, obs, 'I', 14);

    return res;
}

static inline uint64_t ror64(uint64_t v, int r)
{
    return (v >> r) | (v << (64 - r));
}

static uint64_t sig_crc(rinex_signal_t sig)
{
    return _mm_crc32_u64(~uint64_t(0), sig.u64);
}

static uint64_t splitmix(rinex_signal_t sig)
{
    // From https://stackoverflow.com/a/12996028 .
    uint64_t x = sig.u64;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9;
    x = (x ^ (x >> 27)) * 0x94d049bb133111eb;
    return x ^ (x >> 31);
}

/* This generates an absurdly large number of collisions.
static uint64_t xorshift(rinex_signal_t sig)
{
    // From https://stackoverflow.com/a/57556517 .
    uint64_t x = sig.u64;
    x = (x ^ (x >> 32)) * 0x5555555555555555;
    x = (x ^ (x >> 32)) * 17316035218449499591u;
    return x;
}
*/

static uint64_t rrxmrrxmsx_0(rinex_signal_t sig)
{
    uint64_t v = sig.u64;
    v ^= ror64(v, 25) ^ ror64(v, 50);
    v *= 0xA24BAED4963EE407UL;
    v ^= ror64(v, 24) ^ ror64(v, 49);
    v *= 0x9FB21C651E98DF25UL;
    return v ^ v >> 28;
}

/* This tries to extract the "active" bits for a RINEX 2.x signal name:
 *
 * S20 to S58: [CLDS][12]
 * R01 to R24: [CLDS][12], P[12]
 * G01 to G32: [CLDS][125], P[12]
 * E01 to E36: [CLDS][15678]
 *
 * 'E': 0100_0101, 'G': 0100_0111, 'R': 0101_0010, 'S': 0101_0011
 * '0' .. '9': 0011_0000 .. 0011_1001
 * 'C': 0100_0011, 'D': 0100_0100, 'L': 0100_1100, 'S': 0101_0011,
 * 'P': 0101_0000
 *
 * So we extract the following bits (LSB = 0):
 * sv[0]: 2, 3 ^ 0
 * sv[1]: 0, 1
 * sv[2]: 0, 1, 2
 * obs[0]: 0, 3 ^ 4
 * obs[1]: 0, 1 ^ 4, 2
 *
 * ... but this leads to a number of collisions, e.g. S58C1 / S50C1,
 * R17L1 / R17P1.
static uint64_t v2_hash(rinex_signal_t sig)
{
    return (sig.id.sv[1] & 3) // two bits
        | ((sig.id.sv[0] & 12) ^ ((sig.id.sv[0] & 1) << 3)) // two bits
        | ((sig.id.sv[2] & 7) << 4) // three bits
        | (((sig.id.obs[0] & 16) << 3) ^ ((sig.id.obs[0] & 8) << 4)) // one bit
        | ((sig.id.obs[0] & 1) << 8) // one bit
        | (((sig.id.obs[1] & 7) << 9) ^ ((sig.id.obs[1] & 16) << 7)); // two bits
}
 */

static void phash(std::vector<rinex_signal_t> &sigs)
{
    uint64_t act_mask;
    size_t ii, jj;

    act_mask = 0;
    for (ii = 1; ii < sigs.size(); ++ii)
    {
        for (jj = 0; jj < ii; ++jj)
        {
            act_mask |= sigs[ii].u64 ^ sigs[jj].u64;
        }
    }
    printf("# activity mask: %#lx (%d bits)\n", act_mask,
        __builtin_popcountll(act_mask));

    if (false)
    {
        for (auto & s : sigs)
        {
            s.u64 = _pext_u64(s.u64, act_mask);
        }
    }
}

/*
using visit_t = void (*)(const std::vector<char> &);

static void find_all_children2(std::vector<char> a, int k, int depth, visit_t visitor)
{
    bool even = !(depth & 1);
    if (even)
        visitor(a);

    if (!even)
        visitor(a);
}

static void generate_partitions(int n, int k, visit_t visitor)
{
    // Check constraints.
    if (n < 0 || n < k)
    {
        fprintf(stderr, "bogon\n");
        return;
    }

    // Build the root.
    std::vector<char> a(k);
    for (int ii = k, jj = n; ii > 0; )
    {
        if (jj > 0) --jj;
        a[--ii] = jj;
    }

    find_all_children2(a, k, 1, visitor);
}
*/

int main(int, char *[])
{
    std::vector<rinex_signal_t> sigs;
    std::vector<uint64_t> hash;
    std::vector<int> hist, h_2;

    printf("name,collided,max,n0,n1,...\n");

    sigs = build_v2_sigs();
    printf("# %zu signals for RINEX v2\n", sigs.size());
    phash(sigs);
    // v2 has 20 active bits, but fewer than 2^11 items.
    // Wolfram Alpha says StirlingS2[20,11] = 1 900 842 429 486.
    for (extra_order = 0; extra_order <= 4; ++extra_order)
    {
        int order = sizeof(size_t) * CHAR_BIT - __builtin_clzll(sigs.size());
        hist.resize(size_t(1) << (order + extra_order));
        printf("\n# hash table size: %zu\n", hist.size());

        analyze("v2-crc32c", sigs, hash, hist, h_2, sig_crc);
        analyze("v2-splitmix", sigs, hash, hist, h_2, splitmix);
        analyze("v2-rrxmrrxmsx_0", sigs, hash, hist, h_2, rrxmrrxmsx_0);
    }

    sigs = build_v3_sigs();
    printf("\n# %zu signals for RINEX v3\n", sigs.size());
    phash(sigs);
    // v3 has 26 active bits, but fewer than 2^14 items.
    // Wolfram Alpha says StirlingS2[26,14] = ￼477 898 618 396 288 260.
    for (extra_order = 0; extra_order <= 4; ++extra_order)
    {
        int order = sizeof(size_t) * CHAR_BIT - __builtin_clzll(sigs.size());
        hist.resize(size_t(1) << (order + extra_order));
        printf("\n# hash table size: %zu\n", hist.size());

        analyze("v3-crc32c", sigs, hash, hist, h_2, sig_crc);
        analyze("v3-splitmix", sigs, hash, hist, h_2, splitmix);
        analyze("v3-rrxmrrxmsx_0", sigs, hash, hist, h_2, rrxmrrxmsx_0);
    }

    sigs = build_v3_sigs();
    auto v2_sigs = build_v2_sigs();
    sigs.insert(sigs.end(), v2_sigs.begin(), v2_sigs.end());
    printf("\n# %zu signals for combined v2+v3\n", sigs.size());
    // v2+v3 has 27 active bits, but fewer than 2^14 items.
    // Wolfram Alpha says StirlingS2[27,14] = 8 541 149 231 801 585 700.
    // That is only searchable using a cluster of GPUs... e.g. assume
    // RTX 2080 Super: 3072 cores, 1650 Mhz (1815 MHz boost).  Then each
    // cycle needed for a single candidate takes 1685043 seconds, about
    // 19.5 GPU-days.  Calculating ~16k hash codes and checking for
    // collisions will take upwards of 256k cycles.
    phash(sigs);
    for (extra_order = 0; extra_order <= 4; ++extra_order)
    {
        int order = sizeof(size_t) * CHAR_BIT - __builtin_clzll(sigs.size());
        hist.resize(size_t(1) << (order + extra_order));
        printf("\n# hash table size: %zu\n", hist.size());

        analyze("v23-crc32c", sigs, hash, hist, h_2, sig_crc);
        analyze("v23-splitmix", sigs, hash, hist, h_2, splitmix);
        analyze("v23-rrxmrrxmsx_0", sigs, hash, hist, h_2, rrxmrrxmsx_0);
    }

    return EXIT_SUCCESS;
}
