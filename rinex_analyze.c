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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __x86_64__
# include <x86intrin.h>
#endif

/** run_info identifies the start epoch and length of a contiguous set
 * of epochs.
 */
struct run_info
{
    /** start is the index of the first epoch in the run. */
    int start;

    /** count is the number of epochs in the run. */
    int count;
};

/** signal_obs contains information about a single signal's
 * observations.
 */
struct signal_obs
{
    /** used is how many elements are used in #obs, #lli and #ssi. */
    int used;

    /** alloc is how many elements are allocated for the arrays. */
    int alloc;

    /** obs contains the observation values (times 1000). */
    int64_t *obs;

    /** lli contains the loss-of-lock indicators. */
    char *lli;

    /** ssi contains the signal strength indicators. */
    char *ssi;
};

/** sv_obs contains the observations from a single satellite. */
/* XXX: Maybe include satellite name? */
struct sv_obs
{
    /** name is the three-character name of the satellite, plus '\0'. */
    char name[4];

    /** n_obs is the number of epochs this satellite has observations. */
    int n_obs;

    /** n_run is the number of runs in #run. */
    int n_run;

    /** run_alloc is the allocated length of #run. */
    int run_alloc;

    /** run is an array describing runs of epochs when this satellite
     * was observed.
     */
    struct run_info *run;

    /** obs points to observation data for each observation code.
     * The indexing matches whatever is given in the file header;
     * entries may be null.
     */
    struct signal_obs obs[1];
};

/** system_info describes a single satellite system. */
/* XXX: Maybe include signal names? */
struct system_info
{
    /** start is the index of the first satellite in this system. */
    int start;

    /** count is the number of satellites in the system. */
    short count;

    /** n_obs is the number of observation codes for this satellite
     * system in the current file.
     */
    short n_obs;
};

/** file_data holds all the data from a single file. */
struct file_data
{
    /** n_epoch is the number of valid epochs in #epoch. */
    int n_epoch;

    /** epoch_alloc is the allocated length of #epoch. */
    int epoch_alloc;

    /** n_sv is the length (both used and allocated) of #sv. */
    int n_sv;

    /** epoch contains the epochs for this file. */
    struct rinex_epoch *epoch;

    /** sv contains the observation data for each observed satellite. */
    struct sv_obs **sv;

    /** sys_info describes how satellite numbers map into #sv. */
    struct system_info sys_info[32];
};

int64_t min_s128[5], max_s128[5];
uint64_t rlsb[5][64];

static int l_ubase128(uint64_t val)
{
    if (!(val >> 7)) return 1;
    if (!(val >> 14)) return 2;
    if (!(val >> 21)) return 3;
    if (!(val >> 28)) return 4;
    if (!(val >> 35)) return 5;
    if (!(val >> 42)) return 6;
    if (!(val >> 49)) return 7;
    printf(" Bueller! "); /* nothing so large is ever expected */
    if (!(val >> 56)) return 8;
    if (!(val >> 63)) return 9;
    return 10;
}

static int l_sbase128(int64_t val)
{
    return l_ubase128((val << 1) ^ (val >> 63));
}

static int l_sbase128_d(int64_t val, int level)
{
    if (min_s128[level] > val)
    {
        min_s128[level] = val;
    }
    if (max_s128[level] < val)
    {
        max_s128[level] = val;
    }
    ++rlsb[level][__builtin_clrsbl(val)];

    return l_sbase128(val);
}

