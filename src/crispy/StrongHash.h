/**
 * This file is part of the "contour" project.
 *   Copyright (c) 2020 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <crispy/assert.h>

#include <array>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>

// NB: That should be defined via CMakeLists.txt's option()...
// #define STRONGHASH_USE_AES_NI 1

#if defined(STRONGHASH_USE_AES_NI)
    #if defined(__x86_64__)
        #include <immintrin.h>

        #if defined(__AES__)
            #include <wmmintrin.h>
        #endif
    #elif defined(__aarch64__)
        #include <sse2neon/sse2neon.h>

// The following inline functions were borrowed from:
// https://github.com/f1ed/emp/blob/master/emp-tool/utils/block.h
inline __m128i _mm_aesimc_si128(__m128i a) noexcept
{
    return vreinterpretq_m128i_u8(vaesimcq_u8(vreinterpretq_u8_m128i(a)));
}

inline __m128i _mm_aesdec_si128(__m128i a, __m128i roundKey) noexcept
{
    return vreinterpretq_m128i_u8(
        vaesimcq_u8(vaesdq_u8(vreinterpretq_u8_m128i(a), vdupq_n_u8(0)) ^ vreinterpretq_u8_m128i(roundKey)));
}

inline __m128i _mm_aesdeclast_si128(__m128i a, __m128i roundKey) noexcept
{
    return vreinterpretq_m128i_u8(vaesdq_u8(vreinterpretq_u8_m128i(a), vdupq_n_u8(0))
                                  ^ vreinterpretq_u8_m128i(roundKey));
}
    #endif
#else
    #include <crispy/FNV.h>
#endif

namespace crispy
{

struct StrongHash
{
    // some random seed
    static constexpr std::array<unsigned char, 16> defaultSeed = {
        // NOLINT(readability-identifier-naming)
        114, 188, 209, 2, 232, 4, 178, 176, 240, 216, 201, 127, 40, 41, 95, 143,
    };

    StrongHash(uint32_t a, uint32_t b, uint32_t c, uint32_t d) noexcept;

#if defined(STRONGHASH_USE_AES_NI)
    explicit StrongHash(__m128i v) noexcept;
#endif

    StrongHash() = default;
    StrongHash(StrongHash const&) = default;
    StrongHash& operator=(StrongHash const&) = default;
    StrongHash(StrongHash&&) noexcept = default;
    StrongHash& operator=(StrongHash&&) noexcept = default;

    template <typename T>
    static StrongHash compute(T const& value) noexcept;

    template <typename T>
    static StrongHash compute(std::basic_string_view<T> text) noexcept;

    template <typename T, typename Alloc>
    static StrongHash compute(std::basic_string<T, Alloc> const& text) noexcept;

    static StrongHash compute(void const* data, size_t n) noexcept;

    // Retrieves the 4th 32-bit component of the internal representation.
    // TODO: Provide access also to: a, b, c.
    [[nodiscard]] uint32_t d() const noexcept
    {
#if defined(STRONGHASH_USE_AES_NI)
        return static_cast<uint32_t>(_mm_cvtsi128_si32(value));
#else
        return value[3];
#endif
    }

#if defined(STRONGHASH_USE_AES_NI)
    __m128i value {};
#else
    std::array<uint32_t, 4> value;
#endif
};

inline StrongHash::StrongHash(uint32_t a, uint32_t b, uint32_t c, uint32_t d) noexcept:
#if defined(STRONGHASH_USE_AES_NI)
    StrongHash(_mm_xor_si128(
        _mm_set_epi32(static_cast<int>(a), static_cast<int>(b), static_cast<int>(c), static_cast<int>(d)),
        _mm_loadu_si128((__m128i const*) defaultSeed.data())))
#else
    value { a, b, c, d }
#endif
{
}

#if defined(STRONGHASH_USE_AES_NI)
inline StrongHash::StrongHash(__m128i v) noexcept: value { v }
{
}
#endif

inline bool operator==(StrongHash a, StrongHash b) noexcept
{
#if defined(STRONGHASH_USE_AES_NI)
    return _mm_movemask_epi8(_mm_cmpeq_epi32(a.value, b.value)) == 0xFFFF;
#else
    return a.value == b.value;
#endif
}

inline bool operator!=(StrongHash a, StrongHash b) noexcept
{
    return !(a == b);
}

inline StrongHash operator*(StrongHash const& a, StrongHash const& b) noexcept
{
#if defined(STRONGHASH_USE_AES_NI)
    // TODO AES-NI fallback
    __m128i hashValue = a.value;

    hashValue = _mm_xor_si128(hashValue, b.value);
    hashValue = _mm_aesdec_si128(hashValue, _mm_setzero_si128());
    hashValue = _mm_aesdec_si128(hashValue, _mm_setzero_si128());
    hashValue = _mm_aesdec_si128(hashValue, _mm_setzero_si128());
    hashValue = _mm_aesdec_si128(hashValue, _mm_setzero_si128());

    return StrongHash { hashValue };
#else
    return StrongHash { FNV<uint32_t, uint32_t>()(a.value[0], b.value[0]),
                        FNV<uint32_t, uint32_t>()(a.value[1], b.value[1]),
                        FNV<uint32_t, uint32_t>()(a.value[2], b.value[2]),
                        FNV<uint32_t, uint32_t>()(a.value[3], b.value[3]) };
#endif
}

inline StrongHash operator*(StrongHash a, uint32_t b) noexcept
{
    return a * StrongHash(0, 0, 0, b);
}

template <typename T>
StrongHash StrongHash::compute(std::basic_string_view<T> text) noexcept
{
    // return compute(value.data(), value.size());
    auto hash = StrongHash(0, 0, 0, static_cast<uint32_t>(text.size()));
    for (auto const codepoint: text)
        hash = hash * codepoint; // StrongHash(0, 0, 0, static_cast<uint32_t>(codepoint));
    return hash;
}

template <typename T, typename Alloc>
StrongHash StrongHash::compute(std::basic_string<T, Alloc> const& text) noexcept
{
    // return compute(value.data(), value.size());
    auto hash = StrongHash(0, 0, 0, static_cast<uint32_t>(text.size()));
    for (T const codepoint: text)
        hash = hash * static_cast<uint32_t>(codepoint);
    return hash;
}

template <typename T>
StrongHash StrongHash::compute(T const& value) noexcept
{
    return compute(&value, sizeof(value));
}

inline StrongHash StrongHash::compute(void const* data, size_t n) noexcept
{
#if defined(STRONGHASH_USE_AES_NI)
    static_assert(sizeof(__m128i) == 16);
    auto constexpr ChunkSize = static_cast<int>(sizeof(__m128i));

    __m128i hashValue = _mm_cvtsi64_si128(static_cast<long long>(n));
    hashValue = _mm_xor_si128(hashValue, _mm_loadu_si128((__m128i const*) defaultSeed.data()));

    char const* inputPtr = (char const*) data;
    for (int chunkIndex = 0; chunkIndex < static_cast<int>(n) / ChunkSize; chunkIndex++)
    {
        __m128i chunk = _mm_loadu_si128((__m128i const*) inputPtr);

        hashValue = _mm_xor_si128(hashValue, chunk);
        hashValue = _mm_aesdec_si128(hashValue, _mm_setzero_si128());
        hashValue = _mm_aesdec_si128(hashValue, _mm_setzero_si128());
        hashValue = _mm_aesdec_si128(hashValue, _mm_setzero_si128());
        hashValue = _mm_aesdec_si128(hashValue, _mm_setzero_si128());

        inputPtr += ChunkSize;
    }

    auto const remainingByteCount = n % ChunkSize;
    if (remainingByteCount)
    {
        char lastChunk[ChunkSize] { 0 };
        std::memcpy(lastChunk + ChunkSize - remainingByteCount, inputPtr, remainingByteCount);
        __m128i chunk = _mm_loadu_si128((__m128i*) lastChunk);
        hashValue = _mm_xor_si128(hashValue, chunk);
        hashValue = _mm_aesdec_si128(hashValue, _mm_setzero_si128());
        hashValue = _mm_aesdec_si128(hashValue, _mm_setzero_si128());
        hashValue = _mm_aesdec_si128(hashValue, _mm_setzero_si128());
        hashValue = _mm_aesdec_si128(hashValue, _mm_setzero_si128());
    }

    return StrongHash { hashValue };
#else
    auto const* i = (uint8_t const*) data;
    auto const* e = i + n;
    auto const result = FNV<uint8_t, uint64_t>()(i, e);
    auto constexpr a = 0;
    auto constexpr b = 0;
    auto const c = static_cast<uint32_t>((result >> 32) & 0xFFFFFFFFu);
    auto const d = static_cast<uint32_t>(result & 0xFFFFFFFFu);
    return StrongHash { a, b, c, d };
#endif
}

inline std::string to_string(StrongHash const& hash)
{
    uint32_t u32[4];
    std::memcpy(u32, &hash, sizeof(hash));
    return fmt::format("{:04X}{:04X}{:04X}{:04X}", u32[0], u32[1], u32[2], u32[3]);
}

inline std::string to_structured_string(StrongHash const& hash)
{
    uint32_t u32[4];
    std::memcpy(u32, &hash, sizeof(hash));
    // produce a somewhat compressed format to not be too long.
    std::string s;
    for (int i = 3; i >= 0; --i)
    {
        if (!s.empty())
            s += '.';
        if (u32[i] != 0)
            s += fmt::format("{:X}", u32[i]);
    }
    return s;
}

inline int to_integer(StrongHash hash) noexcept
{
#if defined(STRONGHASH_USE_AES_NI)
    return _mm_cvtsi128_si32(hash.value);
#else
    return static_cast<int>(hash.value.back());
#endif
}

template <typename T>
struct StrongHasher
{
    StrongHash operator()(T const&) noexcept; // Specialize and implement me.
};

template <>
struct StrongHasher<uint64_t>
{
    inline StrongHash operator()(uint64_t v) noexcept
    {
        auto const c = static_cast<uint32_t>((v >> 32) & 0xFFFFFFFFu);
        auto const d = static_cast<uint32_t>(v & 0xFFFFFFFFu);
#if defined(STRONGHASH_USE_AES_NI)
        return StrongHash { _mm_set_epi32(0, 0, c, d) };
#else
        return StrongHash { 0, 0, c, d };
#endif
    }
};
// {{{ some standard hash implementations
namespace detail
{
    template <typename T>
    struct StdHash32
    {
        inline StrongHash operator()(T v) noexcept
        {
#if defined(STRONGHASH_USE_AES_NI)
            return StrongHash { _mm_set_epi32(0, 0, 0, v) };
#else
            return StrongHash { 0, 0, 0, static_cast<uint32_t>(v) };
#endif
        }
    };
} // namespace detail

// clang-format off
template <> struct StrongHasher<char>: public detail::StdHash32<char> { };
template <> struct StrongHasher<unsigned char>: public detail::StdHash32<char> { };
template <> struct StrongHasher<int16_t>: public detail::StdHash32<int16_t> { };
template <> struct StrongHasher<uint16_t>: public detail::StdHash32<uint16_t> { };
template <> struct StrongHasher<int32_t>: public detail::StdHash32<int32_t> { };
template <> struct StrongHasher<uint32_t>: public detail::StdHash32<uint32_t> { };
// clang-format on
// }}}

template <class Char, class Allocator>
struct StrongHasher<std::basic_string<Char, Allocator>>
{
    inline StrongHash operator()(std::basic_string<Char, Allocator> const& v) noexcept
    {
        return StrongHasher<int> {}((int) std::hash<std::basic_string<Char, Allocator>> {}(v));
    }
};

template <typename U>
struct StrongHasher<std::basic_string_view<U>>
{
    inline StrongHash operator()(std::basic_string_view<U> v) noexcept
    {
        return StrongHasher<int> {}((int) std::hash<std::basic_string_view<U>> {}(v));
    }
};

} // namespace crispy

namespace fmt
{
template <>
struct formatter<crispy::StrongHash>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(crispy::StrongHash const& hash, FormatContext& ctx)
    {
        return fmt::format_to(ctx.out(), "{}", to_structured_string(hash));
    }
};
} // namespace fmt
