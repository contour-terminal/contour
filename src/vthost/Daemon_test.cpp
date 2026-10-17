// SPDX-License-Identifier: Apache-2.0

#include <vtpty/ProcessPlacement.hpp>
#include <vtpty/test/NonAsciiSample.hpp>
#include <vtpty/test/ProcessTesting.hpp>
#include <vtpty/test/WindowsCommandLineSplit.hpp>

#include <core/Utf8Path.hpp>
#include <core/Utils.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/IoResult.hpp>
#include <core/net/Sockets.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <vthost/Daemon.hpp>
#include <vthost/SocketPath.hpp>

#ifdef _WIN32
    #include <core/WideString.hpp>

    #include <span>

    #include <Windows.h>
    #include <shellapi.h>
#endif

namespace
{

namespace fs = std::filesystem;

/// A unique directory path under the system temp dir, removed on destruction.
/// Each test gets its own so concurrent runs cannot collide on a socket file.
///
/// The directory is deliberately NOT created here: listenUnix creates it with
/// owner-only permissions, and refuses to bind under a world-accessible parent.
/// Pre-creating it would hand it the default 0755 and fail that check.
///
/// Both components are kept terse on purpose: an AF_UNIX path must fit
/// sun_path's 108 bytes, and the system temp dir already eats much of that.
struct TempDir
{
    fs::path path = fs::temp_directory_path() / std::format("contour-d-{}", std::random_device {}());

    TempDir() = default;

    ~TempDir()
    {
        auto ec = std::error_code {};
        fs::remove_all(path, ec);
    }

    TempDir(TempDir const&) = delete;
    TempDir& operator=(TempDir const&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;
};

} // namespace

TEST_CASE("ensureDaemon: a TCP endpoint is a no-op", "[vthost][daemon]")
{
    // A remote daemon cannot be auto-spawned, so this short-circuits before any
    // probe: the address below is TEST-NET-1 and is deliberately unreachable.
    auto const endpoint = vthost::AttachEndpoint { vthost::TcpEndpoint {
        .host = "192.0.2.1", .port = 1, .token = {}, .caPem = {} } };
    auto placement = vtpty::NoPlacement {};
    REQUIRE(vthost::ensureDaemon(endpoint, "contour", placement, {}, std::chrono::seconds { 0 })
            == EXIT_SUCCESS);
}

// [afunix]: skips where AF_UNIX is missing, and Windows CI asserts it does not. @see the tag's
// explanation in net/Socket_test.cpp.
TEST_CASE("ensureDaemon: a listening daemon is detected, nothing is spawned", "[vthost][daemon][afunix]")
{
    // The probe is the AF_UNIX connect that must work on Windows (10 1803+) as
    // well as POSIX; a runtime SKIP documents platforms lacking AF_UNIX entirely.
    // Under a directory beyond ASCII too: core-cpp binds and connects to a path spelled in UTF-8.
    auto const directory = GENERATE(std::u8string {}, std::u8string { vtpty::testing::NonAsciiName });
    auto const tmp = TempDir {};
    auto const controlSocket = tmp.path / directory / "control";
    // listenUnix makes the socket's own directory, owner-only, but not the ones above it.
    fs::create_directories(controlSocket.parent_path().parent_path());

    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };

    // Stand in for a running daemon by binding exactly the endpoint the probe
    // looks for. No accept loop is needed: reaching the listen backlog is the
    // whole liveness signal ensureDaemon asks for.
    auto listener = core::net::listenUnix(loop, core::pathToUtf8(vthost::nativeSocketPath(controlSocket)));
    if (!listener.has_value())
    {
        // Unsupported is the one acceptable failure (no AF_UNIX at all); anything
        // else is a real bind regression, so report what it actually said.
        INFO("listenUnix failed: " << listener.error().toString());
        REQUIRE(listener.error().code == core::net::NetErrorCode::Unsupported);
        SKIP("AF_UNIX not supported on this platform");
    }

    // The binary name is bogus on purpose: taking the spawn path at all is the
    // failure this guards against, and a zero timeout leaves no room to recover.
    auto const endpoint = vthost::AttachEndpoint { vthost::UnixEndpoint { .socketPath = controlSocket } };
    auto placement = vtpty::NoPlacement {};
    REQUIRE(
        vthost::ensureDaemon(endpoint, "contour-no-such-binary", placement, {}, std::chrono::seconds { 0 })
        == EXIT_SUCCESS);
}