__attribute__((noinline))
void analyze_obs(const int64_t *obs, int n_obs, int *l0,
    int *l1, int *l2, int *l3, int *l4, int *l5)
{
    int64_t d[6], p[5];
    int tmp;

    d[0] = obs[0];
    *l0 = *l1 = *l2 = *l3 = *l4 = *l5 = l_sbase128(d[0]);

    if (n_obs < 2) return;
    p[0] = d[0], d[0] = obs[1];
    d[1] = d[0] - p[0];
    *l0 += l_sbase128(d[0]);
    tmp = l_sbase128_d(d[1], 0);
    *l1 += tmp; *l2 += tmp; *l3 += tmp; *l4 += tmp; *l5 += tmp;

    if (n_obs < 3) return;
    p[0] = d[0], d[0] = obs[2];
    p[1] = d[1], d[1] = d[0] - p[0];
    d[2] = d[1] - p[1];
    *l0 += l_sbase128(d[0]);
    *l1 += l_sbase128_d(d[1], 0);
    tmp = l_sbase128_d(d[2], 1);
    *l2 += tmp; *l3 += tmp; *l4 += tmp; *l5 += tmp;

    if (n_obs < 4) return;
    p[0] = d[0], d[0] = obs[3];
    p[1] = d[1], d[1] = d[0] - p[0];
    p[2] = d[2], d[2] = d[1] - p[1];
    d[3] = d[2] - p[2];
    *l0 += l_sbase128(d[0]);
    *l1 += l_sbase128_d(d[1], 0);
    *l2 += l_sbase128_d(d[2], 1);
    tmp = l_sbase128_d(d[3], 2);
    *l3 += tmp; *l4 += tmp; *l5 += tmp;

    if (n_obs < 5) return;
    p[0] = d[0], d[0] = obs[4];
    p[1] = d[1], d[1] = d[0] - p[0];
    p[2] = d[2], d[2] = d[1] - p[1];
    p[3] = d[3], d[3] = d[2] - p[2];
    d[4] = d[3] - p[3];
    *l0 += l_sbase128(d[0]);
    *l1 += l_sbase128_d(d[1], 0);
    *l2 += l_sbase128_d(d[2], 1);
    *l3 += l_sbase128_d(d[3], 2);
    tmp = l_sbase128_d(d[4], 3);
    *l4 += tmp; *l5 += tmp;

    for (int ii = 5; ii < n_obs; ++ii)
    {
        p[0] = d[0], d[0] = obs[ii];
        p[1] = d[1], d[1] = d[0] - p[0];
        p[2] = d[2], d[2] = d[1] - p[1];
        p[3] = d[3], d[3] = d[2] - p[2];
        p[4] = d[4], d[4] = d[3] - p[3];
        d[5] = d[4] - p[4];

        *l0 += l_sbase128(d[0]);
        *l1 += l_sbase128_d(d[1], 0);
        *l2 += l_sbase128_d(d[2], 1);
        *l3 += l_sbase128_d(d[3], 2);
        *l4 += l_sbase128_d(d[4], 3);
        *l5 += l_sbase128_d(d[5], 4);
    }
}

