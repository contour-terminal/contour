// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/StatusLineBuilder.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <concepts>
#include <cstddef>
#include <string>
#include <variant>
#include <vector>

using namespace vtbackend;
using namespace std::string_view_literals;

// parseStatusLineSegment() turns a template string into a sequence of typed items. Known placeholders
// become their concrete Item type; an unrecognized placeholder is echoed verbatim as literal Text (its
// exact original "{...}" slice), matching expandTabLabel()'s tab-strip handling so both surfaces treat an
// unknown placeholder identically instead of silently dropping it.

TEST_CASE("parseStatusLineSegment.knownPlaceholderBecomesTypedItem", "[statusline]")
{
    auto const segment = parseStatusLineSegment("{VTType}"sv);
    REQUIRE(segment.size() == 1);
    CHECK(std::holds_alternative<StatusLineDefinitions::VTType>(segment[0]));
}

TEST_CASE("parseStatusLineSegment.traceModeIsRecognized", "[statusline]")
{
    // {TraceMode} is a first-class status-line item (it has a type and a serializer). Regression guard: it
    // must parse to a TraceMode item, not fall through to the verbatim-echo path as an unknown placeholder.
    // The default indicator status line's `{TraceMode:Bold,Color=#FFFF00,Left= │ }` segment depends on it.
    auto const segment = parseStatusLineSegment("{TraceMode:Bold,Color=#FFFF00,Left= │ }"sv);
    REQUIRE(segment.size() == 1);
    CHECK(std::holds_alternative<StatusLineDefinitions::TraceMode>(segment[0]));
}

TEST_CASE("parseStatusLineSegment.unknownPlaceholderEchoesVerbatim", "[statusline]")
{
    SECTION("a plain unknown placeholder")
    {
        auto const segment = parseStatusLineSegment("{Bogus}"sv);
        REQUIRE(segment.size() == 1);
        REQUIRE(std::holds_alternative<StatusLineDefinitions::Text>(segment[0]));
        CHECK(std::get<StatusLineDefinitions::Text>(segment[0]).text == "{Bogus}");
    }

    SECTION("an unknown placeholder keeps its flags/attributes verbatim")
    {
        auto const segment = parseStatusLineSegment("{Bogus:Bold,Color=#FFFF00}"sv);
        REQUIRE(segment.size() == 1);
        REQUIRE(std::holds_alternative<StatusLineDefinitions::Text>(segment[0]));
        CHECK(std::get<StatusLineDefinitions::Text>(segment[0]).text == "{Bogus:Bold,Color=#FFFF00}");
    }

    SECTION("literal text, a known token and an unknown token coexist in order")
    {
        auto const segment = parseStatusLineSegment("pre {VTType} {Bogus} post"sv);
        // "pre ", {VTType}, " ", {Bogus}->verbatim, " post"
        REQUIRE(segment.size() == 5);
        CHECK(std::holds_alternative<StatusLineDefinitions::Text>(segment[0]));
        CHECK(std::get<StatusLineDefinitions::Text>(segment[0]).text == "pre ");
        CHECK(std::holds_alternative<StatusLineDefinitions::VTType>(segment[1]));
        CHECK(std::holds_alternative<StatusLineDefinitions::Text>(segment[2]));
        CHECK(std::get<StatusLineDefinitions::Text>(segment[2]).text == " ");
        REQUIRE(std::holds_alternative<StatusLineDefinitions::Text>(segment[3]));
        CHECK(std::get<StatusLineDefinitions::Text>(segment[3]).text == "{Bogus}");
        CHECK(std::holds_alternative<StatusLineDefinitions::Text>(segment[4]));
        CHECK(std::get<StatusLineDefinitions::Text>(segment[4]).text == " post");
    }
}

