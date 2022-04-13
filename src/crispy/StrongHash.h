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
#include <immintrin.h>
#include <string>
#include <string_view>
#include <type_traits>

#if defined(__AES__)
    #include <wmmintrin.h>
#endif

namespace crispy
{

struct StrongHash
{
    // some random seed
    static constexpr std::array<unsigned char, 16> DefaultSeed = {
        114, 188, 209, 2, 232, 4, 178, 176, 240, 216, 201, 127, 40, 41, 95, 143,
    };

    StrongHash(uint32_t a, uint32_t b, uint32_t c, uint32_t d) noexcept;

    explicit StrongHash(__m128i v) noexcept;

    StrongHash() = default;
    StrongHash(StrongHash const&) = default;
    StrongHash& operator=(StrongHash const&) = default;
    StrongHash(StrongHash&&) noexcept = default;
    StrongHash& operator=(StrongHash&&) noexcept = default;

    template <typename T>
    static StrongHash compute(T const& value) noexcept;

    template <typename T>
    static StrongHash compute(std::basic_string_view<T> value) noexcept;

    template <typename T, typename Alloc>
    static StrongHash compute(std::basic_string<T, Alloc> const& value) noexcept;

    static StrongHash compute(void const* data, size_t n) noexcept;

    __m128i value {};
};

inline StrongHash::StrongHash(uint32_t a, uint32_t b, uint32_t c, uint32_t d) noexcept:
    StrongHash(_mm_xor_si128(
        _mm_set_epi32(static_cast<int>(a), static_cast<int>(b), static_cast<int>(c), static_cast<int>(d)),
        _mm_loadu_si128((__m128i const*) DefaultSeed.data())))
{
}

inline StrongHash::StrongHash(__m128i v) noexcept: value { v }
{
}

inline bool operator==(StrongHash a, StrongHash b) noexcept
{
    return _mm_movemask_epi8(_mm_cmpeq_epi32(a.value, b.value)) == 0xFFFF;
}

inline bool operator!=(StrongHash a, StrongHash b) noexcept
{
    return !(a == b);
}

inline StrongHash operator*(StrongHash const& a, StrongHash const& b) noexcept
{
    // TODO AES-NI fallback
    __m128i hashValue = a.value;

    hashValue = _mm_xor_si128(hashValue, b.value);
    hashValue = _mm_aesdec_si128(hashValue, _mm_setzero_si128());
    hashValue = _mm_aesdec_si128(hashValue, _mm_setzero_si128());
    hashValue = _mm_aesdec_si128(hashValue, _mm_setzero_si128());
    hashValue = _mm_aesdec_si128(hashValue, _mm_setzero_si128());

    return StrongHash { hashValue };
}

inline StrongHash operator*(StrongHash a, uint32_t b) noexcept
{
    return a * StrongHash(0, 0, 0, b);
}

template <typename T>
StrongHash StrongHash::compute(std::basic_string_view<T> text) noexcept
{
    // return compute(value.data(), value.size());
    StrongHash hash;
    hash = StrongHash(0, 0, 0, static_cast<uint32_t>(text.size()));
    for (auto const codepoint: text)
        hash = hash * codepoint; // StrongHash(0, 0, 0, static_cast<uint32_t>(codepoint));
    return hash;
}

template <typename T, typename Alloc>
StrongHash StrongHash::compute(std::basic_string<T, Alloc> const& text) noexcept
{
    // return compute(value.data(), value.size());
    StrongHash hash;
    hash = StrongHash(0, 0, 0, static_cast<uint32_t>(text.size()));
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
    // TODO AES-NI fallback

    static_assert(sizeof(__m128i) == 16);
    auto constexpr ChunkSize = static_cast<int>(sizeof(__m128i));

    __m128i hashValue = _mm_cvtsi64_si128(static_cast<long long>(n));
    hashValue = _mm_xor_si128(hashValue, _mm_loadu_si128((__m128i const*) DefaultSeed.data()));

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
    return _mm_cvtsi128_si32(hash.value);
}

template <typename T>
struct StrongHasher
{
    StrongHash operator()(T const&) noexcept; // Specialize and implement me.
};

// {{{ some standard hash implementations
namespace detail
{
    template <typename T>
    struct StdHash32
    {
        inline StrongHash operator()(T v) noexcept { return StrongHash { _mm_set_epi32(0, 0, 0, v) }; }
    };
} // namespace detail

// clang-format off
template <> struct StrongHasher<char>: public detail::StdHash32<char> { };
template <> struct StrongHasher<unsigned char>: public detail::StdHash32<char> { };
template <> struct StrongHasher<short>: public detail::StdHash32<short> { };
template <> struct StrongHasher<unsigned short>: public detail::StdHash32<unsigned short> { };
template <> struct StrongHasher<int>: public detail::StdHash32<int> { };
template <> struct StrongHasher<unsigned int>: public detail::StdHash32<int> { };
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
        return format_to(ctx.out(), "{}", to_structured_string(hash));
    }
};
} // namespace fmt
