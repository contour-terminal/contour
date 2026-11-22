// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the pure kitty Unicode placeholder resolver.

#include <vtbackend/graphics/KittyPlaceholders.hpp>

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace vtbackend;
using namespace vtbackend::kitty_graphics;

namespace
{
/// A placeholder cell of image @p idLow, with the given 1-based ordinals (0 = unknown).
[[nodiscard]] PlaceholderCell placeholder(
    uint32_t idLow, uint32_t row = 0, uint32_t column = 0, uint32_t idHigh = 0, uint32_t placementId = 0)
{
    return PlaceholderCell { .presence = CellPresence::Placeholder,
                             .idLow = idLow,
                             .placementId = placementId,
                             .row = row,
                             .column = column,
                             .idHigh = idHigh };
}
} // namespace

TEST_CASE("KittyPlaceholders.diacriticOrdinal", "[kitty][placeholder]")
{
    CHECK(diacriticOrdinal(U'̅') == 1);
    CHECK(diacriticOrdinal(U'̍') == 2);
    CHECK(diacriticOrdinal(U'\U0001D244') == 297);
    CHECK(diacriticOrdinal(U'a') == 0);
    CHECK(diacriticOrdinal(PlaceholderCodepoint) == 0);
}

TEST_CASE("KittyPlaceholders.placeholderIdOf", "[kitty][placeholder]")
{
    CHECK(placeholderIdOf(Color(RGBColor(0x00, 0x01, 0x2C))) == 300);
    CHECK(placeholderIdOf(Color::Indexed(42)) == 42);
    CHECK(placeholderIdOf(Color::Bright(3)) == 11);
    CHECK(placeholderIdOf(Color::Default()) == 0);
    CHECK(placeholderIdOf(Color::Undefined()) == 0);
}

TEST_CASE("KittyPlaceholders.resolve", "[kitty][placeholder]")
{
    SECTION("explicit row and column")
    {
        auto const cells =
            std::vector { placeholder(42, 1, 1), placeholder(42, 1, 2), placeholder(42, 1, 3) };
        CHECK(resolvePlaceholderRuns(cells) == std::vector { PlaceholderRun { 0, 3, 42, 0, 0, 0 } });
    }

    SECTION("inferred columns")
    {
        auto const cells = std::vector { placeholder(42, 2, 5), placeholder(42), placeholder(42) };
        CHECK(resolvePlaceholderRuns(cells) == std::vector { PlaceholderRun { 0, 3, 42, 0, 1, 4 } });
    }

    SECTION("row change breaks run")
    {
        auto const cells = std::vector { placeholder(42, 1, 1), placeholder(42, 2, 2) };
        CHECK(resolvePlaceholderRuns(cells)
              == std::vector { PlaceholderRun { 0, 1, 42, 0, 0, 0 }, PlaceholderRun { 1, 1, 42, 0, 1, 1 } });
    }

    SECTION("id high byte")
    {
        auto const cells = std::vector { placeholder(42, 1, 1, 2), placeholder(42), placeholder(42) };
        CHECK(resolvePlaceholderRuns(cells)
              == std::vector { PlaceholderRun { 0, 3, 42u | (1u << 24), 0, 0, 0 } });
    }

    SECTION("a third diacritic past 256 names no byte, so its cell shows nothing")
    {
        // Ordinal 257 would be bit 32 of the id, shifted out into a different image's id.
        auto const cells =
            std::vector { placeholder(42, 1, 1), placeholder(42, 1, 2, 257), placeholder(42, 1, 3) };
        CHECK(resolvePlaceholderRuns(cells)
              == std::vector { PlaceholderRun { 0, 1, 42, 0, 0, 0 }, PlaceholderRun { 2, 1, 42, 0, 0, 2 } });
    }

    SECTION("placement id splits")
    {
        auto const cells = std::vector { placeholder(42, 1, 1, 0, 7), placeholder(42, 1, 2, 0, 0) };
        CHECK(resolvePlaceholderRuns(cells)
              == std::vector { PlaceholderRun { 0, 1, 42, 7, 0, 0 }, PlaceholderRun { 1, 1, 42, 0, 0, 1 } });
    }

    SECTION("non-placeholder cell ends run")
    {
        auto const cells =
            std::vector { placeholder(42), placeholder(42), PlaceholderCell {}, placeholder(42) };
        CHECK(resolvePlaceholderRuns(cells)
              == std::vector { PlaceholderRun { 0, 2, 42, 0, 0, 0 }, PlaceholderRun { 3, 1, 42, 0, 0, 0 } });
    }

    SECTION("non-placeholder cell ends a run of image id 0")
    {
        // The default foreground encodes id 0, and an absent cell reads as id 0 too: only presence
        // tells them apart, so it must end the run.
        auto const cells = std::vector { placeholder(0), PlaceholderCell {} };
        CHECK(resolvePlaceholderRuns(cells) == std::vector { PlaceholderRun { 0, 1, 0, 0, 0, 0 } });
    }

    SECTION("Claude Code row")
    {
        auto const cells = std::vector {
            placeholder(300, 3, 1), placeholder(300, 3, 2), placeholder(300, 3, 3), placeholder(300, 3, 4)
        };
        CHECK(resolvePlaceholderRuns(cells) == std::vector { PlaceholderRun { 0, 4, 300, 0, 2, 0 } });
    }

    SECTION("no placeholders")
    {
        auto const cells = std::vector { PlaceholderCell {}, PlaceholderCell {} };
        CHECK(resolvePlaceholderRuns(cells).empty());
    }
}
