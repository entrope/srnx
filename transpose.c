/** transpose.c - Succinct RINEX internal bit-matrix transposition.
 * Copyright 2021 Michael Poole.
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

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef __x86_64__
# include <x86intrin.h>
#endif

#include "transpose.h"

void (*transpose)(int64_t *out, const char *in, int bits, int count);

static void transpose_init(void) __attribute__((constructor));

static void transpose_generic(int64_t *out, const char *in, int bits, int count)
    __attribute__((flatten));

#define TRANSPOSE_U64(X) do { \
        X = (X & 0xAA55AA55AA55AA55) \
            | (X & 0x00AA00AA00AA00AA) << 7 \
            | ((X >> 7) & 0x00AA00AA00AA00AA); \
        X = (X & 0xCCCC3333CCCC3333) \
            | (X & 0x0000CCCC0000CCCC) << 14 \
            | ((X >> 14) & 0x0000CCCC0000CCCC); \
        X = (X & 0xF0F0F0F00F0F0F0F) \
            | (X & 0x00000000F0F0F0F0) << 28 \
            | ((X >> 28) & 0x00000000F0F0F0F0); \
    } while (0)

/* XXX: Is there an elegant way to reduce this code size?  Building a
 * (8*n)x(8*n) transpose from an 8x8 transpose is trivial given input
 * and output strides as in Henry S. Warren Jr's book, but we need a
 * special case for the first block because the first bits are replayed.
 */

static void transpose_8_generic(int64_t *out, const unsigned char *in, int bits);
static void transpose_16_generic(int64_t *out, const unsigned char *in, int bits);
static void transpose_32_generic(int64_t *out, const unsigned char *in, int bits);

/** Transposes and sign-extends an 8-by-`bits` bit matrix.
 *
 * \param[out] out Receives transposed, sign-extended values.
 * \param[in] in Transposed input matrix.
 * \param[in] bits Number of bits (uint8_t's) in \a in.
 */
void transpose_8_generic(int64_t *out, const unsigned char *in, int bits)
{
    /* This dummy initialization silences compiler warnings; we always
     * shift the uninitialized content out of the variables.
     */
    uint64_t x = 0, y = 0, z = 0, w = 0;
    int ii;

    if (bits < 1)
    {
        /* noop */
    }
    else if (bits <= 8)
    {
        for (ii = 0; ii < 8 - bits; ++ii)
        {
            x = (x << 8) | in[0];
        }
        for (; ii < 8; ++ii)
        {
            x = (x << 8) | in[ii + bits - 8];
        }

        TRANSPOSE_U64(x);

        for (ii = 7; ii >= 0; ii -= 1, x >>= 8)
        {
            out[ii] = (int64_t)(x << 56) >> 56;
        }
    }
    else if (bits <= 16) /* and bits > 8 */
    {
        for (ii = 0; ii < 16 - bits; ++ii)
        {
            x = (x << 8) | in[0];
            y = (y << 8) | in[ii + bits - 8];
        }
        for (; ii < 8; ++ii)
        {
            x = (x << 8) | in[ii + bits - 16];
            y = (y << 8) | in[ii + bits - 8];
        }

        TRANSPOSE_U64(x);
        TRANSPOSE_U64(y);

        for (ii = 7; ii >= 0; ii -= 1, x >>= 8, y >>= 8)
        {
            out[ii] = ((int64_t)(x << 56) >> 48) | (y & 255);
        }
    }
    else if (bits <= 24) /* and bits > 16 */
    {
        for (ii = 0; ii < 24 - bits; ++ii)
        {
            x = (x << 8) | in[0];
            y = (y << 8) | in[ii + bits - 16];
            z = (z << 8) | in[ii + bits - 8];
        }
        for (; ii < 8; ++ii)
        {
            x = (x << 8) | in[ii + bits - 24];
            y = (y << 8) | in[ii + bits - 16];
            z = (z << 8) | in[ii + bits - 8];
        }

        TRANSPOSE_U64(x);
        TRANSPOSE_U64(y);
        TRANSPOSE_U64(z);

        for (ii = 7; ii >= 0; ii -= 1, x >>= 8, y >>= 8, z >>= 8)
        {
            out[ii] = ((int64_t)(x << 56) >> 40)
                | ((y & 255) << 8) | (z & 255);
        }
    }
    else /* 24 < bits <= 32 */
    {
        for (ii = 0; ii < 32 - bits; ++ii)
        {
            x = (x << 8) | in[0];
            y = (y << 8) | in[ii + bits - 24];
            z = (z << 8) | in[ii + bits - 16];
            w = (w << 8) | in[ii + bits - 8];
        }
        for (; ii < 8; ++ii)
        {
            x = (x << 8) | in[ii + bits - 32];
            y = (y << 8) | in[ii + bits - 24];
            z = (z << 8) | in[ii + bits - 16];
            w = (w << 8) | in[ii + bits - 8];
        }

        TRANSPOSE_U64(x);
        TRANSPOSE_U64(y);
        TRANSPOSE_U64(z);
        TRANSPOSE_U64(w);

        for (ii = 7; ii >= 0; ii -= 1, x >>= 8, y >>= 8, z >>= 8, w >>= 8)
        {
            out[ii] = ((int64_t)(x << 56) >> 32)
                | ((y & 255) << 16) | ((z & 255) << 8) | (w & 255);
        }
    }
}

/** Transposes and sign-extends a 16-by-`bits` bit matrix.
 *
 * \param[out] out Receives transposed, sign-extended values.
 * \param[in] in Transposed input matrix.
 * \param[in] bits Number of bits (uint16_t's) in \a in.
 */
