// SPDX-License-Identifier: Apache-2.0
#pragma once

#if defined(__x86_64__)
    #include <immintrin.h>
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
    #include <arm_neon.h>
#endif

namespace crispy
{

template <typename>
struct PlatformIntrinsics
{
};

#if defined(__x86_64__) // {{{

template <>
struct PlatformIntrinsics<__m128i>
{
    using m128i = __m128i;

    static inline m128i setzero() noexcept { return vreinterpretq_s64_s32(vdupq_n_s32(0)); }

    static inline m128i aesimc(m128i a) noexcept { return _mm_aesimc_si128(a); }

    static inline m128i aesdec(m128i a, m128i roundKey) noexcept { return _mm_aesdec_si128(a, roundKey); }

    static inline m128i aesdeclast(m128i a, m128i roundKey) noexcept
    {
        return _mm_aesdeclast_si128(a, roundKey);
    }

    static inline m128i load32(uint32_t a, uint32_t b, uint32_t c, uint32_t d) noexcept
    {
        return _mm_set_epi32(
            static_cast<int>(a), static_cast<int>(b), static_cast<int>(c), static_cast<int>(d));
    }

    static inline m128i xor128(m128i a, m128i b) noexcept { return _mm_xor_si128(a, b); }

    static inline m128i loadUnaligned(m128i const* p) noexcept
    {
        return _mm_loadu_si128(static_cast<m128i const*>(p));
    }

    static inline int32_t castToInt32(m128i a) { return _mm_cvtsi128_si32(a); }

    static inline bool compare(m128i a, m128i b) noexcept
    {
        return _mm_movemask_epi8(_mm_cmpeq_epi32(a, b)) == 0xFFFF;
    }

    static inline m128i cvtsi64_si128(int64_t a) // NOLINT(readability-identifier-naming)
    {
        return _mm_cvtsi64_si128(a);
    }
};

using Intrinsics = PlatformIntrinsics<__m128i>;

#endif
// }}}

#if defined(__aarch64__) || defined(_M_ARM64) // {{{
template <>
struct PlatformIntrinsics<int64x2_t>
{
    // The following inline functions (in its initial version) were borrowed from:
    // https://github.com/f1ed/emp/blob/master/emp-tool/utils/block.h

    using m128i = int64x2_t;

    static inline m128i setzero() noexcept { return vreinterpretq_s64_s32(vdupq_n_s32(0)); }

    static inline m128i aesimc(m128i a) noexcept
    {
        return vreinterpretq_s64_u8(vaesimcq_u8(vreinterpretq_u8_s64(a)));
    }

    static inline m128i aesdec(m128i a, m128i roundKey) noexcept
    {
        return vreinterpretq_s64_u8(
            vaesimcq_u8(vaesdq_u8(vreinterpretq_u8_s64(a), vdupq_n_u8(0)) ^ vreinterpretq_u8_s64(roundKey)));
    }

    static inline m128i aesdeclast(m128i a, m128i roundKey) noexcept
    {
        return vreinterpretq_s64_u8(vaesdq_u8(vreinterpretq_u8_s64(a), vdupq_n_u8(0))
                                    ^ vreinterpretq_u8_s64(roundKey));
    }

    static inline m128i load32(uint32_t a, uint32_t b, uint32_t c, uint32_t d) noexcept
    {
        alignas(16) int32_t data[4] = {
            static_cast<int>(a),
            static_cast<int>(b),
            static_cast<int>(c),
            static_cast<int>(d),
        };
        return vreinterpretq_s64_s32(vld1q_s32(data));
    }

    static inline m128i xor128(m128i a, m128i b) noexcept
    {
        // Computes the bitwise XOR of the 128-bit value in a and the 128-bit value in
        // b.  https://msdn.microsoft.com/en-us/library/fzt08www(v=vs.100).aspx
        return vreinterpretq_s64_s32(veorq_s32(vreinterpretq_s32_s64(a), vreinterpretq_s32_s64(b)));
    }

    // Loads 128-bit value. :
    // https://msdn.microsoft.com/zh-cn/library/f4k12ae8(v=vs.90).aspx
    static inline m128i loadUnaligned(m128i const* p) noexcept
    {
        return vreinterpretq_s64_s32(vld1q_s32((int32_t const*) p));
    }

    // Copy the lower 32-bit integer in a to dst.
    //
    //   dst[31:0] := a[31:0]
    //
    // https://software.intel.com/sites/landingpage/IntrinsicsGuide/#text=_mm_cvtsi128_si32
    static inline int32_t castToInt32(m128i a) { return vgetq_lane_s32(vreinterpretq_s32_s64(a), 0); }

