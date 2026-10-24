// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include <vthost/CommandRunner.hpp>

#ifndef _WIN32

TEST_CASE("the native runner captures standard output and the exit code", "[vthost][service][command]")
{
    auto const runner = vthost::makeNativeCommandRunner();

    auto const printed = runner->run(
        std::vector<std::string> { "sh", "-c", "printf 'a=1\\nb=2\\n'; echo ignored >&2; exit 3" });
    REQUIRE(printed.has_value());
    CHECK(printed->exitCode == 3);
    CHECK(printed->standardOutput == "a=1\nb=2\n");
}

TEST_CASE("the native runner reports a program that is not installed", "[vthost][service][command]")
{
    auto const runner = vthost::makeNativeCommandRunner();
    auto const result = runner->run(std::vector<std::string> { "contour-no-such-program-here" });
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == std::errc::no_such_file_or_directory);
}

TEST_CASE("the native runner reports a command killed by a signal as 128 plus the signal",
          "[vthost][service][command]")
{
    auto const runner = vthost::makeNativeCommandRunner();
    auto const result = runner->run(std::vector<std::string> { "sh", "-c", "kill -TERM $$" });
    REQUIRE(result.has_value());
    CHECK(result->exitCode == 128 + 15);
}

TEST_CASE("the native runner refuses an empty command", "[vthost][service][command]")
{
    auto const runner = vthost::makeNativeCommandRunner();
    CHECK_FALSE(runner->run({}).has_value());
}

#endif