void transpose_16_generic(int64_t *out, const unsigned char *in, int bits)
{
    uint64_t x = 0, y = 0, z = 0, w = 0;
    uint64_t x2 = 0, y2 = 0, z2 = 0, w2 = 0;
    int ii;

    if (bits < 1)
    {
        /* noop */
    }
    else if (bits <= 8)
    {
        for (ii = 0; ii < 8 - bits; ++ii)
        {
            x = (x << 8) | in[0];
            y = (y << 8) | in[1];
        }
        for (; ii < 8; ++ii)
        {
            x = (x << 8) | in[2*(ii + bits - 8)+0];
            y = (y << 8) | in[2*(ii + bits - 8)+1];
        }

        TRANSPOSE_U64(x);
        TRANSPOSE_U64(y);

        for (ii = 7; ii >= 0; ii -= 1, x >>= 8, y >>= 8)
        {
            out[ii+0] = (int64_t)(x << 56) >> 56;
            out[ii+8] = (int64_t)(y << 56) >> 56;
        }
    }
    else if (bits <= 16)
    {
        for (ii = 0; ii < 16 - bits; ++ii)
        {
            x = (x << 8) | in[0];
            y = (y << 8) | in[1];
            z = (z << 8) | in[2*(ii + bits - 8)+0];
            w = (w << 8) | in[2*(ii + bits - 8)+1];
        }
        for (; ii < 8; ++ii)
        {
            x = (x << 8) | in[2*(ii + bits - 16)+0];
            y = (y << 8) | in[2*(ii + bits - 16)+1];
            z = (z << 8) | in[2*(ii + bits - 8)+0];
            w = (w << 8) | in[2*(ii + bits - 8)+1];
        }

        TRANSPOSE_U64(x);
        TRANSPOSE_U64(y);
        TRANSPOSE_U64(z);
        TRANSPOSE_U64(w);

        for (ii = 7; ii >= 0; ii -= 1, x >>= 8, y >>= 8, z >>= 8, w >>= 8)
        {
            out[ii+0] = ((int64_t)(x << 56) >> 48) | (z & 255);
            out[ii+8] = ((int64_t)(y << 56) >> 48) | (w & 255);
        }
    }
    else if (bits <= 24)
    {
        for (ii = 0; ii < 24 - bits; ++ii)
        {
            x = (x << 8) | in[0];
            y = (y << 8) | in[1];
            z = (z << 8) | in[2*(ii + bits - 16)+0];
            w = (w << 8) | in[2*(ii + bits - 16)+1];
            x2 = (x2 << 8) | in[2*(ii + bits - 8)+0];
            y2 = (y2 << 8) | in[2*(ii + bits - 8)+1];
        }
        for (; ii < 8; ++ii)
        {
            x = (x << 8) | in[2*(ii + bits - 24)+0];
            y = (y << 8) | in[2*(ii + bits - 24)+1];
            z = (z << 8) | in[2*(ii + bits - 16)+0];
            w = (w << 8) | in[2*(ii + bits - 16)+1];
            x2 = (x2 << 8) | in[2*(ii + bits - 8)+0];
            y2 = (y2 << 8) | in[2*(ii + bits - 8)+1];
        }

        TRANSPOSE_U64(x);
        TRANSPOSE_U64(y);
        TRANSPOSE_U64(z);
        TRANSPOSE_U64(w);
        TRANSPOSE_U64(x2);
        TRANSPOSE_U64(y2);

        for (ii = 7; ii >= 0; ii -= 1, x >>= 8, y >>= 8, z >>= 8, w >>= 8)
        {
            out[ii+0] = ((int64_t)(x << 56) >> 40) | ((z & 255) << 8) | (x2 & 255);
            out[ii+8] = ((int64_t)(y << 56) >> 40) | ((w & 255) << 8) | (y2 & 255);
            x2 >>= 8;
            y2 >>= 8;
        }
    }
    else /* 24 < bits <= 32 */
    {
        for (ii = 0; ii < 32 - bits; ++ii)
        {
            x = (x << 8) | in[0];
            y = (y << 8) | in[1];
            z = (z << 8) | in[2*(ii + bits - 24)+0];
            w = (w << 8) | in[2*(ii + bits - 24)+1];
            x2 = (x2 << 8) | in[2*(ii + bits - 16)+0];
            y2 = (y2 << 8) | in[2*(ii + bits - 16)+1];
            z2 = (z2 << 8) | in[2*(ii + bits - 8)+0];
            w2 = (w2 << 8) | in[2*(ii + bits - 8)+1];
        }
        for (; ii < 8; ++ii)
        {
            x = (x << 8) | in[2*(ii + bits - 32)+0];
            y = (y << 8) | in[2*(ii + bits - 32)+1];
            z = (z << 8) | in[2*(ii + bits - 24)+0];
            w = (w << 8) | in[2*(ii + bits - 24)+1];
            x2 = (x2 << 8) | in[2*(ii + bits - 16)+0];
            y2 = (y2 << 8) | in[2*(ii + bits - 16)+1];
            z2 = (z2 << 8) | in[2*(ii + bits - 8)+0];
            w2 = (w2 << 8) | in[2*(ii + bits - 8)+1];
        }

        TRANSPOSE_U64(x);
        TRANSPOSE_U64(y);
        TRANSPOSE_U64(z);
        TRANSPOSE_U64(w);
        TRANSPOSE_U64(x2);
        TRANSPOSE_U64(y2);
        TRANSPOSE_U64(z2);
        TRANSPOSE_U64(w2);

        for (ii = 7; ii >= 0; ii -= 1, x >>= 8, y >>= 8, z >>= 8, w >>= 8)
        {
            out[ii+0] = ((int64_t)(x << 56) >> 32) | ((z & 255) << 16)
                | ((x2 & 255) << 8) | (z2 & 255);
            out[ii+8] = ((int64_t)(y << 56) >> 32) | ((w & 255) << 16)
                | ((y2 & 255) << 8) | (w2 & 255);
            x2 >>= 8;
            y2 >>= 8;
            z2 >>= 8;
            w2 >>= 8;
        }
    }
}

/** Transposes and sign-extends a 32 by `n` bit matrix.
 *
 * \param[out] out Receives transposed, sign-extended values.
 * \param[in] in Transposed input matrix.
 * \param[in] bits Number of bits (uint32_t's) in \a in.
 */
