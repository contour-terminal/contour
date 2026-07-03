// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/StatusLineBuilder.h>

#include <catch2/catch_test_macros.hpp>

#include <variant>

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
