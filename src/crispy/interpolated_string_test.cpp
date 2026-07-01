#include <crispy/interpolated_string.h>

#include <catch2/catch_test_macros.hpp>

#include <format>

TEST_CASE("interpolated_string.parse_interpolation")
{
    using crispy::parse_interpolation;

    auto const interpolation = parse_interpolation("Clock:Bold,Italic,Color=#FFFF00");
    CHECK(interpolation.name == "Clock");
    CHECK(interpolation.flags.size() == 2);
    CHECK(interpolation.flags.count("Bold"));
    CHECK(interpolation.flags.count("Italic") == 1);
    CHECK(interpolation.attributes.size() == 1);
    CHECK(interpolation.attributes.count("Color"));
    CHECK(interpolation.attributes.at("Color") == "#FFFF00");
}

TEST_CASE("interpolated_string.parse_interpolated_string")
{
    using crispy::parse_interpolated_string;

    auto const interpolated = parse_interpolated_string("< {Clock:Bold,Italic,Color=#FFFF00} | {VTType}");

    CHECK(interpolated.size() == 4);

    REQUIRE(std::holds_alternative<std::string_view>(interpolated[0]));
    REQUIRE(std::get<std::string_view>(interpolated[0]) == "< ");

    REQUIRE(std::holds_alternative<crispy::string_interpolation>(interpolated[1]));

    REQUIRE(std::holds_alternative<std::string_view>(interpolated[2]));
    REQUIRE(std::get<std::string_view>(interpolated[2]) == " | ");

    REQUIRE(std::holds_alternative<crispy::string_interpolation>(interpolated[3]));
}

TEST_CASE("interpolated_string.literal_braces_pass_through")
{
    // There is no brace escaping: doubled braces are not collapsed, so a template that contains "{{...}}"
    // is parsed as a placeholder (matching the pre-escaping behavior we restored for compatibility).
    using crispy::parse_interpolated_string;

    auto const parsed = parse_interpolated_string("{{VTType}}");
    // "{{VTType}}" -> first "{...}" run is "{VTType" (a placeholder), then a trailing literal "}".
    REQUIRE(parsed.size() == 2);
    REQUIRE(std::holds_alternative<crispy::string_interpolation>(parsed[0]));
    CHECK(std::get<crispy::string_interpolation>(parsed[0]).name == "{VTType");
    REQUIRE(std::holds_alternative<std::string_view>(parsed[1]));
    CHECK(std::get<std::string_view>(parsed[1]) == "}");
}

TEST_CASE("interpolated_string.whole_captures_exact_source_slice")
{
    // Each parsed interpolation carries its exact original "{...}" slice (braces included) so consumers can
    // echo an unrecognized placeholder verbatim. `whole` is NOT normalized: it is the literal source text.
    using crispy::parse_interpolated_string;

    SECTION("a simple placeholder")
    {
        auto const parsed = parse_interpolated_string("{VTType}");
        REQUIRE(parsed.size() == 1);
        REQUIRE(std::holds_alternative<crispy::string_interpolation>(parsed[0]));
        CHECK(std::get<crispy::string_interpolation>(parsed[0]).whole == "{VTType}");
    }

    SECTION("flags and attributes are preserved verbatim in whole, in original order")
    {
        auto const parsed = parse_interpolated_string("pre {Clock:Bold,Color=#FFFF00} post");
        REQUIRE(parsed.size() == 3);
        REQUIRE(std::holds_alternative<crispy::string_interpolation>(parsed[1]));
        // The parsed flags/attributes are order-normalized (set/map), but whole is the raw slice.
        CHECK(std::get<crispy::string_interpolation>(parsed[1]).whole == "{Clock:Bold,Color=#FFFF00}");
    }

    SECTION("an unterminated placeholder captures to the end of input")
    {
        auto const parsed = parse_interpolated_string("x {Unclosed");
        REQUIRE(parsed.size() == 2);
        REQUIRE(std::holds_alternative<crispy::string_interpolation>(parsed[1]));
        CHECK(std::get<crispy::string_interpolation>(parsed[1]).whole == "{Unclosed");
    }
}