#ifndef _WIN32
TEST_CASE("ensureDaemon: the daemon it spawns is placed as a session is", "[vthost][daemon]")
{
    // The daemon holds the scrollback of every session it hosts. Spawned into the client's own systemd
    // unit -- or into the scope of the tab `contour client` was typed into -- an out-of-memory kill of
    // anything there would end it too.
    auto const tmp = TempDir {};
    auto const endpoint =
        vthost::AttachEndpoint { vthost::UnixEndpoint { .socketPath = tmp.path / "control" } };
    auto placement = vtpty::testing::ReleasingPlacement {};
    REQUIRE(
        vthost::ensureDaemon(endpoint, "contour-no-such-binary", placement, {}, std::chrono::seconds { 0 })
        == EXIT_FAILURE);
    CHECK(placement.pids().size() == 1);
}
#endif

TEST_CASE("ensureDaemon: an unreachable socket fails once the timeout elapses", "[vthost][daemon]")
{
    // Nothing is bound, so the probe must report the daemon down. The spawn that
    // follows cannot succeed (no such binary), and the zero timeout ends the poll
    // immediately — so this also pins that a failed spawn cannot report success.
    auto const tmp = TempDir {};
    auto const endpoint =
        vthost::AttachEndpoint { vthost::UnixEndpoint { .socketPath = tmp.path / "control" } };
    auto placement = vtpty::NoPlacement {};
    REQUIRE(
        vthost::ensureDaemon(endpoint, "contour-no-such-binary", placement, {}, std::chrono::seconds { 0 })
        == EXIT_FAILURE);
}

// ---------------------------------------------------------------------------
// The spawn command line, which both platforms' spawn paths share.

TEST_CASE("daemonSpawnArgs spells the daemon verb and its socket", "[vthost][spawn]")
{
    auto const args = vthost::daemonSpawnArgs(
        "/usr/bin/contour", fs::path { "/run/user/1000/mux" }, {}, vthost::DaemonLifecycle::Persistent);

    REQUIRE(args.size() >= 3);
    CHECK(args[0] == "/usr/bin/contour");
    CHECK(args[1] == "daemon");
    CHECK(args[2] == "--socket=/run/user/1000/mux");
}

TEST_CASE("daemonSpawnArgs marks an exit-when-empty daemon", "[vthost][spawn]")
{
    auto const transient =
        vthost::daemonSpawnArgs("contour", fs::path { "/tmp/s" }, {}, vthost::DaemonLifecycle::ExitWhenEmpty);
    CHECK(std::ranges::contains(transient, "--exit-with-last-session"));

    // A persistent daemon must NOT be told to end with its last session — that asymmetry is the
    // whole point of the flag.
    auto const persistent =
        vthost::daemonSpawnArgs("contour", fs::path { "/tmp/s" }, {}, vthost::DaemonLifecycle::Persistent);
    CHECK_FALSE(std::ranges::contains(persistent, "--exit-with-last-session"));
}

TEST_CASE("daemonSpawnArgs omits logging options that were not given", "[vthost][spawn]")
{
    auto const bare =
        vthost::daemonSpawnArgs("contour", fs::path { "/tmp/s" }, {}, vthost::DaemonLifecycle::Persistent);
    CHECK_FALSE(std::ranges::any_of(bare, [](auto const& a) { return a.starts_with("--log"); }));

    auto const logged =
        vthost::daemonSpawnArgs("contour",
                                fs::path { "/tmp/s" },
                                vthost::DaemonSpawnOptions { .filter = "vthost.*", .logFile = "/tmp/d.log" },
                                vthost::DaemonLifecycle::Persistent);
    CHECK(std::ranges::contains(logged, "--log=vthost.*"));
    CHECK(std::ranges::contains(logged, "--log-file=/tmp/d.log"));
}

