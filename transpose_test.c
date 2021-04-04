#define _POSIX_C_SOURCE 199309L

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "transpose.h"

int64_t out[32];
char input_8[32];
char input_16[64];
char input_32[128];

const int truth[32] = {
    0x55555555, 0x33333333, 0x0f0f0f0f, 0x00ff00ff,
    0x0000ffff, 0xaaaaaaaa, 0xcccccccc, 0xf0f0f0f0,
    0xff00ff00, 0xffff0000, 0x0000ffff, 0x00ffff00,
    0x0ff00ff0, 0x3c3c3c3c, 0x66666666, 0xffffffff,

    0x12345678, 0x31415927, 0xcafebabe, 0xcafed00d,
    0x47494638, 0x89504e47, 0x4d546864, 0x2321202f,
    0x7f454c46, 0x25504446, 0x19540119, 0x4a6f7921,
    0x49492a00, 0x4d4d002a, 0x57414433, 0xd0cf11e0
};

static void test_transpose(void)
{
    int ii, jj;

    printf(" Transpose 8x(m+1):\n");
    for (ii = 1; ii < 33; ++ii)
    {
        transpose(out, input_8, ii, 8);
        printf("%d:", ii);
        for (jj = 0; jj < 8; ++jj)
        {
            int expect = truth[jj] >> (32 - ii);
            printf(" %llx%c", (long long)out[jj],
                (out[jj] == expect) ? ' ' : '!');
        }
        printf("\n");
    }

    printf("\n Transpose 16x(m+1):\n");
    for (ii = 1; ii < 33; ++ii)
    {
        transpose(out, input_16, ii, 16);
        printf("%d:", ii);
        for (jj = 0; jj < 16; ++jj)
        {
            int expect = truth[jj] >> (32 - ii);
            printf(" %llx%c", (long long)out[jj],
                (out[jj] == expect) ? ' ' : '!');
        }
        printf("\n");
    }

    printf("\n Transpose 32x(m+1):\n");
    for (ii = 1; ii < 33; ++ii)
    {
        transpose(out, input_32, ii, 32);
        printf("%d:", ii);
        for (jj = 0; jj < 32; ++jj)
        {
            int expect = truth[jj] >> (32 - ii);
            printf(" %llx%c", (long long)out[jj],
                (out[jj] == expect) ? ' ' : '!');
        }
        printf("\n");
    }
}

static void benchmark_transpose(const char *version)
{
    struct timespec t[33];
    unsigned long long nsec;
    const int n_reps = 1000000;
    int ii, bits, count;

    transpose_select(version);
    if (!version) version = "default";

    /* Warm up the cache and hint that we are CPU-intensive. */
    transpose(out, input_8, 32, 32);
    transpose(out, input_8, 32, 16);
    transpose(out, input_8, 32, 8);

    for (count = 8; count <= 32; count <<= 1)
    {
        printf("\n%s n-by-%d", version, count);
        clock_gettime(CLOCK_MONOTONIC, &t[0]);
        for (bits = 1; bits < 33; ++bits)
        {
            for (ii = 0; ii < n_reps; ++ii)
            {
                transpose(out, input_8, bits, count);
            }
            clock_gettime(CLOCK_MONOTONIC, &t[bits]);
        }

        for (ii = 0; ii + 1 < bits; ++ii)
        {
            nsec = (t[ii+1].tv_sec - t[ii].tv_sec) * 1000000000
                + t[ii+1].tv_nsec - t[ii].tv_nsec;
            printf(",%llu", nsec);
        }
    }
}

int main(int argc, char *argv[])
{
    int ii, jj;

    for (ii = 0; ii < 32; ++ii)
    {
        uint32_t xx = 0;

        for (jj = 0; jj < 32; ++jj)
        {
            int bit = (truth[jj] >> (31 - ii)) & 1;
            xx |= bit << (31 - jj);
        }

        input_8[ii] = input_16[2*ii+0] = input_32[4*ii+0] = xx >> 24;
        input_16[2*ii+1] = input_32[4*ii+1] = xx >> 16;
        input_32[4*ii+2] = xx >> 8;
        input_32[4*ii+3] = xx;
    }

    if (argc < 2)
    {
        test_transpose();
    }

    for (jj = 1; jj < argc; ++jj)
    {
        if (!strcmp(argv[jj], "-truth"))
        {
            printf(" Transposed truth:\n  ");
            for (ii = 0; ii < 128; ++ii)
            {
                printf(" 0x%02x,", (unsigned char)input_32[ii]);
                if (ii % 8 == 7) printf("\n  ");
            }
            printf("\n");
        }

        if (!strcmp(argv[jj], "-bench"))
        {
            printf("\nimplementation");
            for (ii = 1; ii < 33; ++ii)
            {
                printf(",%d", ii);
            }
            benchmark_transpose("generic");
            benchmark_transpose(NULL);
            printf("\n");
        }

        if (!strcmp(argv[jj], "-test"))
        {
            test_transpose();
        }
    }

    return EXIT_SUCCESS;
}