void transpose_32_generic(int64_t *out, const unsigned char *in, int bits)
{
    /* We only use eight registers because there is no real gain from
     * trying to use more; the compiler tends to spill them, and the
     * inner transpose operations do not overlap any more.
     */
    uint64_t x = 0, y = 0, z = 0, w = 0;
    uint64_t x2 = 0, y2 = 0, z2 = 0, w2 = 0;
    int ii;

    if (bits < 1)
    {
        /* noop */
    }
    else if (bits <= 8)
    {
        for (ii = 0; ii < 8 - bits; ++ii)
        {
            x = (x << 8) | in[0];
            y = (y << 8) | in[1];
            z = (z << 8) | in[2];
            w = (w << 8) | in[3];
        }
        for (; ii < 8; ++ii)
        {
            x = (x << 8) | in[4*(ii + bits - 8)+0];
            y = (y << 8) | in[4*(ii + bits - 8)+1];
            z = (z << 8) | in[4*(ii + bits - 8)+2];
            w = (w << 8) | in[4*(ii + bits - 8)+3];
        }

        TRANSPOSE_U64(x);
        TRANSPOSE_U64(y);
        TRANSPOSE_U64(z);
        TRANSPOSE_U64(w);

        for (ii = 7; ii >= 0; ii -= 1, x >>= 8, y >>= 8, z >>= 8, w >>= 8)
        {
            out[ii+0] = (int64_t)(x << 56) >> 56;
            out[ii+8] = (int64_t)(y << 56) >> 56;
            out[ii+16] = (int64_t)(z << 56) >> 56;
            out[ii+24] = (int64_t)(w << 56) >> 56;
        }
    }
    else if (bits <= 16)
    {
        for (ii = 0; ii < 16 - bits; ++ii)
        {
            x = (x << 8) | in[0];
            y = (y << 8) | in[1];
            z = (z << 8) | in[2];
            w = (w << 8) | in[3];
            x2 = (x2 << 8) | in[4*(ii + bits - 8)+0];
            y2 = (y2 << 8) | in[4*(ii + bits - 8)+1];
            z2 = (z2 << 8) | in[4*(ii + bits - 8)+2];
            w2 = (w2 << 8) | in[4*(ii + bits - 8)+3];
        }
        for (; ii < 8; ++ii)
        {
            x = (x << 8) | in[4*(ii + bits - 16)+0];
            y = (y << 8) | in[4*(ii + bits - 16)+1];
            z = (z << 8) | in[4*(ii + bits - 16)+2];
            w = (w << 8) | in[4*(ii + bits - 16)+3];
            x2 = (x2 << 8) | in[4*(ii + bits - 8)+0];
            y2 = (y2 << 8) | in[4*(ii + bits - 8)+1];
            z2 = (z2 << 8) | in[4*(ii + bits - 8)+2];
            w2 = (w2 << 8) | in[4*(ii + bits - 8)+3];
        }

        TRANSPOSE_U64(x);
        TRANSPOSE_U64(y);
        TRANSPOSE_U64(z);
        TRANSPOSE_U64(w);
        TRANSPOSE_U64(x2);
        TRANSPOSE_U64(y2);
        TRANSPOSE_U64(z2);
        TRANSPOSE_U64(w2);

        for (ii = 7; ii >= 0; ii -= 1, x >>= 8, y >>= 8, z >>= 8, w >>= 8)
        {
            out[ii+0]  = ((int64_t)(x << 56) >> 48) | (x2 & 255);
            out[ii+8]  = ((int64_t)(y << 56) >> 48) | (y2 & 255);
            out[ii+16] = ((int64_t)(z << 56) >> 48) | (z2 & 255);
            out[ii+24] = ((int64_t)(w << 56) >> 48) | (w2 & 255);
            x2 >>= 8;
            y2 >>= 8;
            z2 >>= 8;
            w2 >>= 8;
        }
    }
    else if (bits <= 24)
    {
        /* Process the first 16 columns. */
        for (ii = 0; ii < 24 - bits; ++ii)
        {
            x = (x << 8) | in[0];
            y = (y << 8) | in[1];
            z = (z << 8) | in[4*(ii + bits - 16)+0];
            w = (w << 8) | in[4*(ii + bits - 16)+1];
            x2 = (x2 << 8) | in[4*(ii + bits - 8)+0];
            y2 = (y2 << 8) | in[4*(ii + bits - 8)+1];
        }
        for (; ii < 8; ++ii)
        {
            x = (x << 8) | in[4*(ii + bits - 24)+0];
            y = (y << 8) | in[4*(ii + bits - 24)+1];
            z = (z << 8) | in[4*(ii + bits - 16)+0];
            w = (w << 8) | in[4*(ii + bits - 16)+1];
            x2 = (x2 << 8) | in[4*(ii + bits - 8)+0];
            y2 = (y2 << 8) | in[4*(ii + bits - 8)+1];
        }

        TRANSPOSE_U64(x);
        TRANSPOSE_U64(y);
        TRANSPOSE_U64(z);
        TRANSPOSE_U64(w);
        TRANSPOSE_U64(x2);
        TRANSPOSE_U64(y2);

        for (ii = 7; ii >= 0; ii -= 1, x >>= 8, y >>= 8, z >>= 8, w >>= 8)
        {
            out[ii+0] = ((int64_t)(x << 56) >> 40) | ((z & 255) << 8) | (x2 & 255);
            out[ii+8] = ((int64_t)(y << 56) >> 40) | ((w & 255) << 8) | (y2 & 255);
            x2 >>= 8;
            y2 >>= 8;
        }

        /* Repeat for the last 16 columns. */
        for (ii = 0; ii < 24 - bits; ++ii)
        {
            x = (x << 8) | in[2];
            y = (y << 8) | in[3];
            z = (z << 8) | in[4*(ii + bits - 16)+2];
            w = (w << 8) | in[4*(ii + bits - 16)+3];
            x2 = (x2 << 8) | in[4*(ii + bits - 8)+2];
            y2 = (y2 << 8) | in[4*(ii + bits - 8)+3];
        }
        for (; ii < 8; ++ii)
        {
            x = (x << 8) | in[4*(ii + bits - 24)+2];
            y = (y << 8) | in[4*(ii + bits - 24)+3];
            z = (z << 8) | in[4*(ii + bits - 16)+2];
            w = (w << 8) | in[4*(ii + bits - 16)+3];
            x2 = (x2 << 8) | in[4*(ii + bits - 8)+2];
            y2 = (y2 << 8) | in[4*(ii + bits - 8)+3];
        }

        TRANSPOSE_U64(x);
        TRANSPOSE_U64(y);
        TRANSPOSE_U64(z);
        TRANSPOSE_U64(w);
        TRANSPOSE_U64(x2);
        TRANSPOSE_U64(y2);

        for (ii = 7; ii >= 0; ii -= 1, x >>= 8, y >>= 8, z >>= 8, w >>= 8)
        {
            out[ii+16] = ((int64_t)(x << 56) >> 40) | ((z & 255) << 8) | (x2 & 255);
            out[ii+24] = ((int64_t)(y << 56) >> 40) | ((w & 255) << 8) | (y2 & 255);
            x2 >>= 8;
            y2 >>= 8;
        }
    }
    else /* 24 < bits <= 32 */
    {
        /* Process the first 16 columns. */
        for (ii = 0; ii < 32 - bits; ++ii)
        {
            x = (x << 8) | in[0];
            y = (y << 8) | in[1];
            z = (z << 8) | in[4*(ii + bits - 24)+0];
            w = (w << 8) | in[4*(ii + bits - 24)+1];
            x2 = (x2 << 8) | in[4*(ii + bits - 16)+0];
            y2 = (y2 << 8) | in[4*(ii + bits - 16)+1];
            z2 = (z2 << 8) | in[4*(ii + bits - 8)+0];
            w2 = (w2 << 8) | in[4*(ii + bits - 8)+1];
        }
        for (; ii < 8; ++ii)
        {
            x = (x << 8) | in[4*(ii + bits - 32)+0];
            y = (y << 8) | in[4*(ii + bits - 32)+1];
            z = (z << 8) | in[4*(ii + bits - 24)+0];
            w = (w << 8) | in[4*(ii + bits - 24)+1];
            x2 = (x2 << 8) | in[4*(ii + bits - 16)+0];
            y2 = (y2 << 8) | in[4*(ii + bits - 16)+1];
            z2 = (z2 << 8) | in[4*(ii + bits - 8)+0];
            w2 = (w2 << 8) | in[4*(ii + bits - 8)+1];
        }

        TRANSPOSE_U64(x);
        TRANSPOSE_U64(y);
        TRANSPOSE_U64(z);
        TRANSPOSE_U64(w);
        TRANSPOSE_U64(x2);
        TRANSPOSE_U64(y2);
        TRANSPOSE_U64(z2);
        TRANSPOSE_U64(w2);

        for (ii = 7; ii >= 0; ii -= 1, x >>= 8, y >>= 8, z >>= 8, w >>= 8)
        {
            out[ii+0]  = ((int64_t)(x << 56) >> 32) | ((z & 255) << 16)
                | ((x2 & 255) << 8) | (z2 & 255);
            out[ii+8]  = ((int64_t)(y << 56) >> 32) | ((w & 255) << 16)
                | ((y2 & 255) << 8) | (w2 & 255);
            x2 >>= 8;
            y2 >>= 8;
            z2 >>= 8;
            w2 >>= 8;
        }

        /* Repeat for the last 16 columns. */
        for (ii = 0; ii < 32 - bits; ++ii)
        {
            x = (x << 8) | in[2];
            y = (y << 8) | in[3];
            z = (z << 8) | in[4*(ii + bits - 24)+2];
            w = (w << 8) | in[4*(ii + bits - 24)+3];
            x2 = (x2 << 8) | in[4*(ii + bits - 16)+2];
            y2 = (y2 << 8) | in[4*(ii + bits - 16)+3];
            z2 = (z2 << 8) | in[4*(ii + bits - 8)+2];
            w2 = (w2 << 8) | in[4*(ii + bits - 8)+3];
        }
        for (; ii < 8; ++ii)
        {
            x = (x << 8) | in[4*(ii + bits - 32)+2];
            y = (y << 8) | in[4*(ii + bits - 32)+3];
            z = (z << 8) | in[4*(ii + bits - 24)+2];
            w = (w << 8) | in[4*(ii + bits - 24)+3];
            x2 = (x2 << 8) | in[4*(ii + bits - 16)+2];
            y2 = (y2 << 8) | in[4*(ii + bits - 16)+3];
            z2 = (z2 << 8) | in[4*(ii + bits - 8)+2];
            w2 = (w2 << 8) | in[4*(ii + bits - 8)+3];
        }

        TRANSPOSE_U64(x);
        TRANSPOSE_U64(y);
        TRANSPOSE_U64(z);
        TRANSPOSE_U64(w);
        TRANSPOSE_U64(x2);
        TRANSPOSE_U64(y2);
        TRANSPOSE_U64(z2);
        TRANSPOSE_U64(w2);

        for (ii = 7; ii >= 0; ii -= 1, x >>= 8, y >>= 8, z >>= 8, w >>= 8)
        {
            out[ii+16]  = ((int64_t)(x << 56) >> 32) | ((z & 255) << 16)
                | ((x2 & 255) << 8) | (z2 & 255);
            out[ii+24]  = ((int64_t)(y << 56) >> 32) | ((w & 255) << 16)
                | ((y2 & 255) << 8) | (w2 & 255);
            x2 >>= 8;
            y2 >>= 8;
            z2 >>= 8;
            w2 >>= 8;
        }
    }
}

