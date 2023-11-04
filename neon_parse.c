#include "rinex_p.h"
#include <arm_neon.h>

static const char *rnx_neon_parse_obs
(
    const char *obs,
    struct rinex_parser *p,
    int idx
)
{
    const int8x16_t v_zero = vdupq_n_s8(0);
    const int8x16_t v_minus = {'-', '-', '-', '-', '-', '-', '-', '-', '-', '-', 0, 0, 0, 0, 0, 0};
    const int8x16_t v_digits = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0, 0, 0, 0, 0, 0};
    const int8x16_t v_1_10 = {0, 0, 10, 1, 10, 1, 10, 1, 10, 1, 10, 1, 0, 1, 10, 1};
    const int16x8_t v_1_100 = {0, 1, 100, 1, 100, 1, 100, 1};
    const int32x4_t v_1_10000 = {10000, 1, 1000, 1};

    /* This has two paths: First, if there is a newline, replace it and
     * everything after it with spaces (for LLI and SSI); then convert
     * digits in the observation part to their values.
     *
     * The observation part can normally contain digits or any of
     * [-.\r\n]; we subtract '0' and use VTBL to index the value.
     *
     * Variables for the LLI/SSI path start with m_ and variables for
     * the observation path start with v_.
     */
    const int8x16_t m_orig = vld1q_s8((const int8_t *)obs);

    /* m_nl_<n> through m_mask are a prefix-scan "or" of obs[i] == '\n' */
    const int8x16_t m_nl_0 = vceqq_s8(m_orig, vdupq_n_s8('\n'));
    const int8x16_t m_nl_1 = vorrq_s8(m_nl_0, vextq_s8(v_zero, m_nl_0, 15));
    const int8x16_t m_nl_2 = vorrq_s8(m_nl_1, vextq_s8(v_zero, m_nl_1, 14));
    const int8x16_t m_nl_3 = vorrq_s8(m_nl_2, vextq_s8(v_zero, m_nl_2, 12));
    const int8x16_t m_mask = vorrq_s8(m_nl_3, vextq_s8(v_zero, m_nl_3, 8));
    /* replace any masked bytes with ' ', then extract LLI and SSI */
    const int8x16_t m_obs = vbslq_s8(m_mask, vdupq_n_s8(' '), m_orig);
    p->lli[idx] = vgetq_lane_s8(m_obs, 14);
    p->ssi[idx] = vgetq_lane_s8(m_obs, 15);

    /* Check whether the input contains a '-'.  "-.1  " is allowed. */
    const int8_t is_neg = vminvq_s8(vceqq_s8(v_minus, m_obs));
    /* Realign the digits and translate them to byte-wide values. */
    const int8x16_t v_ext_s8 = vextq_s8(v_zero, m_obs, 14);
    const int8x16_t v_idx_s8 = vqsubq_s8(v_ext_s8, vdupq_n_s8('0'));
    const int8x16_t v_tbl_s8 = vqtbl1q_s8(v_digits, v_idx_s8);
    /* Perform multiply-accumulates with the place values. */
    const int8x16_t v_dig_s8 = vmulq_s8(v_tbl_s8, v_1_10);
    const int16x8_t v_dig_s16 = vmulq_s16(vpaddlq_s8(v_dig_s8), v_1_100);
    const int32x4_t v_dig_s32 = vmulq_s32(vpaddlq_s16(v_dig_s16), v_1_10000);
    const int64x2_t v_dig_s64 = vpaddlq_s32(v_dig_s32);
    int64_t obs_s64 = (int64_t)10000000 * vgetq_lane_s64(v_dig_s64, 0)
        + vgetq_lane_s64(v_dig_s64, 1);
    if (is_neg) obs_s64 = -obs_s64;
    p->obs[idx] = obs_s64;

    const int8_t done = 16 + vaddvq_s8(m_mask);
    return obs + done; /* how far did we skip? */
}

#if 0
static void rnx_neon_movemask
(
    char *out,
    uint8_t n_bytes,
    uint8x16_t v_data
)
{
    const uint8x16_t v_pos = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
    const uint8x16_t v_in = vld1q_u8((const uint8_t *)out);
    const uint8x16_t v_n_bytes = vdupq_n_u8(n_bytes);
    uint8x16_t v_mask  = vcleq_u8(v_pos, v_n_bytes);
    v_mask = vbslq_u8(v_mask, v_data, v_in);
    vst1q_u8((uint8_t *)out, v_mask);
}
#endif

