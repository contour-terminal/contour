#include <crispy/interpolated_string.h>

#include <fmt/format.h>

#include <catch2/catch_test_macros.hpp>

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

    auto const interpolated = parse_interpolated_string(
        "< {Clock:Bold,Italic,Color=#FFFF00} | {VTType}");

    CHECK(interpolated.size() == 4);

    REQUIRE(std::holds_alternative<std::string_view>(interpolated[0]));
    REQUIRE(std::get<std::string_view>(interpolated[0]) == "< ");

    REQUIRE(std::holds_alternative<crispy::string_interpolation>(interpolated[1]));

    REQUIRE(std::holds_alternative<std::string_view>(interpolated[2]));
    REQUIRE(std::get<std::string_view>(interpolated[2]) == " | ");

    REQUIRE(std::holds_alternative<crispy::string_interpolation>(interpolated[3]));
}
