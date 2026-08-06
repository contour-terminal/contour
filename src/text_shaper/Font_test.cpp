// SPDX-License-Identifier: Apache-2.0
#include <text_shaper/Font.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string_view>

using namespace std::string_view_literals;
using namespace text;

TEST_CASE("Font.makeFontWeight")
{
    SECTION("every enumerator is reachable by its own spelling")
    {
        // "extra bold" mapped to ExtraBlack until this was fixed: a config asking for extra bold
        // silently got the heaviest weight the enum has, two steps past what it named.
        CHECK(makeFontWeight("thin"sv) == FontWeight::Thin);
        CHECK(makeFontWeight("extra light"sv) == FontWeight::ExtraLight);
        CHECK(makeFontWeight("light"sv) == FontWeight::Light);
        CHECK(makeFontWeight("demilight"sv) == FontWeight::DemiLight);
        CHECK(makeFontWeight("book"sv) == FontWeight::Book);
        CHECK(makeFontWeight("normal"sv) == FontWeight::Normal);
        CHECK(makeFontWeight("medium"sv) == FontWeight::Medium);
        CHECK(makeFontWeight("demibold"sv) == FontWeight::DemiBold);
        CHECK(makeFontWeight("bold"sv) == FontWeight::Bold);
        CHECK(makeFontWeight("extra bold"sv) == FontWeight::ExtraBold);
        CHECK(makeFontWeight("black"sv) == FontWeight::Black);
        CHECK(makeFontWeight("extra black"sv) == FontWeight::ExtraBlack);
    }

    SECTION("distinct spellings map to distinct weights")
    {
        CHECK(makeFontWeight("bold"sv) != makeFontWeight("extra bold"sv));
        CHECK(makeFontWeight("extra bold"sv) != makeFontWeight("extra black"sv));
        CHECK(makeFontWeight("light"sv) != makeFontWeight("extra light"sv));
    }

    SECTION("an unknown name yields nothing rather than a default weight")
    {
        CHECK(makeFontWeight("ultrabold"sv) == std::nullopt);
        CHECK(makeFontWeight(""sv) == std::nullopt);
    }
}

TEST_CASE("Font.makeFontSlant")
{
    CHECK(makeFontSlant("normal"sv) == FontSlant::Normal);
    CHECK(makeFontSlant("italic"sv) == FontSlant::Italic);
    CHECK(makeFontSlant("oblique"sv) == FontSlant::Oblique);
    CHECK(makeFontSlant("slanted"sv) == std::nullopt);
}

TEST_CASE("Font.makeFontSpacing")
{
    CHECK(makeFontSpacing("proportional"sv) == FontSpacing::Proportional);
    CHECK(makeFontSpacing("mono"sv) == FontSpacing::Mono);
    CHECK(makeFontSpacing("monospace"sv) == std::nullopt);
}
