// SPDX-License-Identifier: Apache-2.0
//
// The encodings a shell startup file comes in. Byte strings are spelled with `\x` escapes rather than
// literal characters, so no compiler or code page reads them as anything but the bytes they are.

#include <contour/platform/RcText.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <expected>
#include <string>
#include <string_view>

using namespace contour::platform;
using namespace std::string_view_literals;

TEST_CASE("RcText.decode", "[shellrc]")
{
    struct Case
    {
        std::string_view bytes;
        RcEncoding encoding;
        std::string_view utf8;
    };
    auto const cases = std::array {
        Case { .bytes = "abc"sv, .encoding = RcEncoding::Utf8, .utf8 = "abc"sv },
        Case { .bytes = ""sv, .encoding = RcEncoding::Utf8, .utf8 = ""sv },
        Case { .bytes = "\xEF\xBB\xBF"
                        "abc"sv,
               .encoding = RcEncoding::Utf8Bom,
               .utf8 = "abc"sv },
        Case { .bytes = "\xFF\xFE"
                        "a\0b\0"sv,
               .encoding = RcEncoding::Utf16LeBom,
               .utf8 = "ab"sv },
        Case { .bytes = "\xFF\xFE\xFC\x00"sv, .encoding = RcEncoding::Utf16LeBom, .utf8 = "\xC3\xBC"sv },
        Case { .bytes = "\xFF\xFE\xAC\x20"sv, .encoding = RcEncoding::Utf16LeBom, .utf8 = "\xE2\x82\xAC"sv },
        // U+1F600 as the surrogate pair D83D DE00.
        Case { .bytes = "\xFF\xFE\x3D\xD8\x00\xDE"sv,
               .encoding = RcEncoding::Utf16LeBom,
               .utf8 = "\xF0\x9F\x98\x80"sv },
    };
    for (auto const& c: cases)
    {
        INFO(c.bytes);
        auto const text = decodeRcText(c.bytes);
        REQUIRE(text.has_value());
        CHECK(text->encoding == c.encoding);
        CHECK(text->utf8 == c.utf8);
    }

    struct Failure
    {
        std::string_view bytes;
        RcTextError error;
    };
    auto const failures = std::array {
        Failure { .bytes = "\xFF\xFE"
                           "a"sv,
                  .error = RcTextError::Malformed },
        Failure { .bytes = "\xFF\xFE\x3D\xD8"sv, .error = RcTextError::Malformed }, // lone high surrogate
        Failure { .bytes = "\xFF\xFE\x00\xDE"sv, .error = RcTextError::Malformed }, // lone low surrogate
        Failure { .bytes = "\xFF\xFE\x3D\xD8"
                           "a\0"sv,
                  .error = RcTextError::Malformed }, // high surrogate, then not a low one
        Failure { .bytes = "\xFE\xFF\0a"sv, .error = RcTextError::UnsupportedEncoding },
        Failure { .bytes = "\xFE\xFF"
                           "a\0"sv,
                  .error = RcTextError::UnsupportedEncoding },
        Failure { .bytes = "\xFF\xFE\0\0"sv, .error = RcTextError::UnsupportedEncoding },
        Failure { .bytes = "\0\0\xFE\xFF"sv, .error = RcTextError::UnsupportedEncoding },
    };
    for (auto const& f: failures)
    {
        INFO(f.bytes);
        auto const text = decodeRcText(f.bytes);
        REQUIRE_FALSE(text.has_value());
        CHECK(text.error() == f.error);
    }
}

TEST_CASE("RcText.roundTrip", "[shellrc]")
{
    auto const utf8 = "\xC3\xBC\n"sv;
    auto const utf16 = encodeRcText(utf8, RcEncoding::Utf16LeBom);
    CHECK(utf16 == std::string("\xFC\x00\n\x00", 4));

    auto const decoded = decodeRcText(std::string(byteOrderMark(RcEncoding::Utf16LeBom)) + utf16);
    REQUIRE(decoded.has_value());
    CHECK(decoded->encoding == RcEncoding::Utf16LeBom);
    CHECK(decoded->utf8 == utf8);

    auto const emoji = "a\xF0\x9F\x98\x80"
                       "b\xE2\x82\xAC"sv;
    auto const back = decodeRcText(std::string(byteOrderMark(RcEncoding::Utf16LeBom))
                                   + encodeRcText(emoji, RcEncoding::Utf16LeBom));
    REQUIRE(back.has_value());
    CHECK(back->utf8 == emoji);

    // UTF-8 encodes to itself, with or without the mark, which the caller writes only at the file's start.
    CHECK(encodeRcText(utf8, RcEncoding::Utf8) == utf8);
    CHECK(encodeRcText(utf8, RcEncoding::Utf8Bom) == utf8);
    CHECK(byteOrderMark(RcEncoding::Utf8).empty());
    CHECK(byteOrderMark(RcEncoding::Utf8Bom) == "\xEF\xBB\xBF"sv);
    CHECK(byteOrderMark(RcEncoding::Utf16LeBom) == "\xFF\xFE"sv);
}
