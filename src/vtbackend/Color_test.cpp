// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/Color.h>

#include <catch2/catch_test_macros.hpp>

using namespace vtbackend;

TEST_CASE("Color.Bright", "[Color]")
{
    Color const c = Color(BrightColor::Cyan);
    REQUIRE(isBrightColor(c));
    REQUIRE(getBrightColor(c) == int(BrightColor::Cyan));
}

TEST_CASE("Color.Indexed", "[Color]")
{
    Color const c = Color(IndexedColor::Blue);
    REQUIRE(isIndexedColor(c));
    REQUIRE(getIndexedColor(c) == int(IndexedColor::Blue));
}

TEST_CASE("Color.RGB", "[Color]")
{
    RGBColor const rgb0 = RGBColor { 0x12, 0x34, 0x56 };
    CHECK(rgb0.red == 0x12);
    CHECK(rgb0.green == 0x34);
    CHECK(rgb0.blue == 0x56);

    Color const c = Color(RGBColor { 0x12, 0x34, 0x56 });
    REQUIRE(isRGBColor(c));
    auto const rgb = getRGBColor(c);
    CHECK(rgb.red == 0x12);
    CHECK(rgb.green == 0x34);
    CHECK(rgb.blue == 0x56);
}

TEST_CASE("Color.mixColor.at_t0_returns_a", "[Color]")
{
    auto const a = RGBColor { 10, 20, 30 };
    auto const b = RGBColor { 200, 100, 50 };
    auto const result = mixColor(a, b, 0.0f);
    CHECK(result.red == a.red);
    CHECK(result.green == a.green);
    CHECK(result.blue == a.blue);
}

TEST_CASE("Color.mixColor.at_t1_returns_b", "[Color]")
{
    auto const a = RGBColor { 10, 20, 30 };
    auto const b = RGBColor { 200, 100, 50 };
    auto const result = mixColor(a, b, 1.0f);
    CHECK(result.red == b.red);
    CHECK(result.green == b.green);
    CHECK(result.blue == b.blue);
}

TEST_CASE("Color.mixColor.at_t05_returns_midpoint", "[Color]")
{
    auto const a = RGBColor { 0, 0, 0 };
    auto const b = RGBColor { 200, 100, 50 };
    auto const result = mixColor(a, b, 0.5f);
    CHECK(result.red == 100);
    CHECK(result.green == 50);
    CHECK(result.blue == 25);
}

TEST_CASE("Color.mixColor.clamps_to_valid_range", "[Color]")
{
    auto const a = RGBColor { 250, 250, 250 };
    auto const b = RGBColor { 255, 255, 255 };
    // t > 1 would overshoot without clamping
    auto const result = mixColor(a, b, 2.0f);
    CHECK(result.red == 255);
    CHECK(result.green == 255);
    CHECK(result.blue == 255);
}

TEST_CASE("Color.mixColor.RGBColorPair_overload", "[Color]")
{
    auto const a =
        RGBColorPair { .foreground = RGBColor { 0, 0, 0 }, .background = RGBColor { 100, 100, 100 } };
    auto const b =
        RGBColorPair { .foreground = RGBColor { 200, 200, 200 }, .background = RGBColor { 50, 50, 50 } };

    auto const atZero = mixColor(a, b, 0.0f);
    CHECK(atZero.foreground.red == 0);
    CHECK(atZero.background.red == 100);

    auto const atOne = mixColor(a, b, 1.0f);
    CHECK(atOne.foreground.red == 200);
    CHECK(atOne.background.red == 50);

    auto const atHalf = mixColor(a, b, 0.5f);
    CHECK(atHalf.foreground.red == 100);
    CHECK(atHalf.background.red == 75);
}

// {{{ parseColor
namespace
{
/// Asserts that @p spec parses to the given channel values, reporting @p spec on failure.
void checkColor(std::string_view spec, uint8_t red, uint8_t green, uint8_t blue)
{
    INFO("color specification: " << spec);
    auto const color = parseColor(spec);
    REQUIRE(color.has_value());
    CHECK(static_cast<int>(color->red) == static_cast<int>(red));
    CHECK(static_cast<int>(color->green) == static_cast<int>(green));
    CHECK(static_cast<int>(color->blue) == static_cast<int>(blue));
}

/// Asserts that @p spec is rejected, reporting @p spec on failure.
void checkRejected(std::string_view spec)
{
    INFO("color specification: " << spec);
    CHECK(!parseColor(spec).has_value());
}
} // namespace

