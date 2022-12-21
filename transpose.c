/** transpose.c - Succinct RINEX internal bit-matrix transposition.
 * Copyright 2021 Michael Poole.
 * SPDX-License-Identifier: MIT-Modern-Variant
 */

#include "rinex_p.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

RNX_RESOLVE_VOID(transpose,
    (int64_t *out, const char *in, int bits, int count),
    (out, in, bits, count),
    avx2)

void transpose(int64_t *out, const char *in, int bits, int count)
{
    rnx_transpose(out, in, bits, count);
}
