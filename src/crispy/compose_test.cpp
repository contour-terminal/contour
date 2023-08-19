// SPDX-License-Identifier: Apache-2.0
#include <crispy/compose.h>

#include <catch2/catch.hpp>

TEST_CASE("compose.simple")
{
    auto constexpr Doubled = [](int v) {
        return v + v;
    };
    auto constexpr Squared = [](int v) {
        return v * v;
    };
    auto constexpr A0 = 1;
    auto constexpr Res = A0 >> compose(Doubled) >> compose(Squared);
    static_assert(Res == 4);
}

TEST_CASE("compose.withArgs")
{
    auto constexpr A0 = 1;
    auto constexpr A1 = [](int c, int v) {
        return c + v;
    };
    auto constexpr A2 = [](int c1, int c2, int v) {
        return c1 + c2 + v;
    };
    auto constexpr A3 = [](int c1, int c2, int c3, int v) {
        return c1 + c2 + c3 + v;
    };
    auto constexpr Res = A0 >> compose(A1, 2) >> compose(A2, 3, 4) >> compose(A3, 5, 6, 7);
    static_assert(28 == Res);
}