TEST_CASE("daemonSpawnArgs forwards the client's config and profile", "[vthost][spawn]")
{
    // A spawned daemon that reads a different configuration than the client that started it
    // hosts its terminals with a different scrollback depth — and the client then renders more
    // history than the daemon can name, which a resync truncates.
    auto const bare =
        vthost::daemonSpawnArgs("contour", fs::path { "/tmp/s" }, {}, vthost::DaemonLifecycle::Persistent);
    CHECK_FALSE(std::ranges::any_of(bare, [](auto const& a) { return a.starts_with("--config"); }));
    CHECK_FALSE(std::ranges::any_of(bare, [](auto const& a) { return a.starts_with("--profile"); }));

    auto const configured = vthost::daemonSpawnArgs(
        "contour",
        fs::path { "/tmp/s" },
        vthost::DaemonSpawnOptions { .configPath = "/etc/contour.yml", .profileName = "work" },
        vthost::DaemonLifecycle::Persistent);
    CHECK(std::ranges::contains(configured, "--config=/etc/contour.yml"));
    CHECK(std::ranges::contains(configured, "--profile=work"));
}

TEST_CASE("daemonSpawnArgs forwards the client-area size policy", "[vthost][spawn]")
{
    // The policy is a DAEMON setting, and the daemon most people run is the one their client
    // spawned -- so without this the option is reachable only by starting `contour daemon` by hand,
    // and everyone else silently gets `latest` whatever they asked for.
    auto const bare =
        vthost::daemonSpawnArgs("contour", fs::path { "/tmp/s" }, {}, vthost::DaemonLifecycle::Persistent);
    // Stated even at the default, unlike the string options: absence is itself a value here, so a
    // reader of the process list should not have to know what the default was.
    CHECK(std::ranges::contains(bare, "--size-policy=latest"));

    auto const smallest = vthost::daemonSpawnArgs(
        "contour",
        fs::path { "/tmp/s" },
        vthost::DaemonSpawnOptions { .sizePolicy = vthost::ClientSizePolicy::Smallest },
        vthost::DaemonLifecycle::Persistent);
    CHECK(std::ranges::contains(smallest, "--size-policy=smallest"));

    // The spelling has to be one the daemon's own parser accepts, or the spawned daemon dies on
    // its command line -- which is exactly what a hand-written second spelling would risk.
    for (auto const& [name, policy]: vthost::ClientSizePolicyNames)
    {
        auto const args = vthost::daemonSpawnArgs("contour",
                                                  fs::path { "/tmp/s" },
                                                  vthost::DaemonSpawnOptions { .sizePolicy = policy },
                                                  vthost::DaemonLifecycle::Persistent);
        CHECK(std::ranges::contains(args, std::format("--size-policy={}", name)));
        CHECK(vthost::clientSizePolicyFrom(name) == policy);
    }
}

TEST_CASE("a daemon hosts sessions with scrollback by default", "[vthost][daemon]")
{
    // DaemonConfig used to inherit `vtbackend::Settings{}`'s LineCount(0) verbatim, so the daemon
    // retained no history at all — @see vthost::DefaultSessionHistoryLineCount for what that costs.
    auto const settings = vthost::defaultSessionSettings();
    REQUIRE(std::holds_alternative<vtbackend::LineCount>(settings.historyLimits.capacity));
    CHECK(unbox(std::get<vtbackend::LineCount>(settings.historyLimits.capacity)) > 0);

    // And DaemonConfig itself, since that is what `contour daemon` starts from.
    auto const configured = vthost::DaemonConfig {}.settings.historyLimits.capacity;
    REQUIRE(std::holds_alternative<vtbackend::LineCount>(configured));
    CHECK(std::get<vtbackend::LineCount>(configured)
          == std::get<vtbackend::LineCount>(settings.historyLimits.capacity));
}

TEST_CASE("DaemonConfig defaults to escaping the sandbox, matching the prior hardcoded behavior",
          "[vthost][daemon]")
{
    // makeShellPtyFactory used to hardcode escapeSandbox=true regardless of DaemonConfig, silently
    // ignoring a profile's `escape_sandbox: false`. A bare DaemonConfig{} (no profile resolved, e.g.
    // in tests that construct one directly) must still default to the behavior every hosted shell
    // had before this field existed.
    CHECK(vthost::DaemonConfig {}.escapeSandbox);
}

