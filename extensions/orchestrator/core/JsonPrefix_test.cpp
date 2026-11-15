// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string_view>

#include <orchestrator/core/JsonPrefix.hpp>

using namespace std::string_view_literals;

namespace
{

/// Whether a cut array leaves entries to read.
enum class Prefix : uint8_t
{
    None,  ///< No array, or no entry of it closed.
    Found, ///< The entries a comma closed, as an array.
};

struct PrefixRow
{
    std::string_view name;
    std::string_view text;
    Prefix prefix;
    std::string_view expected; ///< The array read, when there is one.
};

constexpr auto PrefixRows = std::array {
    PrefixRow { .name = "cut in the second entry",
                .text = R"([{"n":1},{"n":2)",
                .prefix = Prefix::Found,
                .expected = R"([{"n":1}])" },
    PrefixRow { .name = "cut just after a comma",
                .text = R"([{"n":1},{"n":2},)",
                .prefix = Prefix::Found,
                .expected = R"([{"n":1},{"n":2}])" },
    PrefixRow { .name = "leading whitespace",
                .text = " \n[{\"n\":1},{",
                .prefix = Prefix::Found,
                .expected = " \n[{\"n\":1}]" },
    PrefixRow { .name = "commas inside nested values do not close an entry",
                .text = R"([{"a":[1,2],"b":{"c":1,"d":2}},{"a":[3,)",
                .prefix = Prefix::Found,
                .expected = R"([{"a":[1,2],"b":{"c":1,"d":2}}])" },
    PrefixRow { .name = "commas and brackets inside strings do not count",
                .text = R"([{"t":"a,]}\"x"},{"t":"b,)",
                .prefix = Prefix::Found,
                .expected = R"([{"t":"a,]}\"x"}])" },
    PrefixRow { .name = "an escaped backslash ends before the quote",
                .text = R"([{"t":"a\\"},{"t":")",
                .prefix = Prefix::Found,
                .expected = R"([{"t":"a\\"}])" },
    PrefixRow { .name = "an array that closes is whole",
                .text = R"([{"n":1},{"n":2}]trailing)",
                .prefix = Prefix::Found,
                .expected = R"([{"n":1},{"n":2}])" },
    PrefixRow {
        .name = "cut in the first entry", .text = R"([{"n":1)", .prefix = Prefix::None, .expected = "" },
    PrefixRow { .name = "only the bracket", .text = "[", .prefix = Prefix::None, .expected = "" },
    PrefixRow { .name = "empty", .text = "", .prefix = Prefix::None, .expected = "" },
    PrefixRow { .name = "an object", .text = R"({"a":1,"b":)", .prefix = Prefix::None, .expected = "" },
};

} // namespace

TEST_CASE("closedArrayPrefix keeps the entries a comma closed", "[orchestrator][json]")
{
    for (auto const& row: PrefixRows)
    {
        CAPTURE(row.name);
        auto const prefix = orchestrator::json::closedArrayPrefix(row.text);
        CHECK((prefix ? Prefix::Found : Prefix::None) == row.prefix);
        if (prefix)
            CHECK(*prefix == row.expected);
    }
}
