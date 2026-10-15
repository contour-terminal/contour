// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/vt/JsonString.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string_view>

using namespace vtbackend;

namespace
{

/// One text, and the JSON string it is spelled as.
struct JsonStringCase
{
    std::string_view name;     ///< What the row pins, for the test's report.
    std::string_view text;     ///< The text.
    std::string_view expected; ///< The JSON string, quotes included.
};

constexpr auto JsonStringCases = std::array {
    JsonStringCase { .name = "plain text", .text = "ls -la", .expected = R"("ls -la")" },
    JsonStringCase { .name = "quote and backslash", .text = R"(a"b\c)", .expected = R"("a\"b\\c")" },
    JsonStringCase { .name = "the C0 codes JSON has a short form for",
                     .text = "\b\f\n\r\t",
                     .expected = R"("\b\f\n\r\t")" },
    JsonStringCase {
        .name = "any other C0", .text = "\x01\x1b[0m\x1f", .expected = R"("\u0001\u001b[0m\u001f")" },
    JsonStringCase { .name = "DEL", .text = "a\x7f", .expected = R"("a\u007f")" },
    JsonStringCase { .name = "C1, as UTF-8 spells it",
                     .text = "\xC2\x80\xC2\x9B\xC2\x9C\xC2\x9F",
                     .expected = R"("\u0080\u009b\u009c\u009f")" },
    JsonStringCase { .name = "printable text past ASCII, the first after C1 included",
                     .text = "\xC2\xA0\xC3\xBC\xE4\xB8\xAD\xF0\x9F\x98\x80",
                     .expected = "\"\xC2\xA0\xC3\xBC\xE4\xB8\xAD\xF0\x9F\x98\x80\"" },
    JsonStringCase { .name = "a byte that is not UTF-8: no raw C1 byte either",
                     .text = "a\x9C"
                             "b",
                     .expected = "\"a\xEF\xBF\xBD"
                                 "b\"" },
    JsonStringCase { .name = "nothing", .text = "", .expected = R"("")" },
};

} // namespace

TEST_CASE("JsonString.escapesEveryControlCode", "[semanticblocks]")
{
    for (auto const& testCase: JsonStringCases)
    {
        INFO(testCase.name);
        CHECK(jsonString(testCase.text) == testCase.expected);
    }
}
