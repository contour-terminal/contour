// SPDX-License-Identifier: Apache-2.0
//
// The CLI verbs' options as a command line spells them, read through the verbs' own definitions.

#include <contour/cli/ContourApp.hpp>
#include <contour/cli/DaemonVerbs.hpp>
#include <contour/cli/ShellIntegration.hpp>

#include <core/Environment.hpp>
#include <core/Utf8Path.hpp>
#include <core/Utils.hpp>
#include <core/log/LogStore.hpp>
#include <core/testing/ScopedTempDir.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <vthost/SocketPath.hpp>

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

    SECTION("stop keeps the snapshot only when asked")
    {
        auto app = contour::cli::ContourApp { core::defaultEnvironment() };
        auto argv = std::array<char const*, 4> { "contour", "daemon", "stop", "--keep-snapshot" };
        REQUIRE(app.parseParametersForTesting(static_cast<int>(argv.size()), argv.data()));
        CHECK(app.parameters().get<bool>("contour.daemon.stop"));
        CHECK(app.parameters().get<bool>("contour.daemon.stop.keep-snapshot"));
        CHECK_FALSE(app.parameters().get<bool>("contour.daemon.stop.yes"));
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

TEST_CASE("ContourApp: --label is Explicit exactly when the command line gave it", "[contour][cli][daemon]")
{
    // The option's declared default is empty, so a given `default` stays distinguishable from none.
    auto const originOf = [](std::vector<char const*> argv, std::string const& key) {
        auto app = contour::cli::ContourApp { core::defaultEnvironment() };
        REQUIRE(app.parseParametersForTesting(static_cast<int>(argv.size()), argv.data()));
        return vthost::socketLabelOf(app.parameters().get<std::string>(key)).origin;
    };

    CHECK(originOf({ "contour", "daemon", "status" }, "contour.daemon.label")
          == vthost::LabelOrigin::Default);
    CHECK(originOf({ "contour", "daemon", "--label", "x", "status" }, "contour.daemon.label")
          == vthost::LabelOrigin::Explicit);
    CHECK(originOf({ "contour", "daemon", "--label=default", "status" }, "contour.daemon.label")
          == vthost::LabelOrigin::Explicit);
    CHECK(originOf({ "contour", "daemon", "status", "--label", "x" }, "contour.daemon.status.label")
          == vthost::LabelOrigin::Explicit);
    CHECK(originOf({ "contour", "daemon", "status" }, "contour.daemon.status.label")
          == vthost::LabelOrigin::Default);
}

namespace
{

/// Points @p stream at a buffer of its own while it lives, so a test can read what was written there.
class StreamCapture
{
  public:
    explicit StreamCapture(std::ostream& stream): _stream { stream }, _saved { stream.rdbuf(_buffer.rdbuf()) }
    {
    }
    ~StreamCapture() { _stream.rdbuf(_saved); }
    StreamCapture(StreamCapture const&) = delete;
    StreamCapture& operator=(StreamCapture const&) = delete;
    StreamCapture(StreamCapture&&) = delete;
    StreamCapture& operator=(StreamCapture&&) = delete;

    /// @return Everything written so far.
    [[nodiscard]] std::string text() const { return _buffer.str(); }

  private:
    std::ostringstream _buffer;
    std::ostream& _stream;
    std::streambuf* _saved;
};

} // namespace

TEST_CASE("ContourApp: daemon --stdio refuses --listen-tcp", "[contour][cli][daemon]")
{
    // Refused before stdout is taken over: the relay would become a daemon of its own otherwise.
    auto const temp = core::testing::ScopedTempDir { "contour-stdio" };
    auto const socket = core::pathToUtf8(temp.path() / "s" / "control");
    auto app = contour::cli::ContourApp { core::defaultEnvironment() };
    auto argv = std::array<char const*, 7> { "contour", "daemon",       "--socket",   socket.c_str(),
                                             "--stdio", "--listen-tcp", "127.0.0.1:1" };

    auto const out = StreamCapture { std::cout };
    auto const err = StreamCapture { std::cerr };
    CHECK(app.run(static_cast<int>(argv.size()), argv.data()) == EXIT_FAILURE);
    CHECK(err.text().contains(
        "contour daemon: --stdio relays to a daemon; it cannot be combined with --listen-tcp"));
    CHECK(out.text().empty());
}

namespace
{

/// Runs `contour daemon --listen-tcp 127.0.0.1:1 <extra>` against a keys file that does not exist.
/// @param extra The options under test.
/// @return What the run wrote to standard error; the run must have refused to start.
std::string refusedDaemonRun(std::vector<char const*> extra)
{
    auto const temp = core::testing::ScopedTempDir { "contour-auth-opts" };
    auto const socket = core::pathToUtf8(temp.path() / "s" / "control");
    auto const keys = core::pathToUtf8(temp.path() / "authorized_keys");
    auto app = contour::cli::ContourApp { core::defaultEnvironment() };
    auto argv = std::vector<char const*> { "contour",      "daemon",      "--socket",          socket.c_str(),
                                           "--listen-tcp", "127.0.0.1:1", "--authorized-keys", keys.c_str() };
    argv.insert(argv.end(), extra.begin(), extra.end());
    auto const out = StreamCapture { std::cout };
    auto const err = StreamCapture { std::cerr };
    REQUIRE(app.run(static_cast<int>(argv.size()), argv.data()) == EXIT_FAILURE);
    return err.text();
}

} // namespace

TEST_CASE("ContourApp: --listen-tcp needs a credential", "[contour][cli][daemon]")
{
    CHECK(refusedDaemonRun({}).contains("--listen-tcp requires --token"));
}

TEST_CASE("ContourApp: --auth keys needs a key", "[contour][cli][daemon]")
{
    CHECK(refusedDaemonRun({ "--auth", "keys" }).contains("--auth keys needs at least one usable key"));
}

TEST_CASE("ContourApp: --auth keys refuses --token", "[contour][cli][daemon]")
{
    CHECK(refusedDaemonRun({ "--auth", "keys", "--token", "x" })
              .contains("--auth keys accepts authorized keys alone"));
}

TEST_CASE("ContourApp: --auth names its choices", "[contour][cli][daemon]")
{
    CHECK(refusedDaemonRun({ "--auth", "pw", "--token", "x" }).contains("any, keys"));
}

TEST_CASE("ContourApp: --auth keys is refused before any token file is read", "[contour][cli][daemon]")
{
    auto const err = refusedDaemonRun({ "--auth", "keys", "--access-file", "/nonexistent/access" });
    CHECK(err.contains("--auth keys accepts authorized keys alone"));
    CHECK_FALSE(err.contains("cannot read"));
}

namespace
{

/// Appends to @p clashes every option name @p command declares twice, by long or by short spelling, then
/// does the same for each of its sub-commands. Two options of one name are one option to the parser, whose
/// one value both meanings then read (`workspace new --label` once named both the daemon to reach and the
/// workspace to make).
/// @param command The command.
/// @param path Its name, from `contour`.
/// @param clashes Where the clashes go, as `PATH --NAME`.
void collectOptionClashes(core::cli::Command const& command,
                          std::string const& path,
                          std::vector<std::string>& clashes)
{
    for (auto const& [index, option]: core::views::enumerate(command.options))
    {
        auto const later = std::span { command.options }.subspan(static_cast<std::size_t>(index) + 1);
        auto const sameLong = [&](core::cli::Option const& other) {
            return other.name.longName == option.name.longName;
        };
        auto const sameShort = [&](core::cli::Option const& other) {
            return option.name.shortName != 0 && other.name.shortName == option.name.shortName;
        };
        if (std::ranges::any_of(later, sameLong))
            clashes.push_back(std::format("{} --{}", path, option.name.longName));
        if (std::ranges::any_of(later, sameShort))
            clashes.push_back(std::format("{} -{}", path, option.name.shortName));
    }
    for (auto const& child: command.children)
        collectOptionClashes(child, std::format("{} {}", path, child.name), clashes);
}

} // namespace

TEST_CASE("ContourApp: no command declares an option name twice", "[contour][cli]")
{
    auto const app = contour::cli::ContourApp { core::defaultEnvironment() };
    auto clashes = std::vector<std::string> {};
    collectOptionClashes(app.parameterDefinition(), "contour", clashes);
    for (auto const& clash: clashes)
        UNSCOPED_INFO(clash);
    CHECK(clashes.empty());
}