__attribute__((noinline))
int analyze_rle(const char v[], int size)
{
    int ii = 0, start = 0, len = 0;
    char curr = v[0];

    for (ii = 1; ii < size; ++ii)
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

struct file_data data;

static int find_n_obs(int svn)
{
    int ii;

    for (ii = 0; ii < 32; ++ii)
    {
        int start = data.sys_info[ii].start;
        int count = data.sys_info[ii].count;
        if (start <= svn && svn < (start + count))
        {
            return data.sys_info[ii].n_obs;
        }
    }

    fprintf(stderr, "Unable to find system for %d'th satellite\n", svn);
    exit(1);
}

static void empty_file_data(struct file_data *f)
{
    int ii, jj, n_obs;

    f->n_epoch = 0;

    for (ii = 0; ii < f->n_sv; ++ii)
    {
        struct sv_obs *sv = f->sv[ii];
        if (!sv)
        {
            continue;
        }

        sv->n_obs = 0;
        sv->n_run = 0;
        n_obs = find_n_obs(ii);
        for (jj = 0; jj < n_obs; ++jj)
        {
            sv->obs[jj].used = 0;
        }
    }
}

static void init_file_data(struct file_data *f)
{
    int ii;

    f->sys_info['G' & 31].start = 0;
    f->sys_info['G' & 31].count = 32;
    f->sys_info['R' & 31].start = 32;
    f->sys_info['R' & 31].count = 24;
    f->sys_info['S' & 31].start = 32 + 24 - 20; /* be careful! */
    f->sys_info['S' & 31].count = 59;
    f->sys_info['E' & 31].start = 32 + 24 + 39;
    f->sys_info['E' & 31].count = 36;
    f->sys_info['C' & 31].start = 32 + 24 + 39 + 36;
    f->sys_info['C' & 31].count = 63;
    f->sys_info['J' & 31].start = 32 + 24 + 39 + 36 + 63;
    f->sys_info['J' & 31].count = 10;
    f->sys_info['I' & 31].start = 32 + 24 + 39 + 36 + 63 + 10;
    f->sys_info['I' & 31].count = 14;

    f->epoch_alloc = 2880;
    f->epoch = calloc(f->epoch_alloc, sizeof f->epoch[0]);
    if (!f->epoch)
    {
        fprintf(stderr, "Unable to allocate space for epochs\n");
        exit(1);
    }

    f->n_sv = 32 + 24 + 39 + 36 + 63 + 10 + 14; /* 218 */
    f->sv = calloc(f->n_sv, sizeof f->sv[0]);
    if (!f->sv)
    {
        fprintf(stderr, "Unable to allocate space for SVs\n");
        exit(1);
    }

    for (ii = 0; ii < f->n_sv; ++ii)
    {
        f->sv[ii] = NULL;
    }
}

static void grow_system(int sys_id, int svn)
{
    struct sv_obs **new_sv;
    int new_n_sv, growth, old_next, ii, jj;

    /* Allocate a big-enough array of pointers-to-sv_obs. */
    growth = svn + 1 - data.sys_info[sys_id].count;
    new_n_sv = data.n_sv + growth;
    new_sv = calloc(new_n_sv, sizeof new_sv[0]);

    /* Copy or initialize elements of new_sv. */
    old_next = data.sys_info[sys_id].start + data.sys_info[sys_id].count;
    for (ii = 0; ii < old_next; ++ii)
    {
        new_sv[ii] = data.sv[ii];
    }
    for (jj = 0; jj < growth; ++jj)
    {
        new_sv[ii+jj] = NULL;
    }
    for (; ii < data.n_sv; ++ii)
    {
        new_sv[ii+growth] = data.sv[ii];
    }
    data.n_sv = new_n_sv;
    data.sv = new_sv;

    /* Update data.sys_info[] to match. */
    data.sys_info[sys_id].count += growth;
    for (ii = 0; ii < 32; ++ii)
    {
        if (data.sys_info[ii].start >= old_next)
        {
            data.sys_info[ii].start += growth;
        }
    }
}

static void record(struct signal_obs *s_obs, int sv_obs, int64_t obs, char lli, char ssi)
{
    if (sv_obs >= s_obs->alloc)
    {
        if (s_obs->alloc == 0)
        {
            s_obs->alloc = 2880;
            s_obs->obs = malloc(s_obs->alloc * sizeof(s_obs->obs[0]));
            s_obs->lli = malloc(s_obs->alloc);
            s_obs->ssi = malloc(s_obs->alloc);
        }
        while (sv_obs >= s_obs->alloc)
        {
            s_obs->alloc <<= 1;
            s_obs->obs = realloc(s_obs->obs, s_obs->alloc * sizeof(s_obs->obs[0]));
            s_obs->lli = realloc(s_obs->lli, s_obs->alloc);
            s_obs->ssi = realloc(s_obs->ssi, s_obs->alloc);
        }
        if (!s_obs->obs || !s_obs->lli || !s_obs->ssi)
        {
            fprintf(stderr, "Unable to grow observation data\n");
            exit(1);
        }
    }

    if (s_obs->used < sv_obs)
    {
        int gap = sv_obs - s_obs->used;
        memset(s_obs->obs + s_obs->used, 0, gap * sizeof(s_obs->obs[0]));
        memset(s_obs->lli + s_obs->used, 0, gap);
        memset(s_obs->ssi + s_obs->used, 0, gap);
        s_obs->used += gap;
    }

#ifdef __SSE2__
    _mm_stream_si64((void *)(s_obs->obs + s_obs->used), obs);
#else
    s_obs->obs[s_obs->used] = obs;
#endif
    s_obs->lli[s_obs->used] = lli;
    s_obs->ssi[s_obs->used] = ssi;
    ++s_obs->used;
}

static void add_run(struct sv_obs *sv, int epoch)
{
    if (sv->n_run >= sv->run_alloc)
    {
        sv->run_alloc <<= 1;
        sv->run = realloc(sv->run, sv->run_alloc * sizeof(sv->run[0]));
        if (!sv->run)
        {
            fprintf(stderr, "Memory allocation failed when growing satellite runs\n");
            exit(1);
        }
    }

    sv->run[sv->n_run].start = epoch;
    sv->run[sv->n_run].count = 1;
    ++sv->n_run;
}

static int64_t read_file(struct rinex_parser *p, const char filename[])
{
    int64_t grand_total = p->buffer_len;
    struct sv_obs *sv;
    const char *buffer;
    int res, ii, jj, kk, sys_id, svn, n_obs;

    /* Read the data into memory. */
    while ((res = p->read(p)) > 0)
    {
        const int idx = data.n_epoch;

        if (p->epoch.flag > '1' && p->epoch.flag < '6')
        {
            /* Special events would be stored as a epoch index and the
             * raw buffer (which includes the EVENT FLAG record and the
             * number of lines that follow).
             */
            grand_total += l_ubase128(idx) + p->buffer_len;
            continue;
        }

        /* Append the epoch to our data structure. */
        if (data.n_epoch >= data.epoch_alloc)
        {
            data.epoch_alloc <<= 1;
            data.epoch = realloc(data.epoch, data.epoch_alloc * sizeof(data.epoch[0]));
            if (!data.epoch)
            {
                fprintf(stderr, "Memory allocation failed when growing epoch array\n");
                exit(1);
            }
        }
        memcpy(&data.epoch[idx], &p->epoch, sizeof data.epoch[idx]);
        ++data.n_epoch;

        /* Save the observations that are present. */
        for (ii = jj = 0, buffer = p->buffer; ii < p->epoch.n_sats; ++ii)
        {
            /* Make sure the satellite system can hold this SVN. */
            sys_id = *buffer++ & 31;
            svn = *buffer++;
            if (svn > data.sys_info[sys_id].count)
            {
                grow_system(sys_id, svn);
            }

            /* Make sure we have a sv_obs. */
            svn += data.sys_info[sys_id].start - 1;
            n_obs = data.sys_info[sys_id].n_obs;
            sv = data.sv[svn];
            if (!sv)
            {
                sv = calloc(1, sizeof(*sv)
                    + (n_obs - 1) * sizeof(sv->obs[0]));
                if (!sv)
                {
                    fprintf(stderr, "Memory allocation failed for new satellite\n");
                    exit(1);
                }

                sv->name[0] = buffer[-2];
                sv->name[1] = (buffer[-1] / 10) + '0';
                sv->name[2] = (buffer[-1] % 10) + '0';
                sv->run_alloc = 31;
                sv->run = calloc(sv->run_alloc, sizeof sv->run[0]);
                if (!sv->run)
                {
                    fprintf(stderr, "Memory allocation failed for new satellite's runs\n");
                    exit(1);
                }

                data.sv[svn] = sv;
            }

            /* Add this epoch to its run list. */
            if (sv->n_run == 0)
            {
                sv->run[0].start = idx;
                sv->run[0].count = 1;
                sv->n_run = 1;
            }
            else if (idx == sv->run[sv->n_run-1].start + sv->run[sv->n_run-1].count)
            {
                ++sv->run[sv->n_run-1].count;
            }
            else
            {
                add_run(sv, idx);
            }

            /* Process its potential observations. */
            for (kk = 0; kk < n_obs; )
            {
                if (*buffer & (1 << (kk & 7)))
                {
                    record(&sv->obs[kk], sv->n_obs, p->obs[jj], p->lli[jj], p->ssi[jj]);
                    ++jj;
                }

                if (!(++kk & 7))
                {
                    buffer++;
                }
            }
            if (kk & 7)
            {
                buffer++;
            }

            ++sv->n_obs;
        }
    }
    if (res < 0)
    {
        printf("Failure %d reading %s\n", res, filename);
    }

    return grand_total;
}

static void analyze_compression(const char filename[], int64_t grand_total)
{
    /* Analyze runs for each satellite. */
    struct sv_obs *sv;
    int n_sigs = 0, prev_epoch = 0, ii, jj, n_obs;

    for (ii = 0; ii < data.n_sv; ++ii)
    {
        int sv_total, sv_obs;
        sv = data.sv[ii];
        if (!sv)
        {
            continue;
        }

        /* How much do we need to store the runs? */
        sv_total = 0;
        for (jj = prev_epoch = sv_obs = 0; jj < sv->n_run; ++jj)
        {
            sv_total += l_ubase128(sv->run[jj].start - prev_epoch)
                + l_ubase128(sv->run[jj].count - 1);
            prev_epoch = sv->run[jj].start + sv->run[jj].count + 1;
            sv_obs += sv->run[jj].count;
        }

        n_obs = find_n_obs(ii);
        for (jj = 0; jj < n_obs; ++jj)
        {
            struct signal_obs *s_obs;
            int l0, l1, l2, l3, l4, l5, l_lli, l_ssi, l_min;

            s_obs = &sv->obs[jj];
            if (!s_obs->used)
            {
                continue;
            }
            if (verbose && (s_obs->used != sv_obs) && 0)
            {
                printf("WARNING: SV %d obs %d had %d observations, %d expected\n",
                    ii, jj, s_obs->used, sv_obs);
            }
            analyze_obs(s_obs->obs, s_obs->used, &l0, &l1, &l2, &l3, &l4, &l5);
            l_lli = analyze_rle(s_obs->lli, s_obs->used);
            l_ssi = analyze_rle(s_obs->ssi, s_obs->used);

            l_min = l0;
            if (l_min > l1) l_min = l1;
            if (l_min > l2) l_min = l2;
            if (l_min > l3) l_min = l3;
            if (l_min > l4) l_min = l4;
            if (l_min > l5) l_min = l5;

            n_sigs += 1;
            sv_total += l_lli + l_ssi + (s_obs->used ? 1 : 0) + l_min;

            if (verbose) printf("%s_%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
                sv->name, jj, l0, l1, l2, l3, l4, l5, l_lli, l_ssi, sv_total);
        }

        grand_total += sv_total;
    }

    printf("%s: %d signals in %d epochs: %lld bytes\n",
        filename, n_sigs, data.n_epoch, (long long)grand_total);
}

void process_file(struct rinex_parser *p, const char filename[])
{
    int64_t grand_total;
    int ii, jj, svn, n_obs;

    /* If necessary, initialize our file data container. */
    if (data.epoch)
    {
        empty_file_data(&data);
    }
    else
    {
        init_file_data(&data);
        if (verbose)
        {
            printf("signal,l0,l1,l2,l3,l4,l5,lli,ssi,total\n");
        }
    }
    if (verbose)
    {
        printf("\"%s\"\n", filename);
    }

    /* Copy number of observations per satellite system. */
    for (ii = 0; ii < 32; ++ii)
    {
        n_obs = data.sys_info[ii].n_obs;
        if (n_obs && (n_obs != p->n_obs[ii]))
        {
            for (jj = 0; jj < data.sys_info[ii].count; ++jj)
            {
                svn = data.sys_info[ii].start + jj;
                if (data.sv[svn])
                {
                    free(data.sv[svn]);
                    data.sv[svn] = NULL;
                    continue;
                }
            }
        }
        data.sys_info[ii].n_obs = p->n_obs[ii];
    }

    /* Need to save the header. */
    grand_total = read_file(p, filename);

    // TODO: Count space for storing the epochs. (Probably small.)

    analyze_compression(filename, grand_total);
}

void finish()
{
    printf("\nzrange = [");
    for (int ii = 0; ii < 5; ++ii)
    {
        printf(" %lld %lld;", (long long)min_s128[ii], (long long)max_s128[ii]);
    }
    printf(" ];\nlrsb = [");
    for (int ii = 0; ii < 64; ++ii)
    {
        printf(" %d %llu %llu %llu %llu %llu;\n", ii,
            (long long)rlsb[0][ii], (long long)rlsb[1][ii],
            (long long)rlsb[2][ii], (long long)rlsb[3][ii],
            (long long)rlsb[4][ii]);
    }
    printf("]\n");
}
