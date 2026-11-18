// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/vt/GlyphGlossary.hpp>

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cstdint>
#include <limits>
#include <vector>

#include <vthost/GlyphWire.hpp>

using namespace vthost;
using vtbackend::GlyphGlossary;
using vtbackend::GlyphLayout;
using vtbackend::GlyphOutline;
using vtbackend::GlyphOutlinePoint;
using vtbackend::GlyphPointKind;

namespace
{

/// A layout that uses every field, so a round trip that drops one is seen.
[[nodiscard]] GlyphLayout fullLayout()
{
    return GlyphLayout { .unitsPerEm = 2048,
                         .advanceWidth = 1200,
                         .lineHeight = std::nullopt,
                         .extentBottom = -400,
                         .size = vtbackend::GlyphSizeMode::Cover,
                         .horizontal = vtbackend::GlyphHorizontalAlignment::End,
                         .vertical = vtbackend::GlyphVerticalAlignment::Baseline,
                         .padding = { .top = 0.1F, .right = 0.2F, .bottom = 0.3F, .left = 0.4F },
                         .span = vtbackend::GlyphRenderSpan::TwoCells };
}

/// Two contours: a triangle with a control point, and a single point.
[[nodiscard]] GlyphOutline twoContours()
{
    auto outline =
        GlyphOutline { .points = { GlyphOutlinePoint { .x = -10, .y = 0, .kind = GlyphPointKind::OnCurve },
                                   GlyphOutlinePoint { .x = 500, .y = 900, .kind = GlyphPointKind::OffCurve },
                                   GlyphOutlinePoint { .x = 1000, .y = 0, .kind = GlyphPointKind::OnCurve },
                                   GlyphOutlinePoint { .x = 7, .y = 7, .kind = GlyphPointKind::OnCurve } },
                       .contourEnds = { 2, 3 },
                       .bounds = {} };
    outline.bounds = vtbackend::boundsOf(outline.points);
    return outline;
}

/// @return The wire form of a glossary holding the full glyph at U+100005.
[[nodiscard]] std::vector<proto::WireRegisteredGlyph> sampleWire()
{
    auto glossary = GlyphGlossary {};
    glossary.registerGlyph(0x100005, fullLayout(), twoContours());
    return glyphsToWire(glossary.changesSince(0));
}

/// @return Why @p glyph alone is refused, if it is.
[[nodiscard]] std::optional<GlyphWireError> refusalOf(proto::WireRegisteredGlyph const& glyph)
{
    auto const staged = stageGlyphUpdates(std::vector { glyph });
    return staged ? std::nullopt : std::optional { staged.error() };
}

} // namespace

TEST_CASE("GlyphWire: a glossary crosses the wire whole", "[vthost][glyph]")
{
    auto source = GlyphGlossary {};
    source.registerGlyph(0x100005, fullLayout(), twoContours());
    source.registerGlyph(0x1003FF, GlyphLayout {}, twoContours());

    auto const staged = stageGlyphUpdates(glyphsToWire(source.changesSince(0)));
    REQUIRE(staged.has_value());
    auto mirror = GlyphGlossary {};
    applyGlyphUpdates(mirror, *staged, GlyphUpdateMode::Replace);

    for (auto const codepoint: { char32_t { 0x100005 }, char32_t { 0x1003FF } })
    {
        auto const original = source.find(codepoint);
        auto const copy = mirror.find(codepoint);
        REQUIRE(copy != nullptr);
        CHECK(copy->layout == original->layout);
        CHECK(copy->outline == original->outline); // the bounds too, which the receiver computes
        // The same content, so the same atlas tiles on either side.
        CHECK(copy->id == original->id);
    }
    CHECK(mirror.find(0x100000) == nullptr);
}

TEST_CASE("GlyphWire: a cleared slot clears, and a replace clears the rest", "[vthost][glyph]")
{
    auto source = GlyphGlossary {};
    source.registerGlyph(0x100001, GlyphLayout {}, twoContours());
    source.registerGlyph(0x100002, GlyphLayout {}, twoContours());

    auto mirror = GlyphGlossary {};
    applyGlyphUpdates(
        mirror, stageGlyphUpdates(glyphsToWire(source.changesSince(0))).value(), GlyphUpdateMode::Change);
    REQUIRE(mirror.find(0x100001) != nullptr);
    REQUIRE(mirror.find(0x100002) != nullptr);

    // A change names the cleared slot and leaves the other.
    auto const before = source.generation();
    source.clear(0x100001);
    applyGlyphUpdates(mirror,
                      stageGlyphUpdates(glyphsToWire(source.changesSince(before))).value(),
                      GlyphUpdateMode::Change);
    CHECK(mirror.find(0x100001) == nullptr);
    CHECK(mirror.find(0x100002) != nullptr);

    // A replace with nothing in it empties the mirror.
    applyGlyphUpdates(mirror, {}, GlyphUpdateMode::Replace);
    CHECK(mirror.empty());
}

