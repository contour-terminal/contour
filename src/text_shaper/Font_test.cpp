// SPDX-License-Identifier: Apache-2.0
#include <text_shaper/Font.hpp>

#include <crispy/Utils.hpp>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <format>
#include <optional>
#include <string_view>

using namespace std::string_view_literals;
using namespace text;

namespace
{

/// Dispatches to the parser belonging to @p Attribute, so the round-trip test below can be written
/// once over every font-attribute enum instead of once per enum.
template <typename Attribute>
[[nodiscard]] constexpr std::optional<Attribute> parseAttribute(std::string_view text)
{
    if constexpr (std::is_same_v<Attribute, FontWeight>)
        return makeFontWeight(text);
    else if constexpr (std::is_same_v<Attribute, FontSlant>)
        return makeFontSlant(text);
    else if constexpr (std::is_same_v<Attribute, FontSpacing>)
        return makeFontSpacing(text);
    else
        return makeRenderMode(text);
}

} // namespace

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

    SECTION("the alternate name on each enumerator is accepted too")
    {
        CHECK(makeFontWeight("regular"sv) == FontWeight::Normal);
        CHECK(makeFontWeight("ultralight"sv) == FontWeight::ExtraLight);
        CHECK(makeFontWeight("semilight"sv) == FontWeight::DemiLight);
        CHECK(makeFontWeight("semibold"sv) == FontWeight::DemiBold);
        CHECK(makeFontWeight("ultrabold"sv) == FontWeight::ExtraBold);
        CHECK(makeFontWeight("ultrablack"sv) == FontWeight::ExtraBlack);
    }

    SECTION("an unknown name yields nothing rather than a default weight")
    {
        CHECK(makeFontWeight("heavyish"sv) == std::nullopt);
        CHECK(makeFontWeight(""sv) == std::nullopt);
    }
}

TEST_CASE("Font.makeFontSlant")
{
    CHECK(makeFontSlant("normal"sv) == FontSlant::Normal);
    CHECK(makeFontSlant("italic"sv) == FontSlant::Italic);
    CHECK(makeFontSlant("oblique"sv) == FontSlant::Oblique);
    CHECK(makeFontSlant("slanted"sv) == std::nullopt);

    // `thin` is a weight, not a slant. The config reader still honours it as a deprecated
    // spelling, but the parser itself must not pretend it names one.
    CHECK(makeFontSlant("thin"sv) == std::nullopt);
}

TEST_CASE("Font.makeFontSpacing")
{
    CHECK(makeFontSpacing("proportional"sv) == FontSpacing::Proportional);
    CHECK(makeFontSpacing("mono"sv) == FontSpacing::Mono);
    CHECK(makeFontSpacing("monospace"sv) == FontSpacing::Mono);
    CHECK(makeFontSpacing("dual width"sv) == std::nullopt);
}

TEST_CASE("Font.makeRenderMode")
{
    CHECK(makeRenderMode("lcd"sv) == RenderMode::LCD);
    CHECK(makeRenderMode("light"sv) == RenderMode::Light);
    CHECK(makeRenderMode("gray"sv) == RenderMode::Gray);
    CHECK(makeRenderMode("monochrome"sv) == RenderMode::Bitmap);
    CHECK(makeRenderMode("bitmap"sv) == RenderMode::Bitmap);
    CHECK(makeRenderMode("color"sv) == RenderMode::Color);

    // A key carrying no value reads as the default rather than as an error.
    CHECK(makeRenderMode(""sv) == RenderMode::Gray);

    CHECK(makeRenderMode("subpixel"sv) == std::nullopt);
}

TEST_CASE("Font.namesMatch")
{
    SECTION("case and separators do not distinguish two names")
    {
        CHECK(namesMatch("extra bold"sv, "extra_bold"sv));
        CHECK(namesMatch("extra bold"sv, "ExtraBold"sv));
        CHECK(namesMatch("extra bold"sv, "EXTRA-BOLD"sv));
        CHECK(namesMatch("extra bold"sv, "extrabold"sv));
        CHECK(namesMatch(""sv, ""sv));
    }

    SECTION("everything else still does")
    {
        CHECK(!namesMatch("bold"sv, "extra bold"sv));
        CHECK(!namesMatch("light"sv, "lightx"sv));
        CHECK(!namesMatch(""sv, "gray"sv));
        CHECK(!namesMatch("gray"sv, ""sv));
    }
}

// Every attribute name travels a round trip: std::formatter writes it into the config that
// `contour generate config` produces, and makeFont*()/makeRenderMode() read that config back. The
// writer's table and the parser's table were written independently and agreed on nothing -- the
// writer emitted `Regular`, `ExtraBold` and `Monospace`, none of which the parser accepted, so a
// generated config silently reverted to defaults on load.
//
// The enumerators are walked with crispy::eachElement rather than listed here, so an enumerator
// added without a parser entry fails this test instead of quietly going untested.
TEMPLATE_TEST_CASE("Font: every formatted attribute name parses back",
                   "[font]",
                   text::FontWeight,
                   text::FontSlant,
                   text::FontSpacing,
                   text::RenderMode)
{
    for (auto const value: crispy::eachElement<TestType>())
    {
        INFO(std::format("{}", value));
        CHECK(parseAttribute<TestType>(std::format("{}", value)) == value);
    }
}

TEST_CASE("Font.FontDescription.toPattern")
{
    auto fd = FontDescription { .familyName = "Fira Code" };
    CHECK(fd.toPattern() == "Fira Code");

    fd.weight = FontWeight::Bold;
    CHECK(fd.toPattern() == "Fira Code Bold");

    fd.weight = FontWeight::Normal;
    fd.slant = FontSlant::Italic;
    CHECK(fd.toPattern() == "Fira Code Italic");

    // The slant used to overwrite the weight rather than follow it, so every bold-italic face
    // reported itself as merely italic -- including through OSC 50 / FontDef, where the querying
    // application takes the answer at face value.
    fd.weight = FontWeight::Bold;
    CHECK(fd.toPattern() == "Fira Code Bold Italic");
}