// serializeStatusLineSegment() is the inverse of parseStatusLineSegment(): it turns a parsed segment back
// into the template form a config file holds. The settings page relies on that pair round-tripping -- it
// parses a profile's template into editable items and serializes them back on save -- so anything the
// parser understands the serializer has to be able to write back in a form the parser still understands.
//
// The separator matters more than it looks. crispy::parse_interpolation splits a placeholder at its FIRST
// colon and splits what follows on commas, so writing "{InputMode,Bold}" where "{InputMode:Bold}" belongs
// makes "InputMode,Bold" the placeholder *name*. That name matches nothing, so the item is echoed back as
// literal text and its styling is gone -- a saved profile silently loses its status line.

namespace
{

/// The variant index of each item, i.e. the segment's shape as a comparable value. Item has no
/// operator==, and the type sequence is what these tests are actually asserting about.
[[nodiscard]] std::vector<size_t> shapeOf(StatusLineSegment const& segment)
{
    auto shape = std::vector<size_t> {};
    for (auto const& item: segment)
        shape.push_back(item.index());
    return shape;
}

/// The segment's canonical template form: parse then serialize.
[[nodiscard]] std::string canonical(std::string_view text)
{
    return serializeStatusLineSegment(parseStatusLineSegment(text));
}

} // namespace

TEST_CASE("serializeStatusLineSegment.firstAttributeIsColonSeparated", "[statusline]")
{
    // The regression this pair of asserts exists for: a comma here instead of a colon folds the whole
    // attribute list into the placeholder name and destroys the item on the next parse.
    auto const text = canonical("{InputMode:Bold,Color=#FFFF00}"sv);
    CHECK(text == "{InputMode:Bold,Color=#FFFF00}");

    // ... and it must still be an InputMode after a round trip, not literal text.
    auto const readBack = parseStatusLineSegment(text);
    REQUIRE(readBack.size() == 1);
    CHECK(std::holds_alternative<StatusLineDefinitions::InputMode>(readBack[0]));
}

TEST_CASE("serializeStatusLineSegment.roundTripsShippedDefaults", "[statusline]")
{
    // The three default indicator segments from config::IndicatorConfig. These are what a fresh install
    // shows and what the settings page parses on first open, so they are the round trip that has to hold.
    auto const defaults = std::array {
        " {InputMode:Bold,Color=#FFFF00}"
        "{TraceMode:Bold,Color=#FFFF00,Left= │ }"
        "{Tabs:ActiveColor=#FFFF00,Left= │ }"
        "{ProtectedMode:Bold,Left= │ }"
        "{SearchPrompt:Left= │ }"sv,
        "« {Title} »"sv,
        "{HistoryLineCount:Faint,Color=#c0c0c0} │ {Clock:Bold}"sv,
    };

    for (auto const& original: defaults)
    {
        auto const once = canonical(original);

        // Every item keeps its type: nothing degraded into literal Text.
        CHECK(shapeOf(parseStatusLineSegment(once)) == shapeOf(parseStatusLineSegment(original)));

        // And the canonical form is a fixed point, so repeated opens and saves of the settings page
        // cannot drift the template.
        CHECK(canonical(once) == once);
    }
}

TEST_CASE("serializeStatusLineSegment.preservesEveryCellFlag", "[statusline]")
{
    // The settings bridge once exposed four of these thirteen and silently dropped the rest on save.
    // Driving both directions off StatusLineDefinitions::CellFlagNames is what makes that impossible.
    for (auto const& [name, label, flag]: StatusLineDefinitions::CellFlagNames)
    {
        auto styles = StatusLineDefinitions::Styles {};
        styles.flags.enable(flag);

        auto const text = serializeStatusLineSegment({ StatusLineDefinitions::InputMode { styles } });
        auto const readBack = parseStatusLineSegment(text);

        REQUIRE(readBack.size() == 1);
        REQUIRE(std::holds_alternative<StatusLineDefinitions::InputMode>(readBack[0]));
        CHECK(std::get<StatusLineDefinitions::InputMode>(readBack[0]).flags.test(flag));
    }

    SECTION("all of them at once")
    {
        auto styles = StatusLineDefinitions::Styles {};
        for (auto const& [name, label, flag]: StatusLineDefinitions::CellFlagNames)
            styles.flags.enable(flag);

        auto const readBack =
            parseStatusLineSegment(serializeStatusLineSegment({ StatusLineDefinitions::Clock { styles } }));
        REQUIRE(readBack.size() == 1);
        REQUIRE(std::holds_alternative<StatusLineDefinitions::Clock>(readBack[0]));
        CHECK(std::get<StatusLineDefinitions::Clock>(readBack[0]).flags == styles.flags);
    }
}

