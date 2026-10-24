// SPDX-License-Identifier: Apache-2.0
//
// The CLI verbs' options as a command line spells them, read through the verbs' own definitions.

#include <contour/cli/ContourApp.hpp>
#include <contour/cli/DaemonVerbs.hpp>
#include <contour/cli/ShellIntegration.hpp>

#include <core/Environment.hpp>
#include <core/Utf8Path.hpp>
#include <core/log/LogStore.hpp>
#include <core/testing/ScopedTempDir.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

using namespace std::string_literals;

namespace
{

/// The lines the --log-file test writes, enabled by its own --log.
auto const logFileTestLog = core::log::Category("contour-test-log-file", "Lines the --log-file test writes.");

} // namespace

TEST_CASE("ContourApp: the daemon's --log-file beyond ASCII opens and is written", "[contour][cli]")
{
    // The command line is UTF-8, and so is --log-file. Read in the ANSI code page on Windows, as core-cpp
    // once read it, a directory such as 李\Jürgen 😀 is one that does not exist, and a daemon whose log does
    // not open exits.
    auto const temp = core::testing::ScopedTempDir { "contour-log-file" };
    auto const directory = temp.path() / std::u8string_view { u8"李/Jürgen \U0001F600" };
    std::filesystem::create_directories(directory);
    auto const logFile = core::pathToUtf8(directory / "daemon.log");

    {
        auto app = contour::cli::ContourApp { core::defaultEnvironment() };
        auto argv = std::array<char const*, 6> { "contour",  "daemon",       "log", "contour-test-log-file",
                                                 "log-file", logFile.c_str() };
        REQUIRE(app.parseParametersForTesting(static_cast<int>(argv.size()), argv.data()));
        auto const installed = app.installLogging("contour.daemon");
        INFO((installed ? std::string {} : installed.error()));
        REQUIRE(installed.has_value());
        logFileTestLog()("written beyond ASCII");
    }

    auto file = std::ifstream { directory / "daemon.log" };
    auto const contents = std::string { std::istreambuf_iterator<char> { file }, {} };
    CHECK(contents.contains("written beyond ASCII"));
}

namespace
{

/// Runs `contour generate integration shell <shell> to <file>`.
/// @return Its exit code.
int generateIntegration(std::string const& shell, std::filesystem::path const& file)
{
    auto app = contour::cli::ContourApp { core::defaultEnvironment() };
    auto const fileName = core::pathToUtf8(file);
    auto argv = std::array<char const*, 7> { "contour",     "generate", "integration",   "shell",
                                             shell.c_str(), "to",       fileName.c_str() };
    return app.run(static_cast<int>(argv.size()), argv.data());
}

/// @return The bytes of @p file, as they are on disk.
std::string bytesOf(std::filesystem::path const& file)
{
    auto in = std::ifstream { file, std::ios::binary };
    return std::string { std::istreambuf_iterator<char> { in }, {} };
}

} // namespace

TEST_CASE("ContourApp: generate integration writes the script byte for byte", "[contour][cli]")
{
    // A shell script with CR in it breaks on every line ($'\r': command not found), Git Bash's on Windows
    // included: what is written is the script's bytes, LF and all, on every platform.
    auto const temp = core::testing::ScopedTempDir { "contour-generate" };
    auto const file = temp.path() / "integration.bash";
    REQUIRE(generateIntegration("bash", file) == EXIT_SUCCESS);
    auto const script = contour::cli::shellIntegrationScript("bash");
    REQUIRE(script.has_value());
    CHECK(bytesOf(file) == *script);
}

TEST_CASE("ContourApp: generate integration fails where it cannot write", "[contour][cli]")
{
    auto const temp = core::testing::ScopedTempDir { "contour-generate" };
    CHECK(generateIntegration("bash", temp.path() / "no-such-directory" / "integration.bash")
          != EXIT_SUCCESS);
}

TEST_CASE("ContourApp: generate integration writes no file for a shell it has no script for",
          "[contour][cli]")
{
    // Neither for a shell that integrates natively nor for one it does not know: an empty file there would
    // be sourced, and do nothing, silently.
    auto const temp = core::testing::ScopedTempDir { "contour-generate" };
    for (auto const& shell: { "nu"s, "nosuchshell"s })
    {
        INFO("shell: " << shell);
        auto const file = temp.path() / ("integration." + shell);
        CHECK(generateIntegration(shell, file) != EXIT_SUCCESS);
        CHECK_FALSE(std::filesystem::exists(file));
    }
}

TEST_CASE("ContourApp: `daemon logs` dispatches the logs verb, not the --log option",
          "[contour][cli][daemon]")
{
    SECTION("a sub-verb")
    {
        auto app = contour::cli::ContourApp { core::defaultEnvironment() };
        auto argv =
            std::array<char const*, 5> { "contour", "daemon", "logs", "--socket", "/tmp/contour-sock" };
        REQUIRE(app.parseParametersForTesting(static_cast<int>(argv.size()), argv.data()));
        CHECK(app.parameters().get<bool>("contour.daemon.logs"));
        CHECK(app.parameters().get<std::string>("contour.daemon.log").empty());
        CHECK(app.parameters().get<std::string>("contour.daemon.logs.socket") == "/tmp/contour-sock");
        CHECK(!app.parameters().get<bool>("contour.daemon.status"));
        CHECK(!app.parameters().get<bool>("contour.daemon.stop"));
    }

    SECTION("the sub-verbs' own options")
    {
        auto app = contour::cli::ContourApp { core::defaultEnvironment() };
        auto argv = std::array<char const*, 6> { "contour", "daemon", "logs", "--follow", "--lines", "7" };
        REQUIRE(app.parseParametersForTesting(static_cast<int>(argv.size()), argv.data()));
        CHECK(app.parameters().get<bool>("contour.daemon.logs.follow"));
        CHECK(app.parameters().get<unsigned>("contour.daemon.logs.lines") == 7);
    }

    SECTION("bare `daemon` stays the daemon")
    {
        auto app = contour::cli::ContourApp { core::defaultEnvironment() };
        auto argv = std::array<char const*, 4> { "contour", "daemon", "--socket", "/tmp/contour-sock" };
        REQUIRE(app.parseParametersForTesting(static_cast<int>(argv.size()), argv.data()));
        CHECK(app.parameters().get<bool>("contour.daemon"));
        CHECK(app.parameters().get<std::string>("contour.daemon.socket") == "/tmp/contour-sock");
        for (auto const& row: contour::cli::DaemonSubVerbs)
            CHECK(!app.parameters().get<bool>(std::format("contour.daemon.{}", row.name)));
    }
}

TEST_CASE("ContourApp: `daemon --socket X status` keeps X in the daemon scope", "[contour][cli][daemon]")
{
    auto app = contour::cli::ContourApp { core::defaultEnvironment() };
    auto argv = std::array<char const*, 5> { "contour", "daemon", "--socket", "/tmp/x.sock", "status" };
    REQUIRE(app.parseParametersForTesting(static_cast<int>(argv.size()), argv.data()));
    CHECK(app.parameters().get<bool>("contour.daemon.status"));
    CHECK(app.parameters().get<std::string>("contour.daemon.socket") == "/tmp/x.sock");
    CHECK(app.parameters().get<std::string>("contour.daemon.status.socket").empty());
}
