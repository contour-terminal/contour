// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/ApplicationRestart.hpp>

#include <catch2/catch_test_macros.hpp>

#include <ranges>
#include <string>
#include <string_view>
#include <vector>

using contour::platform::MaxRestartCommandLine;
using contour::platform::restartCommandLine;

namespace
{
/// @return @p unit written @p count times.
[[nodiscard]] std::string repeated(std::string_view unit, size_t count)
{
    auto text = std::string {};
    text.reserve(unit.size() * count);
    for (auto const part: std::vector<std::string_view>(count, unit))
        text.append(part);
    return text;
}
} // namespace

TEST_CASE("restartCommandLine spells the arguments as Windows does", "[applicationrestart]")
{
    auto const arguments =
        std::vector<std::string> { "--config", "C:\\a b\\c.yml", "client", "--label", "work" };
    CHECK(restartCommandLine(arguments) == R"(--config "C:\a b\c.yml" client --label work)");
}

TEST_CASE("restartArguments leaves the program name out", "[applicationrestart]")
{
    SECTION("an empty command line has no arguments")
    {
        CHECK(contour::platform::restartArguments({}).empty());
    }

    SECTION("the program name alone has none either")
    {
        auto const argv = std::vector<std::string> { "contour" };
        CHECK(contour::platform::restartArguments(argv).empty());
    }

    SECTION("the rest follows")
    {
        auto const argv = std::vector<std::string> { "contour", "--config", "x.yml" };
        auto const arguments = contour::platform::restartArguments(argv);
        REQUIRE(arguments.size() == 2);
        CHECK(arguments[0] == "--config");
        CHECK(arguments[1] == "x.yml");
    }
}

TEST_CASE("restartCommandLine of no arguments is a plain launch", "[applicationrestart]")
{
    CHECK(restartCommandLine({}).empty());
}

TEST_CASE("restartCommandLine keeps a command line that fits and drops one that does not",
          "[applicationrestart]")
{
    SECTION("ASCII counts one unit per character")
    {
        auto const fits = std::vector<std::string> { std::string(MaxRestartCommandLine, 'a') };
        CHECK(restartCommandLine(fits) == fits.front());
        auto const tooLong = std::vector<std::string> { std::string(MaxRestartCommandLine + 1, 'a') };
        CHECK(restartCommandLine(tooLong).empty());
    }

    SECTION("the limit counts UTF-16 units, not UTF-8 bytes")
    {
        auto const fits = std::vector<std::string> { repeated("\xC3\xA4", MaxRestartCommandLine) };
        CHECK(fits.front().size() == 2 * MaxRestartCommandLine);
        CHECK(restartCommandLine(fits) == fits.front());
        auto const tooLong = std::vector<std::string> { repeated("\xC3\xA4", MaxRestartCommandLine + 1) };
        CHECK(restartCommandLine(tooLong).empty());
    }

    SECTION("a character past the BMP is two units")
    {
        auto const emoji = std::string { "\xF0\x9F\x98\x80" };
        auto const text = repeated(emoji, MaxRestartCommandLine / 2);
        auto const fits = std::vector<std::string> { text };
        CHECK(restartCommandLine(fits) == text);
        auto const tooLong = std::vector<std::string> { text + emoji };
        CHECK(restartCommandLine(tooLong).empty());
    }
}
