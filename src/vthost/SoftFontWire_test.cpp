// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/vt/SoftFontStore.hpp>

#include <catch2/catch_test_macros.hpp>

#include <ranges>
#include <span>
#include <string>
#include <utility>

#include <vthost/SoftFontWire.hpp>

using namespace vthost;

namespace
{

/// A full-cell 15x12 DECDLD body whose one glyph is @p value.
std::string body(char designator, char value)
{
    return std::string(1, designator) + std::string(15, value);
}

/// Loads font @p number under the designator " @p designator" into @p store.
void load(vtbackend::SoftFontStore& store, int number, char designator, char value = '@')
{
    REQUIRE(store.define({ .pfn = number, .pcn = 1, .pt = 2 }, " " + body(designator, value)).has_value());
}

/// @return A store holding fonts 0 and 2, loaded in the order 2 then 0.
vtbackend::SoftFontStore twoFonts()
{
    auto store = vtbackend::SoftFontStore {};
    load(store, 2, 'B', '@');
    load(store, 0, 'C', '~');
    return store;
}

/// @return A one-font wire set a mirror accepts, for a test to break one field of.
proto::WireSoftFont acceptedFont()
{
    auto store = vtbackend::SoftFontStore {};
    load(store, 1, 'B');
    return softFontsToWire(store).front();
}

} // namespace

TEST_CASE("soft fonts survive the wire with their identity, geometry and load order", "[vthost][softfont]")
{
    auto const store = twoFonts();
    auto const wire = softFontsToWire(store);
    REQUIRE(wire.size() == 2);
    CHECK(wire[0].fontNumber == 0);
    CHECK(wire[1].fontNumber == 2);

    auto const staged = softFontsFromWire(wire);
    REQUIRE(staged.has_value());
    for (auto const number: { 0, 2 })
    {
        auto const& built = staged->fonts[static_cast<std::size_t>(number)];
        auto const& original = store.fonts()[static_cast<std::size_t>(number)];
        REQUIRE(built != nullptr);
        CHECK(built->designator == original->designator);
        CHECK(built->declaration.usage == original->declaration.usage);
        CHECK(built->declaration.charsetSize == original->declaration.charsetSize);
        CHECK(built->geometry.fullWidth == original->geometry.fullWidth);
        CHECK(built->geometry.fullHeight == original->geometry.fullHeight);
        for (auto const index: std::views::iota(std::size_t { 0 }, original->glyphs.size()))
        {
            REQUIRE((built->glyphs[index] != nullptr) == (original->glyphs[index] != nullptr));
            if (original->glyphs[index])
            {
                CHECK(built->glyphs[index]->id == original->glyphs[index]->id); // the content hash agrees
                CHECK(built->glyphs[index]->coverage == original->glyphs[index]->coverage);
            }
        }
    }
    CHECK(staged->fonts[1] == nullptr);
    CHECK(staged->loadRanks == store.loadRanks());
}

TEST_CASE("a mirror's most recently loaded designator is the daemon's", "[vthost][softfont]")
{
    // Fonts 2 then 1 under one designator: the later load is font 1, which index order alone would not pick.
    auto store = vtbackend::SoftFontStore {};
    load(store, 2, 'B');
    load(store, 1, 'B');
    REQUIRE(store.fontForDesignator(" B") == 1);

    auto const staged = softFontsFromWire(softFontsToWire(store));
    REQUIRE(staged.has_value());
    auto mirrored = vtbackend::SoftFontStore {};
    mirrored.replaceAll(*staged);
    CHECK(mirrored.fontForDesignator(" B") == 1);
}

TEST_CASE("a wire set that DECDLD could not have produced is refused", "[vthost][softfont]")
{
    using Error = SoftFontWireError;
    auto const refused = [](proto::WireSoftFont font, Error expected) {
        auto const staged = softFontsFromWire(std::span { &font, 1 });
        return !staged.has_value() && staged.error() == expected;
    };

    CHECK(softFontsFromWire(std::span<proto::WireSoftFont const> {}).has_value());
    auto const good = acceptedFont();
    REQUIRE(softFontsFromWire(std::span { &good, 1 }).has_value());

    auto font = good;
    font.fontNumber = 3;
    CHECK(refused(font, Error::FontNumber));
    font = good;
    font.loadRank = 3;
    CHECK(refused(font, Error::LoadRank));
    font = good;
    font.designator = "abcd";
    CHECK(refused(font, Error::Designator));
    font = good;
    font.usage = 2;
    CHECK(refused(font, Error::Enumeration));
    font = good;
    font.charsetSize = 2;
    CHECK(refused(font, Error::Enumeration));
    font = good;
    font.glyphs.front().position = 0x80;
    CHECK(refused(font, Error::Glyph));
    font = good;
    font.glyphs.push_back(font.glyphs.front()); // the same position twice
    CHECK(refused(font, Error::Glyph));
    font = good;
    font.glyphs.front().width = static_cast<uint8_t>(font.fullWidth - 1); // not the font's cell
    font.glyphs.front().bits.resize(proto::softGlyphRowBytes(font.glyphs.front().width)
                                    * font.glyphs.front().height);
    CHECK(refused(font, Error::Glyph));
    font = good;
    font.glyphs.clear();
    for (auto const& [width, height]: { std::pair<uint8_t, uint8_t> { 17, 10 }, { 10, 33 }, { 0, 0 } })
    {
        font.fullWidth = width;
        font.fullHeight = height;
        CHECK(refused(font, Error::Glyph)); // a cell outside 1..16 x 1..32, even with no glyph to judge
    }
    font = good;
    font.textWidth = static_cast<uint8_t>(font.fullWidth + 1);
    CHECK(refused(font, Error::Glyph));

    auto const twice = std::vector { good, good };
    auto const staged = softFontsFromWire(twice);
    REQUIRE_FALSE(staged.has_value());
    CHECK(staged.error() == Error::FontNumber);
}