TEST_CASE("Color.parseColor.rgb_digit_count_is_precision", "[Color]")
{
    // XParseColor(3): the digit count gives the channel's precision, so every width of an all-ones
    // channel means full intensity. This is the form OSC 4 queries are answered in, and the form
    // esctest and most applications send.
    checkColor("rgb:f/f/f", 0xFF, 0xFF, 0xFF);
    checkColor("rgb:ff/ff/ff", 0xFF, 0xFF, 0xFF);
    checkColor("rgb:fff/fff/fff", 0xFF, 0xFF, 0xFF);
    checkColor("rgb:ffff/ffff/ffff", 0xFF, 0xFF, 0xFF);

    checkColor("rgb:0/0/0", 0x00, 0x00, 0x00);
    checkColor("rgb:0000/0000/0000", 0x00, 0x00, 0x00);

    // The four-digit form xterm reports back, and the one Contour used to reject outright.
    checkColor("rgb:f0f0/f0f0/f0f0", 0xF0, 0xF0, 0xF0);
    checkColor("rgb:8080/0000/0000", 0x80, 0x00, 0x00);
    checkColor("rgb:aaaa/bbbb/cccc", 0xAA, 0xBB, 0xCC);
}

TEST_CASE("Color.parseColor.rgb_mixed_channel_widths", "[Color]")
{
    // Channels need not share a width. Cross-checked against ghostty's own parser tests.
    checkColor("rgb:7f/a0a0/0", 0x7F, 0xA0, 0x00);
    checkColor("rgb:f/ff/fff", 0xFF, 0xFF, 0xFF);
}

TEST_CASE("Color.parseColor.rgb_is_case_insensitive", "[Color]")
{
    checkColor("rgb:AAAA/BBBB/CCCC", 0xAA, 0xBB, 0xCC);
}

TEST_CASE("Color.parseColor.old_style_hash_is_left_justified", "[Color]")
{
    // The "old style" syntax zero-fills rather than rescales, so #fff is 0xF000 -- NOT white. This is
    // the one place where the two syntaxes deliberately disagree; see XParseColor(3).
    checkColor("#fff", 0xF0, 0xF0, 0xF0);
    checkColor("#f0f0f0", 0xF0, 0xF0, 0xF0);
    checkColor("#f00f00f00", 0xF0, 0xF0, 0xF0);
    checkColor("#f000f000f000", 0xF0, 0xF0, 0xF0);

    checkColor("#888", 0x80, 0x80, 0x80);
    checkColor("#808080", 0x80, 0x80, 0x80);
    checkColor("#800800800", 0x80, 0x80, 0x80);
    checkColor("#800080008000", 0x80, 0x80, 0x80);

    checkColor("#aabbcc", 0xAA, 0xBB, 0xCC);
    checkColor("#aaaabbbbcccc", 0xAA, 0xBB, 0xCC);

    checkColor("#123456", 0x12, 0x34, 0x56);
    checkColor("#000", 0x00, 0x00, 0x00);
}

TEST_CASE("Color.parseColor.rgbi_intensities", "[Color]")
{
    checkColor("rgbi:1/1/1", 0xFF, 0xFF, 0xFF);
    checkColor("rgbi:1.0/1.0/1.0", 0xFF, 0xFF, 0xFF);
    checkColor("rgbi:0/0/0", 0x00, 0x00, 0x00);
    checkColor("rgbi:0.0/0.0/0.0", 0x00, 0x00, 0x00);

    // 0.5 * 255 = 127.5, rounded to nearest.
    checkColor("rgbi:0.5/0.5/0.5", 0x80, 0x80, 0x80);
    checkColor("rgbi:1.0/0/0", 0xFF, 0x00, 0x00);

    // A leading or trailing decimal point is still a well-formed number.
    checkColor("rgbi:.5/1./0", 0x80, 0xFF, 0x00);

    // Digits beyond the ninth cannot move an 8-bit channel, and must not overflow the accumulator.
    checkColor("rgbi:0.5000000000000000000/0/0", 0x80, 0x00, 0x00);
}

TEST_CASE("Color.parseColor.rejects_malformed_specifications", "[Color]")
{
    checkRejected("");
    checkRejected("rgb:");
    checkRejected("rgb:f/f");        // too few channels
    checkRejected("rgb:f/f/f/f");    // too many channels
    checkRejected("rgb:fffff/f/f");  // more than four digits
    checkRejected("rgb://");         // empty channels
    checkRejected("rgb:gg/gg/gg");   // not hexadecimal
    checkRejected("rgb:-1/0/0");     // no sign is accepted
    checkRejected("#");              // no digits
    checkRejected("#ff");            // not a recognised width
    checkRejected("#fffff");         // not a recognised width
    checkRejected("#fffffffffffff"); // not a recognised width
    checkRejected("#gggggg");        // not hexadecimal
    checkRejected("rgbi:1.5/0/0");   // intensity out of range
    checkRejected("rgbi:-0.5/0/0");  // intensity out of range
    checkRejected("rgbi:./0/0");     // no digits at all
    checkRejected("rgbi:x/0/0");     // not a number
    checkRejected("CIELab:1/1/1");   // colorimetric specifications are not supported
    checkRejected("red");            // named colors are not supported
    checkRejected("0xff0000");       // not an X11 color specification
}
// }}}
