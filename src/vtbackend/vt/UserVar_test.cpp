// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/vt/UserVar.hpp>

#include <core/Base64.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>
#include <string_view>

using vtbackend::decodeUserVar;
using vtbackend::UserVarLimits;
using vtbackend::UserVarRejection;

TEST_CASE("decodeUserVar.accepts", "[uservar]")
{
    struct Row
    {
        std::string_view argument;
        std::string_view name;
        std::string_view value;
    };
    constexpr auto Rows = std::array {
        Row { "foo=YmFy", "foo", "bar" },
        Row { "foo=YmE=", "foo", "ba" },
        Row { "foo=YmE", "foo", "ba" },
        Row { "foo=", "foo", "" },
    };
    for (auto const& row: Rows)
    {
        INFO(row.argument);
        auto const result = decodeUserVar(row.argument, UserVarLimits {});
        REQUIRE(result.has_value());
        CHECK(result->name == row.name);
        CHECK(result->value == row.value);
    }
}

TEST_CASE("decodeUserVar.rejects", "[uservar]")
{
    struct Row
    {
        std::string_view argument;
        UserVarRejection reason;
    };
    constexpr auto Rows = std::array {
        Row { "foo", UserVarRejection::MissingSeparator },
        Row { "=YmFy", UserVarRejection::InvalidName },
        Row { "a\x01"
              "b=YmFy",
              UserVarRejection::InvalidName },
        Row { "a b=YmFy", UserVarRejection::InvalidName }, // space
        Row { "a\x7f"
              "b=YmFy",
              UserVarRejection::InvalidName }, // DEL
        Row { "a\xc2\x85"
              "b=YmFy",
              UserVarRejection::InvalidName },                     // C1 NEL as UTF-8
        Row { "caf\xc3\xa9=YmFy", UserVarRejection::InvalidName }, // a high byte
        Row { "foo=Y m!y", UserVarRejection::InvalidValue },
        Row { "foo=Y", UserVarRejection::InvalidValue },
        Row { "foo=YmFy$", UserVarRejection::InvalidValue },
        Row { "foo=Y===", UserVarRejection::InvalidValue },
        Row { "a=b=Yg==", UserVarRejection::InvalidValue }, // the first '=' splits
    };
    for (auto const& row: Rows)
    {
        INFO(row.argument);
        auto const result = decodeUserVar(row.argument, UserVarLimits {});
        REQUIRE(!result.has_value());
        CHECK(result.error() == row.reason);
    }
}

TEST_CASE("decodeUserVar.bounds", "[uservar]")
{
    auto const limits = UserVarLimits {};
    auto const argumentOf = [](std::string const& name, std::size_t valueBytes) {
        return name + "=" + core::base64::encode(std::string(valueBytes, 'x'));
    };

    CHECK(decodeUserVar(argumentOf(std::string(limits.maxNameBytes, 'n'), 1), limits).has_value());
    auto const longName = decodeUserVar(argumentOf(std::string(limits.maxNameBytes + 1, 'n'), 1), limits);
    REQUIRE(!longName.has_value());
    CHECK(longName.error() == UserVarRejection::InvalidName);

    auto const atLimit = decodeUserVar(argumentOf("v", limits.maxValueBytes), limits);
    REQUIRE(atLimit.has_value());
    CHECK(atLimit->value.size() == limits.maxValueBytes);
    auto const over = decodeUserVar(argumentOf("v", limits.maxValueBytes + 1), limits);
    REQUIRE(!over.has_value());
    CHECK(over.error() == UserVarRejection::ValueTooLong);
}
