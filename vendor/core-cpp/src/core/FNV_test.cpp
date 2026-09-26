// SPDX-License-Identifier: Apache-2.0
#include <core/FNV.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <type_traits>

using Fnv32 = core::FNV<char, std::uint32_t>;

TEST_CASE("FNV hashes a string as FNV-1a", "[base][fnv]")
{
    CHECK(Fnv32 {}(std::string { "a" }) == 0xE40C'292CU);
    CHECK(Fnv32 {}(std::string { "abcd" }) == 0xCE34'79BDU);
}

TEST_CASE("FNV hashes a trivially copyable value byte-wise", "[base][fnv]")
{
    // Hashing each byte must not pick this overload again for the byte itself: for FNV<char>
    // an `unsigned char` byte matched `V const&` exactly, and the overload called itself forever.
    auto const fnv = Fnv32 {};
    auto const bytes = std::array<char, 4> { 'a', 'b', 'c', 'd' };
    CHECK(fnv(fnv.basis(), bytes) == 0xCE34'79BDU);
    CHECK(fnv(fnv.basis(), bytes) == fnv(std::string { "abcd" }));
}

namespace
{
/// Trivially copyable, and three bytes of padding between the members.
struct Padded
{
    std::uint8_t tag;
    std::uint32_t value;
};

/// Whether the byte-wise overload accepts @p V at all.
template <typename V>
concept HashableBytewise = requires(Fnv32 const& fnv, V const& value) { fnv(std::uint32_t {}, value); };
} // namespace

TEST_CASE("FNV: only a type whose bytes are its value is hashed byte-wise", "[base][fnv]")
{
    // A type with padding has bytes that are not part of its value: hashing them gave two
    // objects with equal members different hashes, depending on what their padding held.
    // Such a type is now rejected rather than answered wrongly.
    STATIC_CHECK(!std::has_unique_object_representations_v<Padded>);
    STATIC_CHECK(!HashableBytewise<Padded>);

    STATIC_CHECK(std::has_unique_object_representations_v<std::array<char, 4>>);
    STATIC_CHECK(HashableBytewise<std::array<char, 4>>);
}

TEST_CASE("FNV: a byte-wise hash is available at compile time", "[base][fnv]")
{
    // The bytes were read through a reinterpret_cast, which no constant evaluation may do, so
    // the constexpr on the overload could never be taken up.
    STATIC_CHECK(Fnv32 {}(Fnv32 {}.basis(), std::array<char, 4> { 'a', 'b', 'c', 'd' }) == 0xCE34'79BDU);
}
