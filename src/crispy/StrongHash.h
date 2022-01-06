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

#include <cstring>
#include <immintrin.h>
#include <string>
#include <string_view>
#include <type_traits>

namespace crispy
{

struct StrongHash
{
    __m128i value {};

    // clang-format off
    StrongHash(uint32_t a, uint32_t b, uint32_t c, uint32_t d) noexcept:
        StrongHash(_mm_set_epi32(static_cast<int>(a),
                                 static_cast<int>(b),
                                 static_cast<int>(c),
                                 static_cast<int>(d))) {}
    // clang-format on

    StrongHash(__m128i v) noexcept: value { v } {}

    StrongHash() = default;
    StrongHash(StrongHash const&) = default;
    StrongHash& operator=(StrongHash const&) = default;
    StrongHash(StrongHash&&) noexcept = default;
    StrongHash& operator=(StrongHash&&) noexcept = default;
};

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

inline bool operator==(StrongHash a, StrongHash b) noexcept
{
    return _mm_movemask_epi8(_mm_cmpeq_epi32(a.value, b.value)) == 0xFFFF;
}

inline bool operator!=(StrongHash a, StrongHash b) noexcept
{
    return !(a == b);
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
