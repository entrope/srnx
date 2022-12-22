#include "std_parse.h"
#include <arm_neon.h>

static const char *rnx_neon_parse_obs
(
    const char *obs,
    struct rinex_parser *p,
    int idx
)
{
    const uint8x16_t v_zero = vdupq_n_u8(0);

    /* This interleaves two paths: if there is a newline, replace it and
     * everything after it with spaces (for LLI and SSI); and converting
     * digits in the observation part to their values.  The observation
     * part can normally contain digits or any of [-.\r\n]; we subtract
     * '0' and use VTBL to index the value.
     *
     * These are interleaved because a typical ARMv8 core has at least
     * two Advanced SIMD (Neon) execution units (and I don't trust
     * compilers to look far enough ahead to exploit the interleaving).
     */
    const uint8x16_t v_orig = vld1q_u8((const uint8_t *)obs);
    const uint8x16_t m_nl_0 = vceqq_u8(v_orig, vdupq_n_u8('\n'));
    uint8x16_t m_digits = vsubq_u8(v_orig, vdupq_n_u8('0'));
    const uint8x16_t m_nl_1 = vorrq_u8(m_nl_0, vextq_u8(m_nl_0, v_zero, 1));
    const uint8x16_t v_digits = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0, 0, 0, 0, 0, 0 };
    const uint8x16_t m_nl_2 = vorrq_u8(m_nl_1, vextq_u8(m_nl_1, v_zero, 2));
    m_digits = vqtbl1q_u8(m_digits, v_digits);
    const uint8x16_t m_nl_3 = vorrq_u8(m_nl_2, vextq_u8(m_nl_2, v_zero, 4));
    const uint8x16_t m_mask = vorrq_u8(m_mask, vextq_u8(m_nl_3, v_zero, 8));
    m_digits = vbicq_u8(m_digits, m_mask);
    const int idx = 256 - vaddvq_u8(m_mask);
    const uint8x16_t v_1_10 = { 10, 1, 10, 1, 10, 1, 10, 1, 10, 1, 0, 1, 10, 1, 0, 0 };
    m_digits = vmulq_u8(m_digits, v_1_10);
    const uint8x16_t m_obs = vbslq_u8(m_mask, vdupq_n_u8(' '), v_orig);
    uint16x8_t m_dig_u16 = vpaddq_u8(m_digits);
    const uint16x8_t v_1_100 = { 100, 1, 100, 1, 10, 1, 1, 0 };
    m_dig_u16 = vmulq_u16(m_dig_u16, v_1_100);
    uint32x4_t m_dig_u32 = vpaddq_u16(m_dig_u16);
    const uint32x4_t v_

    p->lli[idx] = m_obs[14];
    p->ssi[idx] = m_obs[15];

    /* TODO: convert observation value to int64_t */

    p->obs[idx] = 0;
    return obs + idx;
}

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

#define SIMD_PARSE_OBS obs = rnx_neon_parse_obs(obs, &p->base, nn)

static int rnx_neon_get_n_newlines_inner(
    const struct rinex_parser *p,
    uint64_t *p_whence,
    int n_lines,
    int *found
)
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
        if (found + nn < n_lines)
        {
            found += nn;
        }
        else while (1)
        {
            int r = __builtin_ctzll(m_lo);
            m_lo &= (m_lo - 1);
            if (++found == n_lines)
            {
                return buffer + r + 1;
            }
        }

        uint64_t m_hi = vgetq_lane_u64(v_mask, 1);
        nn = __builtin_popcountll(m_hi);
        if (found + nn < n_lines)
        {
            found += nn;
        }
        else while (1)
        {
            int r = __builtin_ctzll(m_hi);
            m_hi &= (m_hi - 1);
            if (++found == n_lines)
            {
                *p_whence = whence + r + 65;
                return 1;
            }
        }
    }

    *p_whence = whence;
    return 0;
}

#define SIND_GET_N_NEWLINES \
    if (rnx_avx2_get_n_newlines_inner(p, &whence, n_lines, &found)) return whence

#define SIMD_TYPE neon
#include "simd_parse.ii"