TEST_CASE("serializeStatusLineSegment.preservesTabsAttributes", "[statusline]")
{
    // Tabs carries three fields beyond Styles. The settings bridge used to rebuild it as
    // `Tabs { styles, {}, {}, {} }`, so opening the settings page wiped the ActiveColor the shipped
    // default indicator sets.
    auto const tabs = StatusLineDefinitions::Tabs {
        StatusLineDefinitions::Styles {},
        RGBColor { 0xFF, 0xFF, 0x00 },
        RGBColor { 0x20, 0x30, 0x40 },
        std::string { " | " },
    };

    auto const readBack = parseStatusLineSegment(serializeStatusLineSegment({ tabs }));
    REQUIRE(readBack.size() == 1);
    REQUIRE(std::holds_alternative<StatusLineDefinitions::Tabs>(readBack[0]));

    auto const& got = std::get<StatusLineDefinitions::Tabs>(readBack[0]);
    CHECK(got.activeColor == tabs.activeColor);
    CHECK(got.activeBackground == tabs.activeBackground);
    CHECK(got.separator == tabs.separator);
}

TEST_CASE("parseStatusLineSegment.tabsSeparatorIsRead", "[statusline]")
{
    // serializeToVT() consumes Tabs::separator (falling back to "|"), but the parser used to hardcode
    // nullopt, so no config could ever set it.
    auto const segment = parseStatusLineSegment("{Tabs:Separator= · }"sv);
    REQUIRE(segment.size() == 1);
    REQUIRE(std::holds_alternative<StatusLineDefinitions::Tabs>(segment[0]));
    CHECK(std::get<StatusLineDefinitions::Tabs>(segment[0]).separator == " · ");
}

TEST_CASE("parseStatusLineSegment.bareTextPlaceholderIsEmptyNotThrowing", "[statusline]")
{
    // "{Text}" without a `text=` is a plausible hand-editing typo. This parses user-authored config, so
    // it must not throw std::out_of_range at the user.
    auto segment = StatusLineSegment {};
    REQUIRE_NOTHROW(segment = parseStatusLineSegment("{Text}"sv));
    REQUIRE(segment.size() == 1);
    REQUIRE(std::holds_alternative<StatusLineDefinitions::Text>(segment[0]));
    CHECK(std::get<StatusLineDefinitions::Text>(segment[0]).text.empty());
}