    static inline bool compare(m128i a, m128i b) noexcept
    {
        return movemask_epi8(
                   vreinterpretq_s64_u32(vceqq_s32(vreinterpretq_s32_s64(a), vreinterpretq_s32_s64(b))))
               == 0xFFFF;
    }

    static inline int movemask_epi8(m128i a) // NOLINT(readability-identifier-naming)
    {
        // Use increasingly wide shifts+adds to collect the sign bits
        // together.
        // Since the widening shifts would be rather confusing to follow in little
        // endian, everything will be illustrated in big endian order instead. This
        // has a different result - the bits would actually be reversed on a big
        // endian machine.

        // Starting input (only half the elements are shown):
        // 89 ff 1d c0 00 10 99 33
        uint8x16_t input = vreinterpretq_u8_s64(a);

        // Shift out everything but the sign bits with an unsigned shift right.
        //
        // Bytes of the vector::
        // 89 ff 1d c0 00 10 99 33
        // \  \  \  \  \  \  \  \    high_bits = (uint16x4_t)(input >> 7)
        //  |  |  |  |  |  |  |  |
        // 01 01 00 01 00 00 01 00
        //
        // Bits of first important lane(s):
        // 10001001 (89)
        // \______
        //        |
        // 00000001 (01)
        uint16x8_t high_bits = vreinterpretq_u16_u8(vshrq_n_u8(input, 7));

        // Merge the even lanes together with a 16-bit unsigned shift right + add.
        // 'xx' represents garbage data which will be ignored in the final result.
        // In the important bytes, the add functions like a binary OR.
        //
        // 01 01 00 01 00 00 01 00
        //  \_ |  \_ |  \_ |  \_ |   paired16 = (uint32x4_t)(input + (input >> 7))
        //    \|    \|    \|    \|
        // xx 03 xx 01 xx 00 xx 02
        //
        // 00000001 00000001 (01 01)
        //        \_______ |
        //                \|
        // xxxxxxxx xxxxxx11 (xx 03)
        uint32x4_t paired16 = vreinterpretq_u32_u16(vsraq_n_u16(high_bits, high_bits, 7));

        // Repeat with a wider 32-bit shift + add.
        // xx 03 xx 01 xx 00 xx 02
        //     \____ |     \____ |  paired32 = (uint64x1_t)(paired16 + (paired16 >>
        //     14))
        //          \|          \|
        // xx xx xx 0d xx xx xx 02
        //
        // 00000011 00000001 (03 01)
        //        \\_____ ||
        //         '----.\||
        // xxxxxxxx xxxx1101 (xx 0d)
        uint64x2_t paired32 = vreinterpretq_u64_u32(vsraq_n_u32(paired16, paired16, 14));

        // Last, an even wider 64-bit shift + add to get our result in the low 8 bit
        // lanes. xx xx xx 0d xx xx xx 02
        //            \_________ |   paired64 = (uint8x8_t)(paired32 + (paired32 >>
        //            28))
        //                      \|
        // xx xx xx xx xx xx xx d2
        //
        // 00001101 00000010 (0d 02)
        //     \   \___ |  |
        //      '---.  \|  |
        // xxxxxxxx 11010010 (xx d2)
        uint8x16_t paired64 = vreinterpretq_u8_u64(vsraq_n_u64(paired32, paired32, 28));

        // Extract the low 8 bits from each 64-bit lane with 2 8-bit extracts.
        // xx xx xx xx xx xx xx d2
        //                      ||  return paired64[0]
        //                      d2
        // Note: Little endian would return the correct value 4b (01001011) instead.
        return vgetq_lane_u8(paired64, 0) | ((int) vgetq_lane_u8(paired64, 8) << 8);
    }

    // Moves 64-bit integer a to the least significant 64 bits of an __m128 object,
    // zero extending the upper bits.
    //
    //   r0 := a
    //   r1 := 0x0
    static inline m128i cvtsi64_si128(int64_t a) // NOLINT(readability-identifier-naming)
    {
        return vreinterpretq_m128i_s64(vsetq_lane_s64(a, vdupq_n_s64(0), 0));
    }
};

using Intrinsics = PlatformIntrinsics<int64x2_t>;
#endif
// }}}

// #if defined(INTRINSICS_HAS_ARM64_NEON)
// using m128i = int64x2_t; // 128-bit vector containing integers
// #else
// using m128i = __m128i;
// #endif

} // namespace crispy