TEST_CASE("DaemonConfig defaults to an empty startup layout, matching the prior single-tab behavior",
          "[vthost][daemon]")
{
    // A bare DaemonConfig{} (no profile resolved, e.g. constructed directly in a test) must keep
    // behaving exactly as before this field existed: no configured layout, so SessionHost falls
    // back to its usual single default tab.
    CHECK(vthost::DaemonConfig {}.startupLayout.tabs.empty());
}

TEST_CASE("joinCommandLine quotes the program and the arguments that need it", "[vthost][spawn]")
{
    // The Windows spawn path used to quote only the socket VALUE, leaving argv[0] bare — which
    // breaks the default install location `C:\Program Files\contour\contour.exe`.
    auto const args = vthost::daemonSpawnArgs(R"(C:\Program Files\contour\contour.exe)",
                                              fs::path { R"(C:\Users\A B\mux)" },
                                              {},
                                              vthost::DaemonLifecycle::ExitWhenEmpty);

    auto const line = vthost::joinCommandLine(args);

    CHECK(line.starts_with(R"("C:\Program Files\contour\contour.exe" daemon )"));
    CHECK(line.contains(" --exit-with-last-session"));
    CHECK(vthost::joinCommandLine({}).empty());
}

TEST_CASE("joinCommandLine survives trailing backslashes and embedded quotes", "[vthost][spawn]")
{
    // A backslash immediately before the closing quote ESCAPES it under CommandLineToArgvW's rules,
    // so `--config=C:\Users\me\configs\` naively wrapped ends its own quoting and swallows the rest
    // of the command line into that single argument — the spawned daemon then fails to parse its
    // options (or reads the wrong config) and never binds, and `contour client` reports only a
    // timeout. The same string is reused for the Task Scheduler registration, so an installed
    // daemon would be broken persistently.
    auto const args = std::vector<std::string> {
        R"(C:\Program Files\contour\contour.exe)",
        "daemon",
        R"(--config=C:\Users\me\configs\)", // ends in ONE backslash
        R"(--log-file=D:\logs\\)",          // ends in TWO
        R"(--title=say "hi")",              // embedded quotes
        R"(--odd=trailing\"quote)",         // a backslash-quote pair mid-argument
        "--exit-with-last-session",
    };

    CHECK(vtpty::testing::splitWindowsCommandLine(vthost::joinCommandLine(args)) == args);
}

TEST_CASE("joinCommandLine round-trips the arguments a spawn actually builds", "[vthost][spawn]")
{
    auto const args = vthost::daemonSpawnArgs(R"(C:\Program Files\contour\contour.exe)",
                                              fs::path { R"(C:\Users\A B\mux\)" },
                                              vthost::DaemonSpawnOptions { .filter = "vthost.*",
                                                                           .logFile = R"(D:\logs\)",
                                                                           .configPath = R"(C:\cfg\)",
                                                                           .profileName = "work" },
                                              vthost::DaemonLifecycle::ExitWhenEmpty);
    CHECK(vtpty::testing::splitWindowsCommandLine(vthost::joinCommandLine(args)) == args);
}

#ifdef _WIN32
TEST_CASE("joinCommandLine reads back through CommandLineToArgvW as the argv it joined", "[vthost][spawn]")
{
    // CommandLineToArgvW() is what contour.exe reads its own command line with, and the task a
    // registration starts hands it the same: the reader that decides, measured rather than modelled.
    auto const args =
        vthost::daemonSpawnArgs(R"(C:\Program Files\contour\contour.exe)",
                                fs::path { R"(C:\Users\A B\mux\)" },
                                vthost::DaemonSpawnOptions { .filter = "vthost.*",
                                                             .logFile = R"(D:\logs dir\)",
                                                             .configPath = R"(C:\cfg\say "hi"\)",
                                                             .profileName = "" },
                                vthost::DaemonLifecycle::ExitWhenEmpty);
    auto const commandLine = core::toUtf16(vthost::joinCommandLine(args));
    REQUIRE(commandLine.has_value());

    auto count = 0;
    auto* const read = CommandLineToArgvW(commandLine->c_str(), &count);
    REQUIRE(read != nullptr);
    auto readBack = std::vector<std::string> {};
    for (wchar_t const* const argument: std::span { read, static_cast<std::size_t>(count) })
        readBack.push_back(core::toUtf8(argument));
    LocalFree(static_cast<HLOCAL>(read));

    CHECK(readBack == args);
}
#endif
