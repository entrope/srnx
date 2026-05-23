/** bench_digest.c - Benchmark for large digest algorithms.
 * Copyright 2026 Michael Poole.
 * SPDX-License-Identifier: MIT-Modern-Variant
 *
 * Benchmarks digest IDs 4 and 5 over a
 * 40 MiB non-constant input buffer.
 */

#define _POSIX_C_SOURCE 199309L

#include "rinex/digest.h"

#include <blake3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* 40 MiB input buffer */
#define INPUT_SIZE  (40UL * 1024 * 1024)

/* Number of timed iterations per digest after warmup */
#define BENCH_REPS  8

static const int digest_ids[] = { 4, 5 };
static const int n_ids = sizeof digest_ids / sizeof digest_ids[0];

static void fill_input(unsigned char *buf, size_t len)
{
    /* Non-constant, non-repeating pattern derived from a simple LCG
     * so the buffer is not all-zeros and not cache-friendly. */
    uint64_t state = 0x5dee6c915e867a73ULL;
    size_t i;
    for (i = 0; i < len; ++i)
    {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        buf[i] = (unsigned char)(state >> 33);
    }
}

static double elapsed_nsec(struct timespec start, struct timespec end)
{
    return (end.tv_sec - start.tv_sec) * 1e9
        + (end.tv_nsec - start.tv_nsec);
}

int main(void)
{
    unsigned char *input;
    unsigned char out[64];  /* max digest size is BLAKE3-256 = 32 bytes */
    struct timespec t0, t1;
    double nsec;
    int id_idx, rep;

    input = malloc(INPUT_SIZE);
    if (!input)
    {
        fprintf(stderr, "malloc(%zu) failed\n", INPUT_SIZE);
        return 1;
    }
    fill_input(input, INPUT_SIZE);

    /* Print CSV header */
    printf("id,name,output_bytes,input_bytes,reps,ns_per_run,mb_per_sec");

    for (id_idx = 0; id_idx < n_ids; ++id_idx)
    {
        int id = digest_ids[id_idx];
        int out_len = rnx_digest_length(id);
        const char *name;

        if (out_len <= 0)
        {
            fprintf(stderr, "skip unsupported id=%d\n", id);
            continue;
        }

        switch (id)
        {
        case 4:  name = "BLAKE3-128";    break;
        case 5:  name = "BLAKE3-256";    break;
        default: name = "unknown";       break;
        }

        /* Warmup: one full pass so the code path and caches are hot. */
        if (rnx_digest(id, input, INPUT_SIZE, out) < 0)
        {
            fprintf(stderr, "rnx_digest(%d) failed\n", id);
            free(input);
            return 1;
        }

        /* Timed runs */
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (rep = 0; rep < BENCH_REPS; ++rep)
        {
            if (rnx_digest(id, input, INPUT_SIZE, out) < 0)
            {
                fprintf(stderr, "rnx_digest(%d) failed on rep %d\n", id, rep);
                free(input);
                return 1;
            }
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);

        nsec = elapsed_nsec(t0, t1) / BENCH_REPS;
        double mb_sec = (INPUT_SIZE / (1024.0 * 1024.0)) / (nsec / 1e9);

        printf("\n%d,%s,%d,%zu,%d,%.0f,%.1f",
               id, name, out_len, INPUT_SIZE, BENCH_REPS, nsec, mb_sec);
    }

    printf("\n");
    free(input);
    return 0;
}
