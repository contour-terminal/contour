// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/testing/GlyfRecordBuilder.hpp>
#include <vtbackend/vt/GlyphOutline.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

using namespace vtbackend;
using namespace vtbackend::glyf_test;

TEST_CASE("decodeGlyf: reads contours and computes the bounds itself", "[GlyphOutline]")
{
    auto const contours = std::vector<Contour> {
        { on(100, -200), off(300, 900), on(500, -200) },
        { on(0, 0), on(10, 0), on(10, 10) },
    };
    auto const outline = decodeGlyf(record(contours));
    REQUIRE(outline.has_value());
    REQUIRE(outline->contourEnds == std::vector<uint16_t> { 2, 5 });
    CHECK(std::ranges::equal(outline->contour(0), contours[0]));
    CHECK(std::ranges::equal(outline->contour(1), contours[1]));
    // Not the 0x7FFF the record's header claims.
    CHECK(outline->bounds == GlyphOutlineBounds { .xMin = 0, .yMin = -200, .xMax = 500, .yMax = 900 });
}

TEST_CASE("decodeGlyf: reads short vectors, same-as-previous and repeated flags", "[GlyphOutline]")
{
    // Four on-curve points: (10, 0), (10, 0), (5, 20), (5, 20).
    auto const data = std::vector<uint8_t> {
        0x00, 0x01,                                     // one contour
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // bounds, unread
        0x00, 0x03,                                     // it ends at point 3
        0x00, 0x00,                                     // no instructions
        0x33,                                           // point 0: on, x a positive byte, y unchanged
        0x31,                                           // point 1: on, x and y unchanged
        0x09, 0x01,                                     // points 2 and 3: on, x and y as words; repeated once
        0x0A,                                           // x of point 0: +10
        0xFF, 0xFB,                                     // x of point 2: -5
        0x00, 0x00,                                     // x of point 3: 0
        0x00, 0x14,                                     // y of point 2: +20
        0x00, 0x00,                                     // y of point 3: 0
    };
    auto const outline = decodeGlyf(data);
    REQUIRE(outline.has_value());
    REQUIRE(outline->points.size() == 4);
    CHECK(outline->points[0] == on(10, 0));
    CHECK(outline->points[1] == on(10, 0));
    CHECK(outline->points[2] == on(5, 20));
    CHECK(outline->points[3] == on(5, 20));
}

TEST_CASE("decodeGlyf: a record without contours is an empty outline", "[GlyphOutline]")
{
    auto const outline = decodeGlyf(std::vector<uint8_t>(10, 0));
    REQUIRE(outline.has_value());
    CHECK(outline->points.empty());
    CHECK(outline->contourEnds.empty());
}

TEST_CASE("decodeGlyf: refuses what the protocol's subset leaves out", "[GlyphOutline]")
{
    SECTION("a composite glyph")
    {
        auto data = squareRecord();
        data[0] = 0xFF;
        data[1] = 0xFF;
        CHECK(decodeGlyf(data) == std::unexpected { GlyphRegisterError::CompositeUnsupported });
    }

    SECTION("hinting instructions")
    {
        auto data = squareRecord();
        data[13] = 1; // instructionLength, after the header and one contour end
        CHECK(decodeGlyf(data) == std::unexpected { GlyphRegisterError::HintingUnsupported });
    }

    SECTION("a record cut short")
    {
        auto const whole = squareRecord();
        for (auto const size: { size_t { 0 }, size_t { 9 }, size_t { 11 }, size_t { 13 }, whole.size() - 1 })
            CHECK(decodeGlyf(std::span { whole }.first(size))
                  == std::unexpected { GlyphRegisterError::MalformedPayload });
    }

    SECTION("contour ends that do not increase")
    {
        auto const data = std::vector<uint8_t> { 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 0, 3, 0, 0 };
        CHECK(decodeGlyf(data) == std::unexpected { GlyphRegisterError::MalformedPayload });
    }

    SECTION("a flag repeated past the last point")
    {
        auto data = squareRecord();
        data[14] = 0x09; // the first flag: on-curve, repeated ...
        data[15] = 0xFF; // ... 255 more times, for a glyph of four points
        CHECK(decodeGlyf(data) == std::unexpected { GlyphRegisterError::MalformedPayload });
    }
}

TEST_CASE("decodeGlyf: judges the point count before decoding a point", "[GlyphOutline]")
{
    // A record that only claims its points: the contour table says 5462, and nothing follows it. Were the
    // count judged after decoding, this would be a malformed payload.
    auto const lastPoint = static_cast<uint16_t>(MaxGlyphOutlinePoints);
    auto const tooMany = std::vector<uint8_t> { 0,
                                                1,
                                                0,
                                                0,
                                                0,
                                                0,
                                                0,
                                                0,
                                                0,
                                                0,
                                                static_cast<uint8_t>(lastPoint >> 8),
                                                static_cast<uint8_t>(lastPoint & 0xFF) };
    CHECK(decodeGlyf(tooMany) == std::unexpected { GlyphRegisterError::OutlineTooLarge });

    // One point fewer is within the budget, and is then found cut short.
    auto justEnough = tooMany;
    justEnough[10] = static_cast<uint8_t>((lastPoint - 1) >> 8);
    justEnough[11] = static_cast<uint8_t>((lastPoint - 1) & 0xFF);
    CHECK(decodeGlyf(justEnough) == std::unexpected { GlyphRegisterError::MalformedPayload });
}