TEST_CASE("GlyphWire: a daemon cannot send what an application could not register", "[vthost][glyph]")
{
    auto const good = sampleWire().front();
    REQUIRE_FALSE(refusalOf(good).has_value());
    auto const with = [&](auto edit) {
        auto glyph = good;
        edit(glyph);
        return refusalOf(glyph);
    };
    auto const bitsOf = [](float value) {
        return std::bit_cast<uint32_t>(value);
    };

    SECTION("a slot outside the block, registered or cleared")
    {
        CHECK(with([](auto& g) { g.slot = 1024; }) == GlyphWireError::Slot);
        CHECK(refusalOf(proto::WireRegisteredGlyph { .slot = 65535 }) == GlyphWireError::Slot);
    }

    SECTION("a layout the protocol's parser would have refused")
    {
        CHECK(with([](auto& g) { g.unitsPerEm = 0; }) == GlyphWireError::Layout);
        CHECK(with([](auto& g) { g.advanceWidth = 0; }) == GlyphWireError::Layout); // declared, and zero
        CHECK(with([](auto& g) { g.extentFields = 0x08; }) == GlyphWireError::Layout);
        CHECK(with([](auto& g) { g.size = 5; }) == GlyphWireError::Layout);
        CHECK(with([](auto& g) { g.horizontal = 3; }) == GlyphWireError::Layout);
        CHECK(with([](auto& g) { g.vertical = 4; }) == GlyphWireError::Layout);
        CHECK(with([](auto& g) { g.span = 0; }) == GlyphWireError::Layout);
        CHECK(with([](auto& g) { g.span = 3; }) == GlyphWireError::Layout);
        CHECK(with([&](auto& g) { g.padding[0] = bitsOf(1.5F); }) == GlyphWireError::Layout);
        CHECK(with([&](auto& g) { g.padding[1] = bitsOf(-0.1F); }) == GlyphWireError::Layout);
        CHECK(with([&](auto& g) { g.padding[2] = bitsOf(std::numeric_limits<float>::quiet_NaN()); })
              == GlyphWireError::Layout);
        CHECK(with([&](auto& g) { g.padding[3] = bitsOf(std::numeric_limits<float>::infinity()); })
              == GlyphWireError::Layout);
        // Each within range, together leaving no span.
        CHECK(with([&](auto& g) { g.padding = { 0, bitsOf(0.5F), 0, bitsOf(0.5F) }; })
              == GlyphWireError::Layout);
    }

    SECTION("an outline whose contours do not partition its points")
    {
        CHECK(with([](auto& g) { g.contourEnds = { 3, 2 }; }) == GlyphWireError::Outline);
        CHECK(with([](auto& g) { g.contourEnds = { 2, 2 }; }) == GlyphWireError::Outline);
        CHECK(with([](auto& g) { g.contourEnds = { 2 }; }) == GlyphWireError::Outline); // a point left over
        CHECK(with([](auto& g) { g.contourEnds = { 2, 9 }; }) == GlyphWireError::Outline); // one past the end
        CHECK(with([](auto& g) { g.contourEnds.clear(); }) == GlyphWireError::Outline);
        CHECK(with([](auto& g) { g.points[1].onCurve = 2; }) == GlyphWireError::Outline);
        CHECK(with([](auto& g) { g.points[0].x = std::numeric_limits<int32_t>::min(); })
              == GlyphWireError::Outline);
        CHECK(with([](auto& g) { g.points[0].y = (1 << 28) + 1; }) == GlyphWireError::Outline);
    }

    SECTION("more points than a registration may decode to")
    {
        auto const refused = with([](auto& g) {
            g.points.assign(vtbackend::MaxGlyphOutlinePoints + 1, proto::WireGlyphPoint { .onCurve = 1 });
            g.contourEnds = { static_cast<uint16_t>(vtbackend::MaxGlyphOutlinePoints) };
        });
        CHECK(refused == GlyphWireError::Outline);
    }

    SECTION("a glyph without contours is a blank one, as a record without contours is")
    {
        CHECK_FALSE(with([](auto& g) {
                        g.points.clear();
                        g.contourEnds.clear();
                    }).has_value());
    }
}

TEST_CASE("GlyphWire: a refused set applies nothing", "[vthost][glyph]")
{
    auto glyphs = sampleWire();
    glyphs.push_back(proto::WireRegisteredGlyph { .slot = 2000 });
    CHECK(stageGlyphUpdates(glyphs) == std::unexpected { GlyphWireError::Slot });
}
