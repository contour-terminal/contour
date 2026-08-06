// SPDX-License-Identifier: Apache-2.0
#include <crispy/ring.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <format>
#include <iostream>

using crispy::FixedSizeRing;
using crispy::Ring;
using std::generate_n;

namespace
{
template <typename T>
[[maybe_unused]] void dump(Ring<T> const& r)
{
    std::cout << std::format("Ring(@{}): {{", r.zeroIndex());
    for (size_t i = 0; i < r.size(); ++i)
    {
        if (i)
            std::cout << std::format(", ");
        std::cout << std::format("{}", r[i]);
    }
    std::cout << std::format("}}\n");
}
} // namespace

TEST_CASE("Ring.init")
{
    Ring<char> r(3, {});
    generate_n(r.begin(), 3, [c = 'a']() mutable { return c++; });
    REQUIRE(r[0] == 'a');
    REQUIRE(r[1] == 'b');
    REQUIRE(r[2] == 'c');
}

TEST_CASE("Ring.push_back")
{
    Ring<char> r;
    r.push_back('a');
    r.push_back('b');
    r.push_back('c');
    REQUIRE(r[0] == 'a');
    REQUIRE(r[1] == 'b');
    REQUIRE(r[2] == 'c');
}

TEST_CASE("Ring.emplace_back")
{
    Ring<char> r;
    r.emplace_back('a');
    r.emplace_back('b');
    r.emplace_back('c');
    REQUIRE(r[0] == 'a');
    REQUIRE(r[1] == 'b');
    REQUIRE(r[2] == 'c');
}

TEST_CASE("Ring.rotateRight")
{
    Ring<char> r(3, {});
    generate_n(r.begin(), 3, [c = 'a']() mutable { return c++; });
    r.rotateRight(1);
    REQUIRE(r[0] == 'c');
    REQUIRE(r[1] == 'a');
    REQUIRE(r[2] == 'b');
}

TEST_CASE("Ring.rotate_right_2")
{
    Ring<char> r(3, {});
    generate_n(r.begin(), 3, [c = 'a']() mutable { return c++; });
    r.rotateRight(2);
    REQUIRE(r[0] == 'b');
    REQUIRE(r[1] == 'c');
    REQUIRE(r[2] == 'a');
}

TEST_CASE("Ring.rotateLeft")
{
    Ring<char> r(3, {});
    generate_n(r.begin(), 3, [c = 'a']() mutable { return c++; });
    r.rotateLeft(1);
    REQUIRE(r[0] == 'b');
    REQUIRE(r[1] == 'c');
    REQUIRE(r[2] == 'a');
}

TEST_CASE("Ring.rotate_left_2")
{
    Ring<char> r(3, {});
    generate_n(r.begin(), 3, [c = 'a']() mutable { return c++; });
    r.rotateLeft(2);
    REQUIRE(r[0] == 'c');
    REQUIRE(r[1] == 'a');
    REQUIRE(r[2] == 'b');
}

TEST_CASE("Ring.rotate_left_3")
{
    Ring<char> r(3, {});
    generate_n(r.begin(), 3, [c = 'a']() mutable { return c++; });
    r.rotateLeft(3);
    REQUIRE(r[0] == 'a');
    REQUIRE(r[1] == 'b');
    REQUIRE(r[2] == 'c');
}

TEST_CASE("Ring.rezero")
{
    Ring<char> r(6, {});
    generate_n(r.begin(), r.size(), [c = 'a']() mutable { return c++; });

    r.rotateRight(2);
    r.rezero();
    REQUIRE(r[0] == 'e');
    REQUIRE(r[1] == 'f');
    REQUIRE(r[2] == 'a');
    REQUIRE(r[3] == 'b');
    REQUIRE(r[4] == 'c');
    REQUIRE(r[5] == 'd');
}

TEST_CASE("Ring.rezero.iterator")
{
    Ring<char> r(6);
    generate_n(r.begin(), r.size(), [c = 'a']() mutable { return c++; });
    r.rezero(std::next(r.begin(), 2));
    REQUIRE(r[0] == 'c');
    REQUIRE(r[1] == 'd');
    REQUIRE(r[2] == 'e');
    REQUIRE(r[3] == 'f');
    REQUIRE(r[4] == 'a');
    REQUIRE(r[5] == 'b');
}

TEST_CASE("Ring.fixed_size")
{
    FixedSizeRing<char, 6> r;
    generate_n(r.begin(), r.size(), [c = 'a']() mutable { return c++; });
    REQUIRE(r[0] == 'a');
    REQUIRE(r[1] == 'b');
    REQUIRE(r[2] == 'c');
    REQUIRE(r[3] == 'd');
    REQUIRE(r[4] == 'e');
    REQUIRE(r[5] == 'f');
}

TEST_CASE("Ring.offset_negative")
{
    Ring<char> r;
    r.emplace_back('a');
    r.emplace_back('b');
    r.emplace_back('c');

    REQUIRE(r[0] == 'a');
    REQUIRE(r[1] == 'b');
    REQUIRE(r[2] == 'c');
    REQUIRE(r[-1] == 'c');
    REQUIRE(r[-2] == 'b');
    REQUIRE(r[-3] == 'a');
}
