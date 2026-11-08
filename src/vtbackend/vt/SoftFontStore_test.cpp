// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/vt/SoftFontStore.hpp>

#include <catch2/catch_test_macros.hpp>

#include <span>
#include <string>
#include <vector>

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
    REQUIRE(defined != nullptr);
    CHECK(defined != store.font(0)->errorGlyph);
    CHECK(defined->width == 15);

    auto const undefined = store.resolve({ .font = 0, .position = 0x22 });
    REQUIRE(undefined != nullptr);
    CHECK(undefined == store.font(0)->errorGlyph);

    auto const gone = store.resolve({ .font = 1, .position = 0x21 });
    CHECK(gone == nullptr);
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

TEST_CASE("SoftFontStore: a designator is told apart by the set size", "[SoftFontStore]")
{
    auto store = SoftFontStore {};
    auto parameters96 = forFont(2);
    parameters96.pcss = 1;
    REQUIRE(store.define(forFont(1), body('B', '@')).has_value());
    CHECK(store.fontForDesignator("B", SoftCharsetSize::Size94) == 1);
    CHECK_FALSE(store.fontForDesignator("B", SoftCharsetSize::Size96).has_value());

    REQUIRE(store.define(parameters96, body('B', '@')).has_value());
    CHECK(store.fontForDesignator("B", SoftCharsetSize::Size94) == 1);
    CHECK(store.fontForDesignator("B", SoftCharsetSize::Size96) == 2);
    CHECK(store.fontForDesignator("B") == 2); // either size: the newest
}

TEST_CASE("SoftFontStore: redefinition is copy-on-write", "[SoftFontStore]")
{
    auto store = SoftFontStore {};
    REQUIRE(store.define(FullCell, body('B', '@')).has_value());
    auto const held = store.resolve({ .font = 0, .position = 0x21 });
    auto const heldFont = store.font(0);
    auto const heldCoverage = held->coverage;
    auto const heldId = held->id;

    REQUIRE(store.define(FullCell, body('B', 'A')).has_value());

    CHECK(held->coverage == heldCoverage);
    CHECK(held->id == heldId);
    CHECK(store.font(0) != heldFont);
    auto const redefined = store.resolve({ .font = 0, .position = 0x21 });
    CHECK(redefined->id != heldId);
}

TEST_CASE("SoftFontStore: identical glyphs share an id, different ones do not", "[SoftFontStore]")
{
    auto store = SoftFontStore {};
    REQUIRE(
        store.define(forFont(0), body('B', '@') + ";" + body('B', '@').substr(1) + ";" + std::string(15, 'A'))
            .has_value());
    REQUIRE(store.define(forFont(1), body('B', '@')).has_value());

    auto const id = [&](uint8_t font, uint8_t position) {
        return store.resolve({ .font = font, .position = position })->id;
    };
    CHECK(id(0, 0x21) == id(0, 0x22));
    CHECK(id(0, 0x21) == id(1, 0x21)); // across fonts, too: it is the atlas key
    CHECK(id(0, 0x21) != id(0, 0x23));
}

TEST_CASE("SoftFontStore: an out-of-range ref resolves as FontGone", "[SoftFontStore]")
{
    auto store = SoftFontStore {};
    REQUIRE(store.define(FullCell, body('B', '@')).has_value());
    CHECK(store.resolve({ .font = 3, .position = 0x21 }) == nullptr);
    CHECK(store.resolve({ .font = 0, .position = 0x10 }) == nullptr);
    CHECK(store.resolve({ .font = 0, .position = 0x80 }) == nullptr);
    CHECK(store.resolve({ .font = 0, .position = 0x20 }) == store.font(0)->errorGlyph);
    CHECK(store.resolve({ .font = 0, .position = 0x7F }) == store.font(0)->errorGlyph);
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
    store.replaceAll(SoftFontSet { .fonts = source.fonts(), .loadRanks = source.loadRanks() });
    CHECK(store.generation() > before);
    CHECK(store.font(1) == source.font(1));
    CHECK(store.fontForDesignator("B") == 1);
}

TEST_CASE("SoftFontStore: capture then stage rebuilds the same fonts in the same load order",
          "[SoftFontStore]")
{
    auto source = SoftFontStore {};
    REQUIRE(source.define(forFont(2), body('B', '@')).has_value());
    REQUIRE(source.define(forFont(0), body('B', 'A')).has_value()); // the later load, so it wins "B"

    auto records = source.capture();
    REQUIRE(records.size() == 2);
    CHECK(records[0].number == 0);
    CHECK(records[0].loadRank == 1);
    CHECK(records[1].number == 2);
    CHECK(records[1].loadRank == 0);

    auto staged = stageSoftFonts(records);
    REQUIRE(staged.has_value());
    auto mirror = SoftFontStore {};
    mirror.replaceAll(*staged);
    CHECK(mirror.fontForDesignator("B") == 0);
    for (auto const number: { uint8_t { 0 }, uint8_t { 2 } })
    {
        REQUIRE(mirror.font(number) != nullptr);
        CHECK(mirror.font(number)->declaration == source.font(number)->declaration);
        CHECK(mirror.font(number)->glyphs[1]->id == source.font(number)->glyphs[1]->id);
    }
    CHECK(mirror.font(1) == nullptr);
}

TEST_CASE("stageSoftFonts: refuses what an encoding can get wrong", "[SoftFontStore]")
{
    auto source = SoftFontStore {};
    REQUIRE(source.define(forFont(1), body('B', '@')).has_value());
    auto const good = source.capture();
    REQUIRE(good.size() == 1);

    auto const refused = [&](auto&& mutate) {
        auto records = good;
        mutate(records.front());
        return stageSoftFonts(records);
    };
    CHECK(refused([](SoftFontRecord& r) { r.number = 3; }).error() == SoftFontError::FontNumber);
    CHECK(refused([](SoftFontRecord& r) { r.loadRank = 3; }).error() == SoftFontError::LoadRank);
    CHECK(refused([](SoftFontRecord& r) { r.erase = 3; }).error() == SoftFontError::EraseControl);
    CHECK(refused([](SoftFontRecord& r) { r.sizeSource = 2; }).error() == SoftFontError::CellMatrix);
    CHECK(refused([](SoftFontRecord& r) { r.usage = 2; }).error() == SoftFontError::Usage);
    CHECK(refused([](SoftFontRecord& r) { r.charsetSize = 2; }).error() == SoftFontError::CharsetSize);
    CHECK(refused([](SoftFontRecord& r) { r.designator = "abcd"; }).error() == SoftFontError::Designator);
    CHECK(refused([](SoftFontRecord& r) { r.glyphs.front().position = 0x80; }).error()
          == SoftFontError::Glyph);

    auto twice = good;
    twice.push_back(good.front()); // the same number twice
    CHECK(stageSoftFonts(twice).error() == SoftFontError::FontNumber);
}
