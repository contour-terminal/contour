// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <crispy/assert.h>

#include <array>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>

// NB: That should be defined via CMakeLists.txt's option()...
// #define STRONGHASH_USE_INTRINSICS 1

#if defined(STRONGHASH_USE_INTRINSICS)
    #include <crispy/Intrinsics.h>
#else
    #include <crispy/FNV.h>
#endif

namespace crispy
{

struct strong_hash
{
    // some random seed
    static constexpr std::array<unsigned char, 16> DefaultSeed = {
        // NOLINT(readability-identifier-naming)
        114, 188, 209, 2, 232, 4, 178, 176, 240, 216, 201, 127, 40, 41, 95, 143,
    };

    strong_hash(uint32_t a, uint32_t b, uint32_t c, uint32_t d) noexcept;

#if defined(STRONGHASH_USE_INTRINSICS)
    explicit strong_hash(Intrinsics::m128i v) noexcept;
#endif

    strong_hash() = default;
    strong_hash(strong_hash const&) = default;
    strong_hash& operator=(strong_hash const&) = default;
    strong_hash(strong_hash&&) noexcept = default;
    strong_hash& operator=(strong_hash&&) noexcept = default;

    template <typename T>
    static strong_hash compute(T const& value) noexcept;

    template <typename T>
    static strong_hash compute(std::basic_string_view<T> text) noexcept;

    template <typename T, typename Alloc>
    static strong_hash compute(std::basic_string<T, Alloc> const& text) noexcept;

    static strong_hash compute(void const* data, size_t n) noexcept;

    // Retrieves the 4th 32-bit component of the internal representation.
    // TODO: Provide access also to: a, b, c.
    [[nodiscard]] uint32_t d() const noexcept
    {
#if defined(STRONGHASH_USE_INTRINSICS)
        return static_cast<uint32_t>(Intrinsics::castToInt32(value));
#else
        return value[3];
#endif
    }

#if defined(STRONGHASH_USE_INTRINSICS)
    Intrinsics::m128i value {};
#else
    std::array<uint32_t, 4> value;
#endif
};

inline strong_hash::strong_hash(uint32_t a, uint32_t b, uint32_t c, uint32_t d) noexcept:
#if defined(STRONGHASH_USE_INTRINSICS)
    strong_hash(Intrinsics::xor128(Intrinsics::load32(a, b, c, d),
                                   Intrinsics::loadUnaligned((__m128i const*) defaultSeed.data())))
#else
    value { a, b, c, d }
#endif
{
}

#if defined(STRONGHASH_USE_INTRINSICS)
inline strong_hash::strong_hash(__m128i v) noexcept: value { v }
{
}
#endif

inline bool operator==(strong_hash a, strong_hash b) noexcept
{
#if defined(STRONGHASH_USE_INTRINSICS)
    return Intrinsics::compare(a.value, b.value);
#else
    return a.value == b.value;
#endif
}

inline bool operator!=(strong_hash a, strong_hash b) noexcept
{
    return !(a == b);
}

inline strong_hash operator*(strong_hash const& a, strong_hash const& b) noexcept
{
#if defined(STRONGHASH_USE_INTRINSICS)
    Intrinsics::m128i hashValue = a.value;

    hashValue = Intrinsics::xor128(hashValue, b.value);
    hashValue = Intrinsics::aesdec(hashValue, Intrinsics::setzero());
    hashValue = Intrinsics::aesdec(hashValue, Intrinsics::setzero());
    hashValue = Intrinsics::aesdec(hashValue, Intrinsics::setzero());
    hashValue = Intrinsics::aesdec(hashValue, Intrinsics::setzero());

    return strong_hash { hashValue };
#else
    return strong_hash { fnv<uint32_t, uint32_t>()(a.value[0], b.value[0]),
                         fnv<uint32_t, uint32_t>()(a.value[1], b.value[1]),
                         fnv<uint32_t, uint32_t>()(a.value[2], b.value[2]),
                         fnv<uint32_t, uint32_t>()(a.value[3], b.value[3]) };
#endif
}

inline strong_hash operator*(strong_hash a, uint32_t b) noexcept
{
    return a * strong_hash(0, 0, 0, b);
}

template <typename T>
strong_hash strong_hash::compute(std::basic_string_view<T> text) noexcept
{
    // return compute(value.data(), value.size());
    auto hash = strong_hash(0, 0, 0, static_cast<uint32_t>(text.size()));
    for (auto const codepoint: text)
        hash = hash * codepoint; // StrongHash(0, 0, 0, static_cast<uint32_t>(codepoint));
    return hash;
}

template <typename T, typename Alloc>
strong_hash strong_hash::compute(std::basic_string<T, Alloc> const& text) noexcept
{
    // return compute(value.data(), value.size());
    auto hash = strong_hash(0, 0, 0, static_cast<uint32_t>(text.size()));
    for (T const codepoint: text)
        hash = hash * static_cast<uint32_t>(codepoint);
    return hash;
}

template <typename T>
strong_hash strong_hash::compute(T const& value) noexcept
{
    return compute(&value, sizeof(value));
}

inline strong_hash strong_hash::compute(void const* data, size_t n) noexcept
{
#if defined(STRONGHASH_USE_INTRINSICS)
    static_assert(sizeof(__m128i) == 16);
    auto constexpr ChunkSize = static_cast<int>(sizeof(__m128i));

    __m128i hashValue = Intrinsics::cvtsi64_si128(static_cast<long long>(n));
    hashValue = Intrinsics::xor128(hashValue, Intrinsics::loadUnaligned((__m128i const*) defaultSeed.data()));

    char const* inputPtr = (char const*) data;
    for (int chunkIndex = 0; chunkIndex < static_cast<int>(n) / ChunkSize; chunkIndex++)
    {
        auto chunk = Intrinsics::loadUnaligned((Intrinsics::m128i const*) inputPtr);

        hashValue = Intrinsics::xor128(hashValue, chunk);
        hashValue = Intrinsics::aesdec(hashValue, Intrinsics::setzero());
        hashValue = Intrinsics::aesdec(hashValue, Intrinsics::setzero());
        hashValue = Intrinsics::aesdec(hashValue, Intrinsics::setzero());
        hashValue = Intrinsics::aesdec(hashValue, Intrinsics::setzero());

        inputPtr += ChunkSize;
    }

    auto const remainingByteCount = n % ChunkSize;
    if (remainingByteCount)
    {
        char lastChunk[ChunkSize] { 0 };
        std::memcpy(lastChunk + ChunkSize - remainingByteCount, inputPtr, remainingByteCount);
        __m128i chunk = Intrinsics::loadUnaligned((__m128i*) lastChunk);
        hashValue = Intrinsics::xor128(hashValue, chunk);
        hashValue = Intrinsics::aesdec(hashValue, Intrinsics::setzero());
        hashValue = Intrinsics::aesdec(hashValue, Intrinsics::setzero());
        hashValue = Intrinsics::aesdec(hashValue, Intrinsics::setzero());
        hashValue = Intrinsics::aesdec(hashValue, Intrinsics::setzero());
    }

    return strong_hash { hashValue };
#else
    auto const* i = (uint8_t const*) data;
    auto const* e = i + n;
    auto const result = fnv<uint8_t, uint64_t>()(i, e);
    auto constexpr A = 0;
    auto constexpr B = 0;
    auto const c = static_cast<uint32_t>((result >> 32) & 0xFFFFFFFFu);
    auto const d = static_cast<uint32_t>(result & 0xFFFFFFFFu);
    return strong_hash { A, B, c, d };
#endif
}

inline std::string to_string(strong_hash const& hash)
{
    uint32_t u32[4];
    std::memcpy(u32, &hash, sizeof(hash));
    return std::format("{:04X}{:04X}{:04X}{:04X}", u32[0], u32[1], u32[2], u32[3]);
}

inline std::string to_structured_string(strong_hash const& hash)
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
            s += std::format("{:X}", u32[i]);
    }
    return s;
}

