// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/vt/SoftFontStore.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace vtbackend;

namespace
{

/// A full-cell, 15x12 body: @p columns sixel columns of value @p value.
[[nodiscard]] std::string body(char designator, char value, size_t columns = 15)
{
    return std::string(1, designator) + std::string(columns, value);
}

constexpr auto FullCell = DecdldParameters { .pcn = 1, .pt = 2 };

[[nodiscard]] DecdldParameters forFont(int pfn, int pe = 0, int pcn = 1)
{
    return { .pfn = pfn, .pcn = pcn, .pe = pe, .pt = 2 };
}

} // namespace

TEST_CASE("SoftFontStore: define then resolve", "[SoftFontStore]")
{
    auto store = SoftFontStore {};
    REQUIRE(store.define(FullCell, body('B', '@')).has_value());

    auto const defined = store.resolve({ .font = 0, .position = 0x21 });
    CHECK(defined.lookup == SoftGlyphLookup::Defined);
    REQUIRE(defined.glyph != nullptr);
    CHECK(defined.glyph->width == 15);

    auto const undefined = store.resolve({ .font = 0, .position = 0x22 });
    CHECK(undefined.lookup == SoftGlyphLookup::Undefined);
    REQUIRE(undefined.glyph != nullptr);
    CHECK(undefined.glyph == store.font(0)->errorGlyph);

    auto const gone = store.resolve({ .font = 1, .position = 0x21 });
    CHECK(gone.lookup == SoftGlyphLookup::FontGone);
    CHECK(gone.glyph == nullptr);
}

TEST_CASE("SoftFontStore: Pe=2 erases every font", "[SoftFontStore]")
{
    auto store = SoftFontStore {};
    REQUIRE(store.define(forFont(0), body('B', '@')).has_value());
    REQUIRE(store.define(forFont(1), body('C', '@')).has_value());
    REQUIRE(store.define(forFont(2, 2), body('D', '@')).has_value());
    CHECK(store.font(0) == nullptr);
    CHECK(store.font(1) == nullptr);
    CHECK(store.font(2) != nullptr);
}

TEST_CASE("SoftFontStore: clear drops everything and bumps the generation", "[SoftFontStore]")
{
    auto store = SoftFontStore {};
    REQUIRE(store.define(FullCell, body('B', '@')).has_value());
    auto const before = store.generation();
    store.clear();
    CHECK(store.generation() > before);
    CHECK(store.font(0) == nullptr);
    CHECK_FALSE(store.fontForDesignator("B").has_value());
}

TEST_CASE("SoftFontStore: most recently loaded designator wins", "[SoftFontStore]")
{
    auto store = SoftFontStore {};
    REQUIRE(store.define(forFont(1), body('B', '@')).has_value());
    CHECK(store.fontForDesignator("B") == 1);
    REQUIRE(store.define(forFont(2), body('B', '@')).has_value());
    CHECK(store.fontForDesignator("B") == 2);
    REQUIRE(store.define(forFont(1), body('B', '@')).has_value());
    CHECK(store.fontForDesignator("B") == 1);
    CHECK_FALSE(store.fontForDesignator("C").has_value());
}

TEST_CASE("SoftFontStore: redefinition is copy-on-write", "[SoftFontStore]")
{
    auto store = SoftFontStore {};
    REQUIRE(store.define(FullCell, body('B', '@')).has_value());
    auto const held = store.resolve({ .font = 0, .position = 0x21 });
    auto const heldFont = store.font(0);
    auto const heldCoverage = held.glyph->coverage;
    auto const heldId = held.glyph->id;

    REQUIRE(store.define(FullCell, body('B', 'A')).has_value());

    CHECK(held.glyph->coverage == heldCoverage);
    CHECK(held.glyph->id == heldId);
    CHECK(store.font(0) != heldFont);
    auto const redefined = store.resolve({ .font = 0, .position = 0x21 });
    CHECK(redefined.glyph->id != heldId);
}

TEST_CASE("SoftFontStore: identical glyphs share an id, different ones do not", "[SoftFontStore]")
{
    auto store = SoftFontStore {};
    REQUIRE(
        store.define(forFont(0), body('B', '@') + ";" + body('B', '@').substr(1) + ";" + std::string(15, 'A'))
            .has_value());
    REQUIRE(store.define(forFont(1), body('B', '@')).has_value());

    auto const id = [&](uint8_t font, uint8_t position) {
        return store.resolve({ .font = font, .position = position }).glyph->id;
    };
    CHECK(id(0, 0x21) == id(0, 0x22));
    CHECK(id(0, 0x21) == id(1, 0x21)); // across fonts, too: it is the atlas key
    CHECK(id(0, 0x21) != id(0, 0x23));
}

TEST_CASE("SoftFontStore: an out-of-range ref resolves as FontGone", "[SoftFontStore]")
{
    auto store = SoftFontStore {};
    REQUIRE(store.define(FullCell, body('B', '@')).has_value());
    CHECK(store.resolve({ .font = 3, .position = 0x21 }).lookup == SoftGlyphLookup::FontGone);
    CHECK(store.resolve({ .font = 0, .position = 0x10 }).lookup == SoftGlyphLookup::FontGone);
    CHECK(store.resolve({ .font = 0, .position = 0x80 }).lookup == SoftGlyphLookup::FontGone);
    CHECK(store.resolve({ .font = 0, .position = 0x20 }).lookup == SoftGlyphLookup::Undefined);
    CHECK(store.resolve({ .font = 0, .position = 0x7F }).lookup == SoftGlyphLookup::Undefined);
}

TEST_CASE("SoftFontStore: an invalid DECDLD leaves the store unchanged", "[SoftFontStore]")
{
    auto store = SoftFontStore {};
    REQUIRE(store.define(FullCell, body('B', '@')).has_value());
    auto const generation = store.generation();
    auto const font = store.font(0);

    CHECK(store.define({ .pfn = 3 }, body('B', '@')).error() == SoftFontError::FontNumber);
    CHECK(store.define(forFont(2, 2), " ").error()
          == SoftFontError::Designator); // Pe=2 must not erase on failure
    CHECK(store.generation() == generation);
    CHECK(store.font(0) == font);
}

TEST_CASE("SoftFontStore: replaceAll installs fonts and bumps the generation", "[SoftFontStore]")
{
    auto source = SoftFontStore {};
    REQUIRE(source.define(forFont(1), body('B', '@')).has_value());

    auto store = SoftFontStore {};
    auto const before = store.generation();
    store.replaceAll(source.fonts());
    CHECK(store.generation() > before);
    CHECK(store.font(1) == source.font(1));
    CHECK(store.fontForDesignator("B") == 1);
}