TEST_CASE("serializeStatusLineSegment.textKeepsBracesAndCommasVerbatim", "[statusline]")
{
    SECTION("braces are not doubled")
    {
        // A template has no brace escaping (see crispy::parse_interpolated_string), so emitting "{{" for
        // "{" would not read back as one brace -- the text would gain a brace on every save.
        auto const text = canonical("a{b"sv);
        CHECK(text == "a{b");
        CHECK(canonical(text) == text);
    }

    SECTION("a comma in styled text falls back to an unstyled literal")
    {
        // "{Text:text=a,b}" would read back as text "a" plus a stray flag "b". Keeping the content and
        // losing the styling round-trips exactly; the other way corrupts.
        auto styles = StatusLineDefinitions::Styles {};
        styles.flags.enable(CellFlag::Bold);

        auto const text = serializeStatusLineSegment({ StatusLineDefinitions::Text { styles, "a,b" } });
        CHECK(text == "a,b");

        auto const readBack = parseStatusLineSegment(text);
        REQUIRE(readBack.size() == 1);
        REQUIRE(std::holds_alternative<StatusLineDefinitions::Text>(readBack[0]));
        CHECK(std::get<StatusLineDefinitions::Text>(readBack[0]).text == "a,b");
    }

    SECTION("styled text without a comma or brace keeps its styling")
    {
        auto styles = StatusLineDefinitions::Styles {};
        styles.flags.enable(CellFlag::Bold);

        auto const readBack = parseStatusLineSegment(
            serializeStatusLineSegment({ StatusLineDefinitions::Text { styles, "plain" } }));
        REQUIRE(readBack.size() == 1);
        REQUIRE(std::holds_alternative<StatusLineDefinitions::Text>(readBack[0]));

        auto const& got = std::get<StatusLineDefinitions::Text>(readBack[0]);
        CHECK(got.text == "plain");
        CHECK(got.flags.test(CellFlag::Bold));
    }
}

TEST_CASE("serializeStatusLineSegment.everyPlaceholderNameRoundTrips", "[statusline]")
{
    // Walks the whole vocabulary from ItemTraits, so a newly added placeholder is covered here the moment
    // it is added rather than whenever someone remembers to extend this list.
    StatusLineDefinitions::forEachItemType([](auto tag) {
        using T = decltype(tag)::type;
        auto const name = std::string(StatusLineDefinitions::ItemTraits<T>::Name);

        // Text and Command need their payload attribute to be recognized at all.
        // Capture-default rather than naming `name`: the branch that uses it is discarded for Text and
        // Command, which would make an explicit capture unused for those instantiations.
        auto const templateText = [&] -> std::string {
            if constexpr (std::same_as<T, StatusLineDefinitions::Text>)
                return "{Text:text=x}";
            else if constexpr (std::same_as<T, StatusLineDefinitions::Command>)
                return "{Command:Program=true}";
            else
                return "{" + name + "}";
        }();

        auto const segment = parseStatusLineSegment(templateText);
        INFO("placeholder: " << name);
        REQUIRE(segment.size() == 1);
        REQUIRE(std::holds_alternative<T>(segment[0]));

        // And its serialized form parses back to the same type.
        auto const readBack = parseStatusLineSegment(serializeStatusLineSegment(segment));
        REQUIRE(readBack.size() == 1);
        CHECK(std::holds_alternative<T>(readBack[0]));
    });
}

TEST_CASE("serializeStatusLineSegment.preservesBothSideAdornments", "[statusline]")
{
    // Left= and Right= wrap an item's rendered value. The shipped defaults only use Left=, so Right= would
    // otherwise go untested on both sides of the round trip.
    auto styles = StatusLineDefinitions::Styles {};
    styles.textLeft = " | ";
    styles.textRight = " > ";

    auto const readBack =
        parseStatusLineSegment(serializeStatusLineSegment({ StatusLineDefinitions::Clock { styles } }));
    REQUIRE(readBack.size() == 1);
    REQUIRE(std::holds_alternative<StatusLineDefinitions::Clock>(readBack[0]));

    auto const& got = std::get<StatusLineDefinitions::Clock>(readBack[0]);
    CHECK(got.textLeft == " | ");
    CHECK(got.textRight == " > ");
}

TEST_CASE("parseStatusLineSegment.commandWithoutProgramIsNotAnItem", "[statusline]")
{
    // A {Command} that says nothing to run is not a usable item, so it takes the unknown-placeholder path
    // and is echoed verbatim rather than becoming a Command that would run an empty program.
    auto const segment = parseStatusLineSegment("{Command}"sv);
    REQUIRE(segment.size() == 1);
    REQUIRE(std::holds_alternative<StatusLineDefinitions::Text>(segment[0]));
    CHECK(std::get<StatusLineDefinitions::Text>(segment[0]).text == "{Command}");
}