void transpose_generic(int64_t *out, const char *in, int bits, int count)
{
    const unsigned char *cu_in = (const unsigned char *)in;

    switch (count)
    {
    case 8:  transpose_8_generic (out, cu_in, bits); break;
    case 16: transpose_16_generic(out, cu_in, bits); break;
    case 32: transpose_32_generic(out, cu_in, bits); break;
    }
}

#ifdef __x86_64__

static void transpose_avx2(int64_t *out, const char *in, int bits, int count)
    __attribute__((flatten, target("avx2")));

static const char shuffle_8[31] __attribute__((aligned(16))) = {
    15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

static const char shuffle_16[32] __attribute__((aligned(16))) = {
    0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
};

static const char shuffle_32[80] __attribute__((aligned(16))) = {
    15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0,
    3, 2, 1, 0, 3, 2, 1, 0, 3, 2, 1, 0, 3, 2, 1, 0,
    7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12, 0, 0, 0, 0,
    0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3,
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
};

static void transpose_8_avx2(int64_t out[8], const char in[], int nbits)
    __attribute__((target("avx2")));
static void transpose_16_avx2(int64_t out[16], const char in[], int nbits)
    __attribute__((target("avx2")));
static void transpose_32_avx2(int64_t out[32], const char in[], int nbits)
    __attribute__((target("avx2")));

/* SSE and AVX don't have native forms of these instructions, because
 * the bit-cast between int and float is supposedly a noop.
 */

static inline int _mm_movemask_epi32(__m128i x)
    __attribute__((target("avx2")));
static inline int _mm_movemask_epi32(__m128i x)
{
    return _mm_movemask_ps(_mm_castsi128_ps(x));
}

static inline int _mm256_movemask_epi32(__m256i x)
    __attribute__((target("avx2")));
static inline int _mm256_movemask_epi32(__m256i x)
{
    return _mm256_movemask_ps(_mm256_castsi256_ps(x));
}

/* Transpose an 8-by-nbits (each 8 bits adjacent) matrix to \a out[0..7].
 * The nbit-bit values are sign-extended.
 */
void transpose_8_avx2(int64_t out[8], const char in[], int nbits)
{
    int ii;

    /* Hat tip to Mischa Sandberg (mischasan) for these inner loops. */
    if (nbits <= 16)
    {
        /* Use only 128-bit operations for power efficiency. */
        __m128i perm = _mm_loadu_si128((const __m128i *)(shuffle_8 + 16 - nbits));
        __m128i v_in = _mm_loadu_si128((const __m128i *)in);
        v_in = _mm_shuffle_epi8(v_in, perm);
        for (ii = 0; ii < 8; ++ii)
        {
            out[ii] = (int16_t)_mm_movemask_epi8(v_in);
            v_in = _mm_slli_epi64(v_in, 1);
        }
    }
    else /* nbits <= 32, use an AVX2 register */
    {
        __m256i perm = _mm256_loadu2_m128i(
            (const __m128i *)(shuffle_8 + 32 - nbits),
            (const __m128i *)shuffle_8);
        __m256i v_in = _mm256_loadu2_m128i((const __m128i *)in,
            (const __m128i *)(in + nbits - 16));
        v_in = _mm256_shuffle_epi8(v_in, perm);
        for (ii = 0; ii < 8; ++ii)
        {
            out[ii] = _mm256_movemask_epi8(v_in);
            v_in = _mm256_slli_epi64(v_in, 1);
        }
    }
}

/* Transpose a 16-by-nbits (each 16 bits adjacent) matrix to \a out[0..15].
 * The nbit-bit values are sign-extended.
 */
void transpose_16_avx2(int64_t out[16], const char in[], int nbits)
{
    int ii;

    /* AVX and SSE have nothing like _mm_movemask_epi16(), so we use a
     * different approach for transposing blocks of 16 numbers.
     *
     * This approach described in "Hacker's Delight" by Henry S. Warren, Jr.,
     * (section 7-3), based on an approach credited to Guy Steele.  We
     * need to byte-swap the input and output to make the 64-bit
     * arithmetic work properly for the SRNX bit ordering.
     */
    if (nbits <= 8)
    {
        /* 16*nbits <= 128, so we only need 128-bit ops initially... */
        const __m128i c1 = _mm_set1_epi64x(0xAA55AA55AA55AA55);
        const __m128i c2 = _mm_set1_epi64x(0x00AA00AA00AA00AA);
        const __m128i c3 = _mm_set1_epi64x(0xCCCC3333CCCC3333);
        const __m128i c4 = _mm_set1_epi64x(0x0000CCCC0000CCCC);
        const __m128i c5 = _mm_set1_epi64x(0xF0F0F0F00F0F0F0F);
        const __m128i c6 = _mm_set1_epi64x(0x00000000F0F0F0F0);
        const __m128i p1 = _mm_setr_epi8(14, 12, 10, 8, 6, 4, 2, 0,
            15, 13, 11, 9, 7, 5, 3, 1);
        const __m128i p2 = _mm_setr_epi8(7, 6, 5, 4, 3, 2, 1, 0,
            15, 14, 13, 12, 11, 10, 9, 8);
        const __m128i perm = _mm_loadu_si128((const __m128i *)(shuffle_16 + nbits * 2));

        __m128i v_in = _mm_loadu_si128((const __m128i *)in);
        v_in = _mm_shuffle_epi8(v_in, perm);
        v_in = _mm_shuffle_epi8(v_in, p1);

        const __m128i x1 = _mm_or_si128(_mm_and_si128(v_in, c1),
            _mm_or_si128(_mm_slli_epi64(_mm_and_si128(v_in, c2), 7),
            _mm_and_si128(_mm_srli_epi64(v_in, 7), c2)));
        const __m128i x2 = _mm_or_si128(_mm_and_si128(x1, c3),
            _mm_or_si128(_mm_slli_epi64(_mm_and_si128(x1, c4), 14),
            _mm_and_si128(_mm_srli_epi64(x1, 14), c4)));
        const __m128i x3 = _mm_or_si128(_mm_and_si128(x2, c5),
            _mm_or_si128(_mm_slli_epi64(_mm_and_si128(x2, c6), 28),
            _mm_and_si128(_mm_srli_epi64(x2, 28), c6)));
        const __m128i t1 = _mm_shuffle_epi8(x3, p2);

        /* ... but once we get to this point, AVX2 is easier. */
        _mm256_storeu_si256((__m256i *)out, _mm256_cvtepi8_epi64(t1));
        const __m128i t2 = _mm_shuffle_epi32(t1, 0x55);
        _mm256_storeu_si256((__m256i *)(out + 4), _mm256_cvtepi8_epi64(t2));
        const __m128i t3 = _mm_shuffle_epi32(t1, 0xAA);
        _mm256_storeu_si256((__m256i *)(out + 8), _mm256_cvtepi8_epi64(t3));
        const __m128i t4 = _mm_shuffle_epi32(t1, 0xFF);
        _mm256_storeu_si256((__m256i *)(out + 12), _mm256_cvtepi8_epi64(t4));

        return;
    }

    const __m256i p1 = _mm256_setr_epi8(
        14, 12, 10, 8, 6, 4, 2, 0, 15, 13, 11, 9, 7, 5, 3, 1,
        14, 12, 10, 8, 6, 4, 2, 0, 15, 13, 11, 9, 7, 5, 3, 1);
    if (nbits <= 16) /* fits within one AVX2 register */
    {
        const __m256i c1 = _mm256_set1_epi64x(0xAA55AA55AA55AA55);
        const __m256i c2 = _mm256_set1_epi64x(0x00AA00AA00AA00AA);
        const __m256i c3 = _mm256_set1_epi64x(0xCCCC3333CCCC3333);
        const __m256i c4 = _mm256_set1_epi64x(0x0000CCCC0000CCCC);
        const __m256i c5 = _mm256_set1_epi64x(0xF0F0F0F00F0F0F0F);
        const __m256i c6 = _mm256_set1_epi64x(0x00000000F0F0F0F0);
        const __m256i p2 = _mm256_setr_epi8(
            7, 15, 6, 14, 5, 13, 4, 12, 3, 11, 2, 10, 1, 9, 0, 8,
            7, 15, 6, 14, 5, 13, 4, 12, 3, 11, 2, 10, 1, 9, 0, 8);
        const __m256i perm = _mm256_loadu2_m128i(
            (const __m128i *)(shuffle_16 + nbits * 2 - 16),
            (const __m128i *)(shuffle_16 + 16));

        __m256i v_in = _mm256_loadu2_m128i(
            (const __m128i *)in,
            (const __m128i *)(in + nbits * 2 - 16));
        v_in = _mm256_shuffle_epi8(v_in, perm);
        v_in = _mm256_shuffle_epi8(v_in, p1);

        const __m256i x1 = _mm256_or_si256(_mm256_and_si256(v_in, c1),
            _mm256_or_si256(_mm256_slli_epi64(_mm256_and_si256(v_in, c2), 7),
            _mm256_and_si256(_mm256_srli_epi64(v_in, 7), c2)));
        const __m256i x2 = _mm256_or_si256(_mm256_and_si256(x1, c3),
            _mm256_or_si256(_mm256_slli_epi64(_mm256_and_si256(x1, c4), 14),
            _mm256_and_si256(_mm256_srli_epi64(x1, 14), c4)));
        const __m256i x3 = _mm256_or_si256(_mm256_and_si256(x2, c5),
            _mm256_or_si256(_mm256_slli_epi64(_mm256_and_si256(x2, c6), 28),
            _mm256_and_si256(_mm256_srli_epi64(x2, 28), c6)));
        const __m256i x4 = _mm256_permute4x64_epi64(x3, 0xd8);
        const __m256i t_w = _mm256_shuffle_epi8(x4, p2);

        const __m128i t1 = _mm256_castsi256_si128(t_w);
        _mm256_storeu_si256((__m256i *)out, _mm256_cvtepi16_epi64(t1));
        const __m128i t2 = _mm_shuffle_epi32(t1, 0xEE);
        _mm256_storeu_si256((__m256i *)(out + 4), _mm256_cvtepi16_epi64(t2));
        const __m128i t3 = _mm256_extracti128_si256(t_w, 1);
        _mm256_storeu_si256((__m256i *)(out + 8), _mm256_cvtepi16_epi64(t3));
        const __m128i t4 = _mm_shuffle_epi32(t3, 0xEE);
        _mm256_storeu_si256((__m256i *)(out + 12), _mm256_cvtepi16_epi64(t4));

        return;
    }

    /* needs two AVX2 registers; how to get the MSBs depends on nbits,
     * but it's always a win to use _mm256_movemask_epi8().
     *
     * FIXME: This ends up being slightly slower than transpose_32_avx2(..., nbits).
     */
    __m256i perm, v_hi;
    if (nbits <= 24)
    {
        perm = _mm256_loadu2_m128i(
            (const __m128i *)(shuffle_16 + nbits * 2 - 32),
            (const __m128i *)shuffle_16);
        v_hi = _mm256_broadcastsi128_si256(
            _mm_loadu_si128((const __m128i *)in));
    }
    else /* 24 < nbits <= 32 */
    {
        perm = _mm256_loadu2_m128i(
            (const __m128i *)(shuffle_16 + 16),
            (const __m128i *)(shuffle_16 + nbits * 2 - 48));
        v_hi = _mm256_loadu2_m128i((const __m128i *)(in + nbits * 2 - 48),
            (const __m128i *)in);
    }
    const __m256i v_lo = _mm256_lddqu_si256((const __m256i *)(in + nbits * 2 - 32));
    const __m256i v_th = _mm256_shuffle_epi8(v_hi, perm);
    const __m256i v_pl = _mm256_shuffle_epi8(v_lo, p1);
    const __m256i v_ph = _mm256_shuffle_epi8(v_th, p1);
    const __m256i u_lo = _mm256_unpacklo_epi64(v_ph, v_pl);
    const __m256i u_hi = _mm256_unpackhi_epi64(v_ph, v_pl);
    __m256i u_pl = _mm256_permute4x64_epi64(u_lo, 0x27);
    __m256i u_ph = _mm256_permute4x64_epi64(u_hi, 0x27);
    for (ii = 0; ii < 8; ++ii)
    {
        out[ii+0] = _mm256_movemask_epi8(u_pl);
        out[ii+8] = _mm256_movemask_epi8(u_ph);
        u_pl = _mm256_slli_epi64(u_pl, 1);
        u_ph = _mm256_slli_epi64(u_ph, 1);
    }
}

/* Transpose a 32-by-nbits (each 32 bits adjacent) matrix to \a out[0..31].
 * The nbit-bit values are sign-extended.
 */
void transpose_32_avx2(int64_t out[32], const char in[], int nbits)
{
    int ii;

    /* The "128-bit lane" nature of AVX really hurts here.  Only 4 rows
     * of 32 bits fit into a 128-bit lane, so the branching goes down to
     * nbits/4.  nbits < 9 looks mostly like transpose_8_avx2(), and
     * nbits > 8 looks more like transpose_16_avx2().
     */

    if (nbits < 1)
    {
        /* noop */
    }
    else if (nbits < 17)
    {
        /* FIXME: These are slower than 16 < nbits <= 24, or even <= 32. */
        if (nbits < 9)
        {
            /* Up to 8 bits looks like transpose_8_avx2(). */
            if (nbits < 5) /* 1 to 4 bits: one SSE register */
            {
                __m128i perm = _mm_loadu_si128((const __m128i *)(shuffle_32 + 16 - nbits * 4));
                __m128i v_in = _mm_loadu_si128((const __m128i *)in);
                v_in = _mm_shuffle_epi8(v_in, perm);
                for (ii = 0; ii < 32; ++ii)
                {
                    out[ii] = _mm_movemask_epi32(v_in) << 28 >> 28;
                    v_in = _mm_slli_epi64(v_in, 1);
                }
            }
            else /* 5 to 8 bits: one AVX register */
            {
                __m256i perm = _mm256_loadu2_m128i(
                    (const __m128i *)(shuffle_32 + 32 - nbits * 4),
                    (const __m128i *)shuffle_32);
                __m256i v_in = _mm256_loadu2_m128i(
                    (const __m128i *)in,
                    (const __m128i *)(in + nbits * 4 - 16));
                v_in = _mm256_shuffle_epi8(v_in, perm);
                for (ii = 0; ii < 32; ++ii)
                {
                    out[ii] = (char)_mm256_movemask_epi32(v_in);
                    v_in = _mm256_slli_epi64(v_in, 1);
                }
            }
        }
        else /* 8 < nbits < 17 */
        {
            /* nbits > 8 looks more like transpose_16_avx2() */
            const __m256i c1 = _mm256_set1_epi64x(0xAA55AA55AA55AA55);
            const __m256i c2 = _mm256_set1_epi64x(0x00AA00AA00AA00AA);
            const __m256i c3 = _mm256_set1_epi64x(0xCCCC3333CCCC3333);
            const __m256i c4 = _mm256_set1_epi64x(0x0000CCCC0000CCCC);
            const __m256i c5 = _mm256_set1_epi64x(0xF0F0F0F00F0F0F0F);
            const __m256i c6 = _mm256_set1_epi64x(0x00000000F0F0F0F0);

            /* 9 to 16 bits: up to two AVX registers; we save no
             * logic by trying to use one SSE and one AVX, so just
             * deinterleave into two YMMs and then expand the results.
             */
            __m256i perm, v_hi, v_lo;
            if (nbits < 13)
            {
                perm = _mm256_loadu2_m128i(
                    (const __m128i *)(shuffle_32 + nbits * 4 + 16),
                    (const __m128i *)(shuffle_32 + 48));
                v_hi = _mm256_broadcastsi128_si256(
                    _mm_loadu_si128((const __m128i *)in));
            }
            else /* 12 < nbits < 17 */
            {
                perm = _mm256_loadu2_m128i(
                    (const __m128i *)(shuffle_32 + 64),
                    (const __m128i *)(shuffle_32 + nbits * 4));
                v_hi = _mm256_loadu2_m128i(
                    (const __m128i *)(in + nbits * 4 - 48),
                    (const __m128i *)in);
            }
            v_lo = _mm256_lddqu_si256((const __m256i *)(in + nbits * 4 - 32));
            v_hi = _mm256_shuffle_epi8(v_hi, perm);

            const __m256i p1 = _mm256_setr_epi8(
                12, 8, 4, 0, 13, 9, 5, 1, 14, 10, 6, 2, 15, 11, 7, 3,
                12, 8, 4, 0, 13, 9, 5, 1, 14, 10, 6, 2, 15, 11, 7, 3);
            v_hi = _mm256_shuffle_epi8(v_hi, p1);
            v_lo = _mm256_shuffle_epi8(v_lo, p1);

            const __m256i p2 = _mm256_setr_epi32(4, 0, 5, 1, 6, 2, 7, 3);
            v_hi = _mm256_permutevar8x32_epi32(v_hi, p2);
            v_lo = _mm256_permutevar8x32_epi32(v_lo, p2);

            const __m256i x1_hi = _mm256_or_si256(_mm256_and_si256(v_hi, c1),
                _mm256_or_si256(_mm256_slli_epi64(_mm256_and_si256(v_hi, c2), 7),
                _mm256_and_si256(_mm256_srli_epi64(v_hi, 7), c2)));
            const __m256i x1_lo = _mm256_or_si256(_mm256_and_si256(v_lo, c1),
                _mm256_or_si256(_mm256_slli_epi64(_mm256_and_si256(v_lo, c2), 7),
                _mm256_and_si256(_mm256_srli_epi64(v_lo, 7), c2)));

            const __m256i x2_hi = _mm256_or_si256(_mm256_and_si256(x1_hi, c3),
                _mm256_or_si256(_mm256_slli_epi64(_mm256_and_si256(x1_hi, c4), 14),
                _mm256_and_si256(_mm256_srli_epi64(x1_hi, 14), c4)));
            const __m256i x2_lo = _mm256_or_si256(_mm256_and_si256(x1_lo, c3),
                _mm256_or_si256(_mm256_slli_epi64(_mm256_and_si256(x1_lo, c4), 14),
                _mm256_and_si256(_mm256_srli_epi64(x1_lo, 14), c4)));

            const __m256i x3_hi = _mm256_or_si256(_mm256_and_si256(x2_hi, c5),
                _mm256_or_si256(_mm256_slli_epi64(_mm256_and_si256(x2_hi, c6), 28),
                _mm256_and_si256(_mm256_srli_epi64(x2_hi, 28), c6)));
            const __m256i x3_lo = _mm256_or_si256(_mm256_and_si256(x2_lo, c5),
                _mm256_or_si256(_mm256_slli_epi64(_mm256_and_si256(x2_lo, c6), 28),
                _mm256_and_si256(_mm256_srli_epi64(x2_lo, 28), c6)));

            const __m256i p3 = _mm256_setr_epi8(
                7, 6, 5, 4, 3, 2, 1, 0, 15, 14, 13, 12, 11, 10, 9, 8,
                7, 6, 5, 4, 3, 2, 1, 0, 15, 14, 13, 12, 11, 10, 9, 8);
            const __m256i x4_hi = _mm256_shuffle_epi8(x3_hi, p3);
            const __m256i x4_lo = _mm256_shuffle_epi8(x3_lo, p3);

            const __m256i x5_hi = _mm256_unpacklo_epi8(x4_lo, x4_hi);
            const __m256i x5_lo = _mm256_unpackhi_epi8(x4_lo, x4_hi);

            const __m128i t1 = _mm256_castsi256_si128(x5_hi);
            _mm256_storeu_si256((__m256i *)out, _mm256_cvtepi16_epi64(t1));
            const __m128i t2 = _mm_shuffle_epi32(t1, 0xEE);
            _mm256_storeu_si256((__m256i *)(out + 4), _mm256_cvtepi16_epi64(t2));
            const __m128i t3 = _mm256_castsi256_si128(x5_lo);
            _mm256_storeu_si256((__m256i *)(out + 8), _mm256_cvtepi16_epi64(t3));
            const __m128i t4 = _mm_shuffle_epi32(t3, 0xEE);
            _mm256_storeu_si256((__m256i *)(out + 12), _mm256_cvtepi16_epi64(t4));
            const __m128i t5 = _mm256_extracti128_si256(x5_hi, 1);
            _mm256_storeu_si256((__m256i *)(out + 16), _mm256_cvtepi16_epi64(t5));
            const __m128i t6 = _mm_shuffle_epi32(t5, 0xEE);
            _mm256_storeu_si256((__m256i *)(out + 20), _mm256_cvtepi16_epi64(t6));
            const __m128i t7 = _mm256_extracti128_si256(x5_lo, 1);
            _mm256_storeu_si256((__m256i *)(out + 24), _mm256_cvtepi16_epi64(t7));
            const __m128i t8 = _mm_shuffle_epi32(t7, 0xEE);
            _mm256_storeu_si256((__m256i *)(out + 28), _mm256_cvtepi16_epi64(t8));
        }
    }
    else /* 16 < nbits <= 32 */
    {
        /* 17 to 32 bits: up to four AVX registers; it is hard to
         * deinterleave correctly with three, and that would add more
         * total instructions, so always extend to a 32x32 transpose.
         */
        __m256i perm, v_0, v_1;
        if (nbits < 25)
        {
            v_0 = _mm256_broadcastsi128_si256(_mm_loadu_si128((const __m128i *)in));
            if (nbits < 21)
            {
                /* 17 to 20 bits: first 128 bits repeat in both */
                v_1 = v_0;
                perm = _mm256_loadu2_m128i(
                    (const __m128i *)(shuffle_32 + nbits * 4 - 16),
                    (const __m128i *)(shuffle_32 + 48));
            }
            else /* 20 < nbits < 25 */
            {
                /* 21 to 24 bits: fourth 128 bits from different spot */
                v_1 = _mm256_inserti128_si256(v_0,
                    _mm_loadu_si128((const __m128i *)(in + nbits * 4 - 80)), 1);
                perm = _mm256_loadu2_m128i(
                    (const __m128i *)(shuffle_32 + 64),
                    (const __m128i *)(shuffle_32 + nbits * 4 - 32));
            }
            v_1 = _mm256_shuffle_epi8(v_1, perm);
            perm = _mm256_broadcastsi128_si256(_mm_loadu_si128(
                (const __m128i *)(shuffle_32 + 48)));
        }
        else
        {
            if (nbits < 29)
            {
                v_0 = _mm256_broadcastsi128_si256(_mm_loadu_si128((const __m128i *)in));
                perm = _mm256_loadu2_m128i(
                    (const __m128i *)(shuffle_32 + nbits * 4 - 48),
                    (const __m128i *)(shuffle_32 + 48));
            }
            else /* 28 < nbits < 33 */
            {
                v_0 = _mm256_loadu2_m128i((const __m128i *)(in + nbits * 4 - 112),
                    (const __m128i *)in);
                perm = _mm256_loadu2_m128i(
                    (const __m128i *)(shuffle_32 + 64),
                    (const __m128i *)(shuffle_32 + nbits * 4 - 64));
            }
            v_1 = _mm256_lddqu_si256((const __m256i *)(in + nbits * 4 - 96));
        }
        v_0 = _mm256_shuffle_epi8(v_0, perm);
        __m256i v_2 = _mm256_lddqu_si256((const __m256i *)(in + nbits * 4 - 64));
        __m256i v_3 = _mm256_lddqu_si256((const __m256i *)(in + nbits * 4 - 32));

        perm = _mm256_setr_epi8(
            12, 8, 4, 0, 13, 9, 5, 1, 14, 10, 6, 2, 15, 11, 7, 3,
            12, 8, 4, 0, 13, 9, 5, 1, 14, 10, 6, 2, 15, 11, 7, 3);
        v_0 = _mm256_shuffle_epi8(v_0, perm);
        v_1 = _mm256_shuffle_epi8(v_1, perm);
        v_2 = _mm256_shuffle_epi8(v_2, perm);
        v_3 = _mm256_shuffle_epi8(v_3, perm);

        perm = _mm256_setr_epi32(4, 0, 5, 1, 6, 2, 7, 3);
        v_0 = _mm256_permutevar8x32_epi32(v_0, perm);
        v_1 = _mm256_permutevar8x32_epi32(v_1, perm);
        v_2 = _mm256_permutevar8x32_epi32(v_2, perm);
        v_3 = _mm256_permutevar8x32_epi32(v_3, perm);

        const __m256i c1 = _mm256_set1_epi64x(0xAA55AA55AA55AA55);
        const __m256i c2 = _mm256_set1_epi64x(0x00AA00AA00AA00AA);
        const __m256i x1_0 = _mm256_or_si256(_mm256_and_si256(v_0, c1),
            _mm256_or_si256(_mm256_slli_epi64(_mm256_and_si256(v_0, c2), 7),
            _mm256_and_si256(_mm256_srli_epi64(v_0, 7), c2)));
        const __m256i x1_1 = _mm256_or_si256(_mm256_and_si256(v_1, c1),
            _mm256_or_si256(_mm256_slli_epi64(_mm256_and_si256(v_1, c2), 7),
            _mm256_and_si256(_mm256_srli_epi64(v_1, 7), c2)));
        const __m256i x1_2 = _mm256_or_si256(_mm256_and_si256(v_2, c1),
            _mm256_or_si256(_mm256_slli_epi64(_mm256_and_si256(v_2, c2), 7),
            _mm256_and_si256(_mm256_srli_epi64(v_2, 7), c2)));
        const __m256i x1_3 = _mm256_or_si256(_mm256_and_si256(v_3, c1),
            _mm256_or_si256(_mm256_slli_epi64(_mm256_and_si256(v_3, c2), 7),
            _mm256_and_si256(_mm256_srli_epi64(v_3, 7), c2)));

        const __m256i c3 = _mm256_set1_epi64x(0xCCCC3333CCCC3333);
        const __m256i c4 = _mm256_set1_epi64x(0x0000CCCC0000CCCC);
        const __m256i x2_0 = _mm256_or_si256(_mm256_and_si256(x1_0, c3),
            _mm256_or_si256(_mm256_slli_epi64(_mm256_and_si256(x1_0, c4), 14),
            _mm256_and_si256(_mm256_srli_epi64(x1_0, 14), c4)));
        const __m256i x2_1 = _mm256_or_si256(_mm256_and_si256(x1_1, c3),
            _mm256_or_si256(_mm256_slli_epi64(_mm256_and_si256(x1_1, c4), 14),
            _mm256_and_si256(_mm256_srli_epi64(x1_1, 14), c4)));
        const __m256i x2_2 = _mm256_or_si256(_mm256_and_si256(x1_2, c3),
            _mm256_or_si256(_mm256_slli_epi64(_mm256_and_si256(x1_2, c4), 14),
            _mm256_and_si256(_mm256_srli_epi64(x1_2, 14), c4)));
        const __m256i x2_3 = _mm256_or_si256(_mm256_and_si256(x1_3, c3),
            _mm256_or_si256(_mm256_slli_epi64(_mm256_and_si256(x1_3, c4), 14),
            _mm256_and_si256(_mm256_srli_epi64(x1_3, 14), c4)));

        const __m256i c5 = _mm256_set1_epi64x(0xF0F0F0F00F0F0F0F);
        const __m256i c6 = _mm256_set1_epi64x(0x00000000F0F0F0F0);
        const __m256i x3_0 = _mm256_or_si256(_mm256_and_si256(x2_0, c5),
            _mm256_or_si256(_mm256_slli_epi64(_mm256_and_si256(x2_0, c6), 28),
            _mm256_and_si256(_mm256_srli_epi64(x2_0, 28), c6)));
        const __m256i x3_1 = _mm256_or_si256(_mm256_and_si256(x2_1, c5),
            _mm256_or_si256(_mm256_slli_epi64(_mm256_and_si256(x2_1, c6), 28),
            _mm256_and_si256(_mm256_srli_epi64(x2_1, 28), c6)));
        const __m256i x3_2 = _mm256_or_si256(_mm256_and_si256(x2_2, c5),
            _mm256_or_si256(_mm256_slli_epi64(_mm256_and_si256(x2_2, c6), 28),
            _mm256_and_si256(_mm256_srli_epi64(x2_2, 28), c6)));
        const __m256i x3_3 = _mm256_or_si256(_mm256_and_si256(x2_3, c5),
            _mm256_or_si256(_mm256_slli_epi64(_mm256_and_si256(x2_3, c6), 28),
            _mm256_and_si256(_mm256_srli_epi64(x2_3, 28), c6)));

        perm = _mm256_setr_epi8(
            7, 6, 5, 4, 3, 2, 1, 0, 15, 14, 13, 12, 11, 10, 9, 8,
            7, 6, 5, 4, 3, 2, 1, 0, 15, 14, 13, 12, 11, 10, 9, 8);
        const __m256i x4_0 = _mm256_shuffle_epi8(x3_0, perm);
        const __m256i x4_1 = _mm256_shuffle_epi8(x3_1, perm);
        const __m256i x4_2 = _mm256_shuffle_epi8(x3_2, perm);
        const __m256i x4_3 = _mm256_shuffle_epi8(x3_3, perm);

        const __m256i x5_0 = _mm256_unpacklo_epi8(x4_1, x4_0);
        const __m256i x5_1 = _mm256_unpackhi_epi8(x4_1, x4_0);
        const __m256i x5_2 = _mm256_unpacklo_epi8(x4_3, x4_2);
        const __m256i x5_3 = _mm256_unpackhi_epi8(x4_3, x4_2);

        const __m256i x6_0 = _mm256_unpacklo_epi16(x5_2, x5_0);
        const __m256i x6_1 = _mm256_unpackhi_epi16(x5_2, x5_0);
        const __m256i x6_2 = _mm256_unpacklo_epi16(x5_3, x5_1);
        const __m256i x6_3 = _mm256_unpackhi_epi16(x5_3, x5_1);

        const __m128i t1 = _mm256_castsi256_si128(x6_0);
        _mm256_storeu_si256((__m256i *)(out + 0), _mm256_cvtepi32_epi64(t1));
        const __m128i t2 = _mm256_castsi256_si128(x6_1);
        _mm256_storeu_si256((__m256i *)(out + 4), _mm256_cvtepi32_epi64(t2));
        const __m128i t3 = _mm256_castsi256_si128(x6_2);
        _mm256_storeu_si256((__m256i *)(out + 8), _mm256_cvtepi32_epi64(t3));
        const __m128i t4 = _mm256_castsi256_si128(x6_3);
        _mm256_storeu_si256((__m256i *)(out + 12), _mm256_cvtepi32_epi64(t4));
        const __m128i t5 = _mm256_extracti128_si256(x6_0, 1);
        _mm256_storeu_si256((__m256i *)(out + 16), _mm256_cvtepi32_epi64(t5));
        const __m128i t6 = _mm256_extracti128_si256(x6_1, 1);
        _mm256_storeu_si256((__m256i *)(out + 20), _mm256_cvtepi32_epi64(t6));
        const __m128i t7 = _mm256_extracti128_si256(x6_2, 1);
        _mm256_storeu_si256((__m256i *)(out + 24), _mm256_cvtepi32_epi64(t7));
        const __m128i t8 = _mm256_extracti128_si256(x6_3, 1);
        _mm256_storeu_si256((__m256i *)(out + 28), _mm256_cvtepi32_epi64(t8));
    }
}

void transpose_avx2(int64_t *out, const char *in, int bits, int count)
{
    switch (count)
    {
    case 8:  transpose_8_avx2 (out, in, bits); break;
    case 16: transpose_16_avx2(out, in, bits); break;
    case 32: transpose_32_avx2(out, in, bits); break;
    }
}

#endif

/* ifunc-based resolvers are glibc-specific and cannot use getenv(). */
static void (*resolve_transpose(const char *version))(int64_t *, const char *, int, int)
{
    if (version && !strcmp(version, "generic"))
        return transpose_generic;

#ifdef __x86_64__
    if (__builtin_cpu_supports("avx2") || (version && !strcmp(version, "avx2")))
        return transpose_avx2;
#endif

    return transpose_generic;
}

void transpose_select(const char *version)
{
    transpose = resolve_transpose(version);
}

void transpose_init(void)
{
    transpose_select(getenv("TRANSPOSE_FORCE"));
}
