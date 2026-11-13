// SPDX-License-Identifier: Apache-2.0
#include <contour/window/Utf16Offsets.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string_view>
#include <vector>

using contour::window::byteOffsetsToUtf16;

TEST_CASE("byteOffsetsToUtf16 counts a 4-byte code point as two units", "[palette]")
{
    struct Case
    {
        char const* what = "";
        std::string_view text;
        std::vector<int> bytes;
        std::vector<int> expected;
    };
    auto const cases = std::array {
        Case { .what = "ASCII maps one to one", .text = "abc", .bytes = { 0, 2 }, .expected = { 0, 2 } },
        Case { .what = "an emoji is a surrogate pair",
               .text = "\xF0\x9F\x98\x80"
                       "ab",
               .bytes = { 4, 5 },
               .expected = { 2, 3 } },
        Case { .what = "a 2-byte code point at the start",
               .text = "\xC3\xA9",
               .bytes = { 0 },
               .expected = { 0 } },
        Case { .what = "past a 2-byte and a 3-byte code point",
               .text = "\xC3\xA9\xE2\x82\xAC"
                       "x",
               .bytes = { 5 },
               .expected = { 2 } },
        Case { .what = "an offset inside a code point is dropped",
               .text = "\xC3\xA9x",
               .bytes = { 1, 2 },
               .expected = { 1 } },
        Case {
            .what = "an offset past the end is dropped", .text = "ab", .bytes = { 1, 9 }, .expected = { 1 } },
        Case { .what = "a negative offset is ignored", .text = "ab", .bytes = { -1, 1 }, .expected = { 1 } },
    };
    for (auto const& c: cases)
    {
        INFO(c.what);
        CHECK(byteOffsetsToUtf16(c.text, c.bytes) == c.expected);
    }
}
