#include <crispy/interpolated_string.hpp>

#include <catch2/catch_test_macros.hpp>

#include <format>

TEST_CASE("InterpolatedString.parseInterpolation")
{
    using crispy::parseInterpolation;

    auto const interpolation = parseInterpolation("Clock:Bold,Italic,Color=#FFFF00");
    CHECK(interpolation.name == "Clock");
    CHECK(interpolation.flags.size() == 2);
    CHECK(interpolation.flags.count("Bold"));
    CHECK(interpolation.flags.count("Italic") == 1);
    CHECK(interpolation.attributes.size() == 1);
    CHECK(interpolation.attributes.count("Color"));
    CHECK(interpolation.attributes.at("Color") == "#FFFF00");
}

TEST_CASE("InterpolatedString.parseInterpolatedString")
{
    using crispy::parseInterpolatedString;

    auto const interpolated = parseInterpolatedString("< {Clock:Bold,Italic,Color=#FFFF00} | {VTType}");

    CHECK(interpolated.size() == 4);

    REQUIRE(std::holds_alternative<std::string_view>(interpolated[0]));
    REQUIRE(std::get<std::string_view>(interpolated[0]) == "< ");

    REQUIRE(std::holds_alternative<crispy::StringInterpolation>(interpolated[1]));

    REQUIRE(std::holds_alternative<std::string_view>(interpolated[2]));
    REQUIRE(std::get<std::string_view>(interpolated[2]) == " | ");

    REQUIRE(std::holds_alternative<crispy::StringInterpolation>(interpolated[3]));
}

TEST_CASE("InterpolatedString.literal_braces_pass_through")
{
    // There is no brace escaping: doubled braces are not collapsed, so a template that contains "{{...}}"
    // is parsed as a placeholder (matching the pre-escaping behavior we restored for compatibility).
    using crispy::parseInterpolatedString;

    auto const parsed = parseInterpolatedString("{{VTType}}");
    // "{{VTType}}" -> first "{...}" run is "{VTType" (a placeholder), then a trailing literal "}".
    REQUIRE(parsed.size() == 2);
    REQUIRE(std::holds_alternative<crispy::StringInterpolation>(parsed[0]));
    CHECK(std::get<crispy::StringInterpolation>(parsed[0]).name == "{VTType");
    REQUIRE(std::holds_alternative<std::string_view>(parsed[1]));
    CHECK(std::get<std::string_view>(parsed[1]) == "}");
}

TEST_CASE("InterpolatedString.whole_captures_exact_source_slice")
{
    // Each parsed interpolation carries its exact original "{...}" slice (braces included) so consumers can
    // echo an unrecognized placeholder verbatim. `whole` is NOT normalized: it is the literal source text.
    using crispy::parseInterpolatedString;

    SECTION("a simple placeholder")
    {
        auto const parsed = parseInterpolatedString("{VTType}");
        REQUIRE(parsed.size() == 1);
        REQUIRE(std::holds_alternative<crispy::StringInterpolation>(parsed[0]));
        CHECK(std::get<crispy::StringInterpolation>(parsed[0]).whole == "{VTType}");
    }

    SECTION("flags and attributes are preserved verbatim in whole, in original order")
    {
        auto const parsed = parseInterpolatedString("pre {Clock:Bold,Color=#FFFF00} post");
        REQUIRE(parsed.size() == 3);
        REQUIRE(std::holds_alternative<crispy::StringInterpolation>(parsed[1]));
        // The parsed flags/attributes are order-normalized (set/map), but whole is the raw slice.
        CHECK(std::get<crispy::StringInterpolation>(parsed[1]).whole == "{Clock:Bold,Color=#FFFF00}");
    }

    SECTION("an unterminated placeholder captures to the end of input")
    {
        auto const parsed = parseInterpolatedString("x {Unclosed");
        REQUIRE(parsed.size() == 2);
        REQUIRE(std::holds_alternative<crispy::StringInterpolation>(parsed[1]));
        CHECK(std::get<crispy::StringInterpolation>(parsed[1]).whole == "{Unclosed");
    }
}
