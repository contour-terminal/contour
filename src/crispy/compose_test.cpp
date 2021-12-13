/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
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
#include <crispy/compose.h>

#include <catch2/catch_all.hpp>

TEST_CASE("compose.simple")
{
    auto constexpr doubled = [](int v) {
        return v + v;
    };
    auto constexpr squared = [](int v) {
        return v * v;
    };
    auto constexpr a0 = 1;
    auto constexpr res = a0 >> compose(doubled) >> compose(squared);
    static_assert(res == 4);
}

TEST_CASE("compose.withArgs")
{
    auto constexpr a0 = 1;
    auto constexpr a1 = [](int c, int v) {
        return c + v;
    };
    auto constexpr a2 = [](int c1, int c2, int v) {
        return c1 + c2 + v;
    };
    auto constexpr a3 = [](int c1, int c2, int c3, int v) {
        return c1 + c2 + c3 + v;
    };
    auto constexpr res = a0 >> compose(a1, 2) >> compose(a2, 3, 4) >> compose(a3, 5, 6, 7);
    static_assert(28 == res);
}
