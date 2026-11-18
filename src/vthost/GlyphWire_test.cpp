// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/vt/GlyphGlossary.hpp>

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include <vthost/GlyphWire.hpp>

using namespace vthost;
using vtbackend::GlyphDefinition;
using vtbackend::GlyphGlossary;
using vtbackend::GlyphStatement;

namespace
{

/// A two-layer glyph with a colour of its own, so a round trip that drops a layer or a colour is seen.
constexpr auto Badge = std::string_view {
    "fill=nonzero,fg:M4 4h16v16H4Z|fill=evenodd,#e5534bff:M17 13a4 4 0 1 0 0 8a4 4 0 1 0 0-8Z"
};

/// @return A glyph that uses every placement key, with a viewBox that is not the default one.
[[nodiscard]] GlyphDefinition fullDefinition()
{
    return vtbackend::parseGlyphDefinition(GlyphStatement { .viewBox = "-1.5,2,24,25",
                                                            .fit = "cover",
                                                            .align = "end,start",
                                                            .width = "2",
                                                            .payload = Badge })
        .value();
}

[[nodiscard]] GlyphDefinition plainDefinition()
{
    return vtbackend::parseGlyphDefinition(
               GlyphStatement { .viewBox = "0,0,24,24", .payload = "stroke=2,fg:M6 12h12" })
        .value();
}

/// @return The wire form of a glossary holding the full glyph at U+100005.
[[nodiscard]] std::vector<proto::WireRegisteredGlyph> sampleWire()
{
    auto glossary = GlyphGlossary {};
    glossary.registerGlyph(0x100005, fullDefinition());
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
    source.registerGlyph(0x100005, fullDefinition());
    source.registerGlyph(0x1003FF, plainDefinition());

    auto const staged = stageGlyphUpdates(glyphsToWire(source.changesSince(0)));
    REQUIRE(staged.has_value());
    auto mirror = GlyphGlossary {};
    applyGlyphUpdates(mirror, *staged, GlyphUpdateMode::Replace);

    for (auto const codepoint: { char32_t { 0x100005 }, char32_t { 0x1003FF } })
    {
        auto const original = source.find(codepoint);
        auto const copy = mirror.find(codepoint);
        REQUIRE(copy != nullptr);
        // The payload, and what it was read into: the layers, the viewBox, the placement.
        CHECK(copy->definition == original->definition);
        // The same content, so the same atlas tiles on either side.
        CHECK(copy->id == original->id);
    }
    CHECK(mirror.find(0x100000) == nullptr);
}

TEST_CASE("GlyphWire: a cleared slot clears, and a replace clears the rest", "[vthost][glyph]")
{
    auto source = GlyphGlossary {};
    source.registerGlyph(0x100001, plainDefinition());
    source.registerGlyph(0x100002, plainDefinition());

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

    SECTION("a placement no key spells")
    {
        CHECK(with([](auto& g) { g.fit = 3; }) == GlyphWireError::Layout);
        CHECK(with([](auto& g) { g.horizontal = 3; }) == GlyphWireError::Layout);
        CHECK(with([](auto& g) { g.vertical = 3; }) == GlyphWireError::Layout);
        CHECK(with([](auto& g) { g.span = 0; }) == GlyphWireError::Layout);
        CHECK(with([](auto& g) { g.span = 3; }) == GlyphWireError::Layout);
    }

    SECTION("a viewBox `vb=` would have refused")
    {
        CHECK(with([&](auto& g) { g.viewBox[2] = bitsOf(0); }) == GlyphWireError::Layout);
        CHECK(with([&](auto& g) { g.viewBox[3] = bitsOf(-24); }) == GlyphWireError::Layout);
        CHECK(with([&](auto& g) { g.viewBox[0] = bitsOf(1e7F); }) == GlyphWireError::Layout);
        CHECK(with([&](auto& g) { g.viewBox[1] = bitsOf(std::numeric_limits<float>::quiet_NaN()); })
              == GlyphWireError::Layout);
        CHECK(with([&](auto& g) { g.viewBox[2] = bitsOf(std::numeric_limits<float>::infinity()); })
              == GlyphWireError::Layout);
    }

    SECTION("a payload the protocol's parser refuses")
    {
        // Read again here by the parser an application's is read by: malformed, past a limit, or just
        // too long, it is refused as it would have been.
        CHECK(with([](auto& g) { g.payload = "fill=nonzero,fg:L1 1"; }) == GlyphWireError::Payload);
        CHECK(with([](auto& g) { g.payload.clear(); }) == GlyphWireError::Payload);
        CHECK(with([](auto& g) { g.payload = "fill=nonzero,fg:M0 0L999 999"; }) == GlyphWireError::Payload);
        CHECK(with([](auto& g) { g.payload = "fill=nonzero,fg:M1 1\x01"; }) == GlyphWireError::Payload);
        CHECK(with([](auto& g) {
                  g.payload =
                      "fill=nonzero,fg:M1 1" + std::string(vtbackend::MaxGlyphPayloadBytes, ' ') + "h1";
              })
              == GlyphWireError::Payload);
    }

    SECTION("the payload is judged against the viewBox it travels with")
    {
        // The same path, in a viewBox too small to hold it.
        CHECK(with([&](auto& g) { g.viewBox = { bitsOf(0), bitsOf(0), bitsOf(2), bitsOf(2) }; })
              == GlyphWireError::Payload);
    }
}

TEST_CASE("GlyphWire: a refused set applies nothing", "[vthost][glyph]")
{
    auto glyphs = sampleWire();
    glyphs.push_back(proto::WireRegisteredGlyph { .slot = 2000 });
    CHECK(stageGlyphUpdates(glyphs) == std::unexpected { GlyphWireError::Slot });
}