#define SIMD_PARSE_OBS obs = rnx_neon_parse_obs(obs, &p->base, nn)

static int rnx_neon_get_n_newlines_inner(
    const struct rinex_parser *p,
    uint64_t *p_whence,
    int n_lines,
    int *found)
{
    const uint8x16_t v_nl = vdupq_n_u8('\n');
    const uint8x16_t v_m0 = { 0x01, 0x10, 0x01, 0x10, 0x01, 0x10, 0x01, 0x10,
        0x01, 0x10, 0x01, 0x10, 0x01, 0x10, 0x01, 0x10 };
    const uint8x16_t v_m1 = { 0x02, 0x20, 0x02, 0x20, 0x02, 0x20, 0x02, 0x20,
        0x02, 0x20, 0x02, 0x20, 0x02, 0x20, 0x02, 0x20 };
    const uint8x16_t v_m2 = { 0x04, 0x40, 0x04, 0x40, 0x04, 0x40, 0x04, 0x40,
        0x04, 0x40, 0x04, 0x40, 0x04, 0x40, 0x04, 0x40 };
    const uint8x16_t v_m3 = { 0x08, 0x80, 0x08, 0x80, 0x08, 0x80, 0x08, 0x80,
            0x08, 0x80, 0x08, 0x80, 0x08, 0x80, 0x08, 0x80 };
    const char * restrict buffer = p->stream->buffer;
    uint64_t whence = *p_whence;

    for (; whence + 128 < p->stream->size; whence += 128)
    {
        uint8x16x4_t v_lo = vld4q_u8((const uint8_t *)(buffer + whence));
        uint8x16x4_t v_hi = vld4q_u8((const uint8_t *)(buffer + whence + 64));
        uint64x2_t v_mask;
        uint64_t m_lo;
        int nn;

        v_lo.val[0] = vceqq_u8(v_lo.val[0], v_nl);
        v_hi.val[0] = vceqq_u8(v_hi.val[0], v_nl);
        v_lo.val[0] = vandq_u8(v_m0, v_lo.val[0]);
        v_hi.val[0] = vandq_u8(v_m0, v_hi.val[0]);

        v_lo.val[1] = vceqq_u8(v_lo.val[1], v_nl);
        v_hi.val[1] = vceqq_u8(v_hi.val[1], v_nl);
        v_lo.val[1] = vbslq_u8(v_m1, v_lo.val[1], v_lo.val[0]);
        v_hi.val[1] = vbslq_u8(v_m1, v_hi.val[1], v_hi.val[0]);

        v_lo.val[2] = vceqq_u8(v_lo.val[2], v_nl);
        v_hi.val[2] = vceqq_u8(v_hi.val[2], v_nl);
        v_lo.val[2] = vbslq_u8(v_m2, v_lo.val[2], v_lo.val[1]);
        v_hi.val[2] = vbslq_u8(v_m2, v_hi.val[2], v_hi.val[1]);

        v_lo.val[3] = vceqq_u8(v_lo.val[3], v_nl);
        v_hi.val[3] = vceqq_u8(v_hi.val[3], v_nl);
        v_lo.val[3] = vbslq_u8(v_m3, v_lo.val[3], v_lo.val[2]);
        v_hi.val[3] = vbslq_u8(v_m3, v_hi.val[3], v_hi.val[2]);

        v_mask = vreinterpretq_u64_u8(vpaddq_u8(v_lo.val[3], v_hi.val[3]));

        m_lo = vgetq_lane_u64(v_mask, 0);
        nn = __builtin_popcountll(m_lo);
        if (*found + nn < n_lines)
        {
            *found += nn;
        }
        else while (1)
        {
            int r = __builtin_ctzll(m_lo);
            m_lo &= (m_lo - 1);
            if (++*found == n_lines)
            {
                *p_whence = whence + r + 1;
                return 1;
            }
        }

        uint64_t m_hi = vgetq_lane_u64(v_mask, 1);
        nn = __builtin_popcountll(m_hi);
        if (*found + nn < n_lines)
        {
            *found += nn;
        }
        else while (1)
        {
            int r = __builtin_ctzll(m_hi);
            m_hi &= (m_hi - 1);
            if (++*found == n_lines)
            {
                *p_whence = whence + r + 65;
                return 1;
            }
        }
    }

    *p_whence = whence;
    return 0;
}

#define SIMD_GET_N_NEWLINES \
    if (rnx_neon_get_n_newlines_inner(p, &whence, n_lines, &found)) return whence

#define SIMD_TYPE neon
#include "simd_parse.ii"
