// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <string>

#include <vthost/control/StatusText.hpp>

using vthost::control::sanitizeStatusText;

namespace
{
constexpr auto Replacement = std::string_view { "\xEF\xBF\xBD" };
}

TEST_CASE("sanitizeStatusText.stripsControlsAndBounds", "[vthost][control][statustext]")
{
    CHECK(sanitizeStatusText("ok\x1b]52;c;ZXZpbA==\x07 done") == "ok]52;c;ZXZpbA== done");
    // Built from its bytes: a literal carrying the override is what the bidi lint exists to refuse.
    auto const withBidi = std::string { 'a', '\xE2', '\x80', '\xAE', 'b' }; // U+202E between a and b
    CHECK(sanitizeStatusText(withBidi) == "ab");
    auto const invalid = std::string { '\xff', '\xfe', 'x' };
    CHECK(sanitizeStatusText(invalid) == std::string { Replacement } + std::string { Replacement } + "x");
    CHECK(sanitizeStatusText(std::string(10'000, 'x')).size() == 200);
    CHECK(sanitizeStatusText(std::string(199, 'x') + "\xC3\xA9").size() == 199); // no split code point
}

TEST_CASE("sanitizeStatusText.keepsWellFormedText", "[vthost][control][statustext]")
{
    CHECK(sanitizeStatusText("Run \xC3\xA9\xE2\x9C\x93 tests?") == "Run \xC3\xA9\xE2\x9C\x93 tests?");
    CHECK(sanitizeStatusText("c1\xC2\x9b[31m") == "c1[31m"); // C1 CSI introducer removed
    CHECK(sanitizeStatusText("\xC0\xAF")
          == std::string { Replacement } + std::string { Replacement });        // overlong
    CHECK(sanitizeStatusText("\xED\xA0\x80").size() == 3 * Replacement.size()); // surrogate: three bad bytes
    CHECK(sanitizeStatusText("abc", 2) == "ab");
    CHECK(sanitizeStatusText("").empty());
}

TEST_CASE("sanitizeStatusText.showsLineBreaksAsOneSpace", "[vthost][control][statustext]")
{
    CHECK(sanitizeStatusText("a\nb") == "a b");
    CHECK(sanitizeStatusText("a\r\n\r\nb") == "a b");
    CHECK(sanitizeStatusText("\r\ndone it\n") == "done it"); // no space at either end
    CHECK(sanitizeStatusText("a\xE2\x80\xA8"
                             "b")
          == "a b"); // U+2028 LINE SEPARATOR
    CHECK(sanitizeStatusText("a\xC2\x85"
                             "b")
          == "a b"); // U+0085 NEXT LINE
}

TEST_CASE("sanitizeStatusText.dropsEveryInvisibleFormatCharacter", "[vthost][control][statustext]")
{
    // Built from their bytes, as the bidi lint asks: U+0600 (Arabic number sign), U+061C (Arabic letter
    // mark), U+3164 (Hangul filler), U+E0041 (tag A), U+1D173 (musical symbol begin beam).
    CHECK(sanitizeStatusText(std::string { 'a', '\xD8', '\x80', 'b' }) == "ab");
    CHECK(sanitizeStatusText(std::string { 'a', '\xD8', '\x9C', 'b' }) == "ab");
    CHECK(sanitizeStatusText(std::string { 'a', '\xE3', '\x85', '\xA4', 'b' }) == "ab");
    CHECK(sanitizeStatusText(std::string { 'a', '\xF3', '\xA0', '\x81', '\x81', 'b' }) == "ab");
    CHECK(sanitizeStatusText(std::string { 'a', '\xF0', '\x9D', '\x85', '\xB3', 'b' }) == "ab");
    // A variation selector only picks a glyph's form: the emoji keeps its colour presentation.
    auto const checkMark = std::string { '\xE2', '\x9C', '\x94', '\xEF', '\xB8', '\x8F' }; // U+2714 U+FE0F
    CHECK(sanitizeStatusText(checkMark) == checkMark);
}