inline int to_integer(strong_hash hash) noexcept
{
#if defined(STRONGHASH_USE_INTRINSICS)
    return Intrinsics::castToInt32(hash.value);
#else
    return static_cast<int>(hash.value.back());
#endif
}

template <typename T>
struct strong_hasher
{
    strong_hash operator()(T const&) noexcept; // Specialize and implement me.
};

template <>
struct strong_hasher<uint64_t>
{
    inline strong_hash operator()(uint64_t v) noexcept
    {
        auto const c = static_cast<uint32_t>((v >> 32) & 0xFFFFFFFFu);
        auto const d = static_cast<uint32_t>(v & 0xFFFFFFFFu);
#if defined(STRONGHASH_USE_INTRINSICS)
        return strong_hash { Intrinsics::load32(0, 0, c, d) };
#else
        return strong_hash { 0, 0, c, d };
#endif
    }
};
// {{{ some standard hash implementations
namespace detail
{
    template <typename T>
    struct std_hash32
    {
        inline strong_hash operator()(T v) noexcept
        {
#if defined(STRONGHASH_USE_INTRINSICS)
            return strong_hash { Intrinsics::load32(0, 0, 0, v) };
#else
            return strong_hash { 0, 0, 0, static_cast<uint32_t>(v) };
#endif
        }
    };
} // namespace detail

// clang-format off
template <> struct strong_hasher<char>: public detail::std_hash32<char> { };
template <> struct strong_hasher<unsigned char>: public detail::std_hash32<char> { };
template <> struct strong_hasher<int16_t>: public detail::std_hash32<int16_t> { };
template <> struct strong_hasher<uint16_t>: public detail::std_hash32<uint16_t> { };
template <> struct strong_hasher<int32_t>: public detail::std_hash32<int32_t> { };
template <> struct strong_hasher<uint32_t>: public detail::std_hash32<uint32_t> { };
// clang-format on
// }}}

template <class Char, class Allocator>
struct strong_hasher<std::basic_string<Char, Allocator>>
{
    inline strong_hash operator()(std::basic_string<Char, Allocator> const& v) noexcept
    {
        return strong_hasher<int> {}((int) std::hash<std::basic_string<Char, Allocator>> {}(v));
    }
};

template <typename U>
struct strong_hasher<std::basic_string_view<U>>
{
    inline strong_hash operator()(std::basic_string_view<U> v) noexcept
    {
        return strong_hasher<int> {}((int) std::hash<std::basic_string_view<U>> {}(v));
    }
};

} // namespace crispy

template <>
struct std::formatter<crispy::strong_hash>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(crispy::strong_hash const& hash, FormatContext& ctx) const
    {
        return std::format_to(ctx.out(), "{}", to_structured_string(hash));
    }
};
