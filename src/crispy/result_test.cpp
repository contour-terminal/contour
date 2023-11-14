// SPDX-License-Identifier: Apache-2.0
#include <crispy/result.h>

#include <fmt/format.h>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("result.transform")
{
    using result = crispy::result<int, std::error_code>;

    auto const b = result { 10 }
                       .transform([](int x) { return x * 10; })
                       .transform([](int x) { return x + 1; })
                       .transform([](int x) { return std::to_string(x); })
                       .value();

    static_assert(std::is_same_v<decltype(b), std::string const>);
    REQUIRE(b == "101");
}

TEST_CASE("result.transform_error")
{
    using result = crispy::result<int, int>;

    auto const b = result { crispy::failure(10) }
                       .transform_error([](int x) { return x * 10; })
                       .transform_error([](int x) { return x + 1; })
                       .transform_error([](int x) { return std::to_string(x); })
                       .error();
    static_assert(std::is_same_v<decltype(b), std::string const>);

    REQUIRE(b == "101");
}
