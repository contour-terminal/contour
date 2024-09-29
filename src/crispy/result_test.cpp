// SPDX-License-Identifier: Apache-2.0
#include <crispy/result.h>

#include <catch2/catch_test_macros.hpp>

#include <format>
#include <type_traits>

using crispy::failure;

namespace
{

enum class error_code : uint8_t
{
    E0,
    E1,
    E2,
    E3,
    E4,
    E5
};

enum class another_error : uint8_t
{
    E0,
    E1,
    E2,
    E3,
    E4,
    E5
};

constexpr error_code nextErrorCode(error_code e) noexcept
{
    return e == error_code::E5 ? error_code::E0 : error_code(int(e) + 1);
}

constexpr crispy::result<int, error_code> tryIntoNextErrorResult(error_code e) noexcept
{
    return failure(nextErrorCode(e));
}

} // namespace

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
    using result = crispy::result<int, error_code>;
    using another_result = crispy::result<int, another_error>;

    // test rvalue reference
    auto const some = result { crispy::failure { error_code::E1 } };
    auto const b = some.transform_error(
        [](error_code e) -> another_result { return failure { another_error(int(e) + 1) }; });
    static_assert(std::is_same_v<decltype(b), another_result const>);
    REQUIRE(b.error() == another_error::E2);

    // test rvalue reference chain
    auto const b2 =
        b.transform_error([](another_error e) {
             return failure { another_error(int(e) + 1) };
         }).transform_error([](another_error e) { return failure { another_error(int(e) + 1) }; });
    REQUIRE(b2.error() == another_error::E4);

    // test const lvalue reference
    auto const c = b.transform_error([](another_error e) { return failure { error_code(int(e) - 1) }; });
    static_assert(std::is_same_v<decltype(c), result const>);
    REQUIRE(c.error() == error_code::E1);
}

TEST_CASE("result.and_then")
{
    using result = crispy::result<int, std::error_code>;

    // test rvalue reference
    auto const b = result { 10 }
                       .and_then([](int x) -> result { return result { x * 10 }; })
                       .and_then([](int x) -> result { return result { x + 1 }; })
                       .and_then([](int x) -> result { return result { x * 2 }; });

    static_assert(std::is_same_v<decltype(*b), int const&>);
    REQUIRE(b.value() == 202);

    // test const lvalue reference
    auto const c = b.and_then([](int x) -> result { return result { x / 2 }; });
    REQUIRE(c.value() == 101);
}

TEST_CASE("result.or_else")
{
    using result = crispy::result<int, error_code>;

    // test rvalue reference
    auto const b = result { failure { error_code::E1 } }.or_else(tryIntoNextErrorResult);

    static_assert(std::is_same_v<decltype(*b), int const&>);
    REQUIRE(b.error() == error_code::E2);

    // test const lvalue reference
    auto const c = b.or_else([](error_code e) -> result { return failure { error_code(int(e) + 1) }; });
    REQUIRE(c.error() == error_code::E3);

    // ensure chaining works over rvalues
    auto const b2 = result { failure { error_code::E1 } } // E1
                        .or_else([](error_code) { return result { 12 }; })
                        .or_else([](error_code) { return result { 13 }; });
    REQUIRE(b2.value() == 12);

    // ensure chaining works over lvalues
    auto const someError = result { failure { error_code::E1 } };
    auto const c2 = someError // E1
                        .or_else([](error_code) { return result { 14 }; })
                        .or_else([](error_code) { return result { 15 }; })
                        .or_else([](error_code) { return result { 16 }; });
    REQUIRE(c2.value() == 14);
}

TEST_CASE("result.emplace")
{
    auto a = crispy::result<int, error_code> {};
    a.emplace(2);
    REQUIRE(a.value() == 2);
}
