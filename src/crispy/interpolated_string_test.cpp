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

TEST_CASE("interpolated_string.escaped_braces")
{
    using crispy::parse_interpolated_string;

    auto const concat = [](crispy::interpolated_string const& parts) {
        std::string out;
        for (auto const& p: parts)
            if (std::holds_alternative<std::string_view>(p))
                out += std::get<std::string_view>(p);
        return out;
    };

    SECTION("'{{' collapses to a literal '{'")
    {
        auto const parsed = parse_interpolated_string("a{{b");
        // No interpolation fragment should be produced for an escaped brace.
        for (auto const& p: parsed)
            CHECK_FALSE(std::holds_alternative<crispy::string_interpolation>(p));
        CHECK(concat(parsed) == "a{b");
    }

    SECTION("'}}' collapses to a literal '}'")
    {
        auto const parsed = parse_interpolated_string("a}}b");
        CHECK(concat(parsed) == "a}b");
    }

    SECTION("a doubled-brace wrapped token is literal, not an interpolation")
    {
        auto const parsed = parse_interpolated_string("task {{123}}");
        for (auto const& p: parsed)
            CHECK_FALSE(std::holds_alternative<crispy::string_interpolation>(p));
        CHECK(concat(parsed) == "task {123}");
    }

    SECTION("a real placeholder still parses alongside escaped braces")
    {
        auto const parsed = parse_interpolated_string("{{x}} {Clock}");
        // Exactly one real interpolation: {Clock}.
        int interpolations = 0;
        for (auto const& p: parsed)
            if (std::holds_alternative<crispy::string_interpolation>(p))
            {
                ++interpolations;
                CHECK(std::get<crispy::string_interpolation>(p).name == "Clock");
            }
        CHECK(interpolations == 1);
        CHECK(concat(parsed) == "{x} "); // the literal part, placeholder excluded
    }

    SECTION("a single unpaired '}' is still emitted verbatim (only DOUBLED braces are escapes)")
    {
        // Compatibility boundary: only a *doubled* brace is an escape. A lone '}' must pass through
        // unchanged exactly as the pre-escaping parser emitted it, so the brace-escaping feature does
        // not silently alter status-line templates that contain a single literal '}'.
        auto const parsed = parse_interpolated_string("a } b");
        for (auto const& p: parsed)
            CHECK_FALSE(std::holds_alternative<crispy::string_interpolation>(p));
        CHECK(concat(parsed) == "a } b");
    }

    SECTION("an odd run of closing braces collapses pairs and keeps the trailing single brace")
    {
        // "}}}" -> one escaped "}" (the leading pair) followed by a lone literal "}".
        auto const parsed = parse_interpolated_string("x}}}y");
        CHECK(concat(parsed) == "x}}y");
    }
}
