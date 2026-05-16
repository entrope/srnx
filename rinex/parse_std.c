#include "rinex/rnx_priv.h"

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

static void transpose_8_generic(int64_t *out, const unsigned char *in, int bits);

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

/** Decode a row-major K-rows × (count/8)-bytes bit matrix as count/8
 * consecutive 8-wide transpositions, one per column group of 8 values.
 */
void rnx_std_transpose(int64_t *out, const char *in, int bits, int count)
{
    const unsigned char *cu_in = (const unsigned char *)in;
    int n_groups = count >> 3;
    int gg, rr;
    unsigned char tmp[32];      /* bits <= 32 per SRNX block-header k field */

    for (gg = 0; gg < n_groups; ++gg)
    {
        for (rr = 0; rr < bits; ++rr)
            tmp[rr] = cu_in[gg + rr * n_groups];
        transpose_8_generic(out + gg * 8, tmp, bits);
    }
}

static const char *rnx_parse_obs
(
    struct rinex_parser *p,
    const char *obs,
    int nn
)
{
    int64_t value;
    int neg;
    int kk;
    char buf[16];

    /* The first 11 characters must be present: space, minus, digit or dot. */
    for (kk = 0; kk < 10; ++kk)
    {
        if (*obs != ' ') break;
        buf[kk] = *obs++;
    }
    neg = 0;
    if (kk < 10 && *obs == '-')
    {
        neg = 1;
        buf[kk++] = ' ';
        obs++;
    }
    for (; kk < 10; ++kk)
    {
        if (*obs < '0' || *obs > '9') break;
        buf[kk] = *obs++;
    }
    if (kk != 10 || *obs++ != '.')
    {
        return NULL;
    }
    buf[kk++] = '.';
    for (; kk < 16; ++kk)
    {
        buf[kk] = (*obs == '\n') ? ' ' : *obs++;
    }

    /* Save the LLI and SSI. */
    p->lli[nn] = buf[14];
    p->ssi[nn] = buf[15];

    /* Parse the observation value. */
#define DVAL(A) ((A == ' ') ? 0 : (A - '0'))
#define FOUR(W, X, Y, Z) (DVAL(W) + 10 * DVAL(X) + 100 * DVAL(Y) + 1000 * DVAL(Z))
    value = DVAL(buf[13])
        /* buf[10] == '.', at least in theory */
        + 10 * FOUR(buf[12], buf[11], buf[9], buf[8])
        + 100000 * FOUR(buf[7], buf[6], buf[5], buf[4])
        + INT64_C(1000000000) * FOUR(buf[3], buf[2], buf[1], buf[0]);
#undef FOUR
#undef DVAL
    if (neg)
    {
        value = -value;
    }
    p->obs[nn] = value;

    return obs;
}

#define SIMD_PARSE_OBS obs = rnx_parse_obs(&p->base, obs, nn)

#define SIMD_GET_N_NEWLINES

#define SIMD_TYPE std
#include "rinex/parse_simd.ii"
