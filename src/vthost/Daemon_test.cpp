// SPDX-License-Identifier: Apache-2.0

#include <crispy/Utils.hpp>

#include <catch2/catch_test_macros.hpp>

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

#include <net/EventLoop.hpp>
#include <net/IoResult.hpp>
#include <net/PollEventSource.hpp>
#include <net/Sockets.hpp>
#include <vthost/Daemon.hpp>
#include <vthost/SocketPath.hpp>

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
    REQUIRE(vthost::ensureDaemon(endpoint, "contour", {}, std::chrono::seconds { 0 }) == EXIT_SUCCESS);
}

// [afunix]: skips where AF_UNIX is missing, and Windows CI asserts it does not. @see the tag's
// explanation in net/Socket_test.cpp.
TEST_CASE("ensureDaemon: a listening daemon is detected, nothing is spawned", "[vthost][daemon][afunix]")
{
    // The probe is the AF_UNIX connect that must work on Windows (10 1803+) as
    // well as POSIX; a runtime SKIP documents platforms lacking AF_UNIX entirely.
    auto const tmp = TempDir {};
    auto const controlSocket = tmp.path / "control";

    auto source = net::PollEventSource {};
    auto loop = net::EventLoop { source };

    // Stand in for a running daemon by binding exactly the endpoint the probe
    // looks for. No accept loop is needed: reaching the listen backlog is the
    // whole liveness signal ensureDaemon asks for.
    auto listener = net::listenUnix(loop, vthost::nativeSocketPath(controlSocket).string());
    if (!listener.has_value())
    {
        // Unsupported is the one acceptable failure (no AF_UNIX at all); anything
        // else is a real bind regression, so report what it actually said.
        INFO("listenUnix failed: " << listener.error().toString());
        REQUIRE(listener.error().code == net::NetErrorCode::Unsupported);
        SKIP("AF_UNIX not supported on this platform");
    }

    // The binary name is bogus on purpose: taking the spawn path at all is the
    // failure this guards against, and a zero timeout leaves no room to recover.
    auto const endpoint = vthost::AttachEndpoint { vthost::UnixEndpoint { .socketPath = controlSocket } };
    REQUIRE(vthost::ensureDaemon(endpoint, "contour-no-such-binary", {}, std::chrono::seconds { 0 })
            == EXIT_SUCCESS);
}

TEST_CASE("ensureDaemon: an unreachable socket fails once the timeout elapses", "[vthost][daemon]")
{
    // Nothing is bound, so the probe must report the daemon down. The spawn that
    // follows cannot succeed (no such binary), and the zero timeout ends the poll
    // immediately — so this also pins that a failed spawn cannot report success.
    auto const tmp = TempDir {};
    auto const endpoint =
        vthost::AttachEndpoint { vthost::UnixEndpoint { .socketPath = tmp.path / "control" } };
    REQUIRE(vthost::ensureDaemon(endpoint, "contour-no-such-binary", {}, std::chrono::seconds { 0 })
            == EXIT_FAILURE);
}

// ---------------------------------------------------------------------------
// The spawn command line, which both platforms' spawn paths share.

namespace
{

/// @param args The argv to search.
/// @param needle The exact argument to look for.
/// @return True if @p args contains @p needle.
///
/// std::ranges::contains is not in Apple's libc++ yet; find() is.
[[nodiscard]] bool containsArg(std::vector<std::string> const& args, std::string_view needle)
{
    return std::ranges::find(args, needle) != args.end();
}

} // namespace

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
    CHECK(containsArg(transient, "--exit-with-last-session"));

    // A persistent daemon must NOT be told to end with its last session — that asymmetry is the
    // whole point of the flag.
    auto const persistent =
        vthost::daemonSpawnArgs("contour", fs::path { "/tmp/s" }, {}, vthost::DaemonLifecycle::Persistent);
    CHECK_FALSE(containsArg(persistent, "--exit-with-last-session"));
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
    CHECK(containsArg(logged, "--log=vthost.*"));
    CHECK(containsArg(logged, "--log-file=/tmp/d.log"));
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
    CHECK(containsArg(configured, "--config=/etc/contour.yml"));
    CHECK(containsArg(configured, "--profile=work"));
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
    CHECK(containsArg(bare, "--size-policy=latest"));

    auto const smallest = vthost::daemonSpawnArgs(
        "contour",
        fs::path { "/tmp/s" },
        vthost::DaemonSpawnOptions { .sizePolicy = vthost::ClientSizePolicy::Smallest },
        vthost::DaemonLifecycle::Persistent);
    CHECK(containsArg(smallest, "--size-policy=smallest"));

    // The spelling has to be one the daemon's own parser accepts, or the spawned daemon dies on
    // its command line -- which is exactly what a hand-written second spelling would risk.
    for (auto const& [name, policy]: vthost::ClientSizePolicyNames)
    {
        auto const args = vthost::daemonSpawnArgs("contour",
                                                  fs::path { "/tmp/s" },
                                                  vthost::DaemonSpawnOptions { .sizePolicy = policy },
                                                  vthost::DaemonLifecycle::Persistent);
        CHECK(containsArg(args, std::format("--size-policy={}", name)));
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

TEST_CASE("joinCommandLine quotes every argument, argv[0] included", "[vthost][spawn]")
{
    // The Windows spawn path used to quote only the socket VALUE, leaving argv[0] bare — which
    // breaks the default install location `C:\Program Files\contour\contour.exe`.
    auto const args = vthost::daemonSpawnArgs(R"(C:\Program Files\contour\contour.exe)",
                                              fs::path { R"(C:\Users\A B\mux)" },
                                              {},
                                              vthost::DaemonLifecycle::ExitWhenEmpty);

    auto const line = vthost::joinCommandLine(args);

    CHECK(line.starts_with(R"("C:\Program Files\contour\contour.exe")"));
    CHECK(line.contains(R"("daemon")"));
    CHECK(line.contains(R"("--exit-with-last-session")"));
    // Every argument contributes exactly one quoted token, so the count is 2 per argument.
    CHECK(static_cast<std::size_t>(std::ranges::count(line, '"')) == args.size() * 2);
}

namespace
{

/// Undoes `CommandLineToArgvW`'s quoting rules over @p line, so a round-trip proves the encoder
/// against the parser that actually consumes it rather than against a hand-written expectation.
///
/// Deliberately a second, independent implementation of the rule: outside a quoted region a space
/// separates arguments; a backslash run is literal EXCEPT before a `"`, where it halves and an odd
/// run escapes the quote; an unescaped `"` toggles the quoted region.
/// @param line The command line to split.
/// @return The argv a Win32 process would see.
[[nodiscard]] std::vector<std::string> splitWin32CommandLine(std::string_view line)
{
    auto argv = std::vector<std::string> {};
    auto current = std::string {};
    auto quoted = false;
    auto started = false;
    auto backslashes = std::size_t { 0 };

    auto const flushBackslashes = [&](std::size_t keep) {
        current.append(keep, '\\');
        backslashes = 0;
    };

    for (auto const ch: line)
    {
        if (ch == '\\')
        {
            ++backslashes;
            started = true;
            continue;
        }
        if (ch == '"')
        {
            auto const escaped = backslashes % 2 == 1;
            flushBackslashes(backslashes / 2);
            if (escaped)
                current += '"'; // escaped by the odd backslash
            else
                quoted = !quoted;
            started = true;
            continue;
        }
        flushBackslashes(backslashes);
        if (ch == ' ' && !quoted)
        {
            if (started)
                argv.push_back(std::exchange(current, {}));
            started = false;
            continue;
        }
        current += ch;
        started = true;
    }
    flushBackslashes(backslashes);
    if (started)
        argv.push_back(current);
    return argv;
}

} // namespace

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

    CHECK(splitWin32CommandLine(vthost::joinCommandLine(args)) == args);
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
    CHECK(splitWin32CommandLine(vthost::joinCommandLine(args)) == args);
}

// ---------------------------------------------------------------------------
// argv[0] -> executable path resolution (POSIX exec has no PATH search).

namespace
{

/// A filesystem-free `isExecutable` predicate: exactly the paths named are executable.
/// @param executables The paths that exist, in the generic (forward-slash) spelling.
/// @return The predicate resolveExecutablePath takes.
[[nodiscard]] auto onlyTheseExist(std::vector<std::string> executables)
{
    return [executables = std::move(executables)](fs::path const& candidate) {
        return std::ranges::find(executables, candidate.generic_string()) != executables.end();
    };
}

/// Joins @p entries with the platform's PATH separator.
[[nodiscard]] std::string pathList(std::vector<std::string> const& entries)
{
    return crispy::joinHumanReadable(entries, std::string { vthost::PathListSeparator });
}

} // namespace

TEST_CASE("resolveExecutablePath finds a bare name on PATH", "[vthost][spawn]")
{
    // The regression this pins: `execv` does NOT search PATH, and a client started as plain
    // `contour` hands exactly `contour` to the spawn as argv[0]. Exec'ing it verbatim fails with
    // ENOENT, so auto-spawn never worked for anyone who did not spell out a path.
    auto const search = pathList({ "/opt/empty", "/usr/local/bin", "/usr/bin" });
    auto const exists = onlyTheseExist({ "/usr/local/bin/contour", "/usr/bin/contour" });

    // Left-to-right: the FIRST matching entry wins, exactly as an exec would resolve it.
    CHECK(fs::path { vthost::resolveExecutablePath("contour", search, exists) }.generic_string()
          == "/usr/local/bin/contour");
}

TEST_CASE("resolveExecutablePath leaves a path-shaped name alone", "[vthost][spawn]")
{
    // execvp's own rule, and what keeps a relative or absolute argv[0] meaning what it says instead
    // of being silently redirected to some other install on PATH.
    auto const search = pathList({ "/usr/bin" });
    auto const exists = onlyTheseExist({ "/usr/bin/contour" });

    CHECK(vthost::resolveExecutablePath("/opt/contour/contour", search, exists) == "/opt/contour/contour");
    CHECK(vthost::resolveExecutablePath("./contour", search, exists) == "./contour");
}

TEST_CASE("resolveExecutablePath falls back to the name it was given", "[vthost][spawn]")
{
    auto const nothingExists = onlyTheseExist({});
    // Nothing matched: hand back the original so the exec still reports the name the user typed
    // (ENOENT on "contour" is a far better diagnostic than ENOENT on an invented path).
    CHECK(vthost::resolveExecutablePath("contour", pathList({ "/usr/bin" }), nothingExists) == "contour");
    // No PATH at all, and an empty name, are both just "nothing to search".
    CHECK(vthost::resolveExecutablePath("contour", "", nothingExists) == "contour");
    CHECK(vthost::resolveExecutablePath("", pathList({ "/usr/bin" }), nothingExists).empty());
}

TEST_CASE("resolveExecutablePath skips empty PATH entries", "[vthost][spawn]")
{
    // POSIX reads an empty entry as the current directory. Resolving argv[0] against the cwd is
    // exactly the ambiguity this function removes, so an empty entry must not match anything.
    auto const search = std::format("{}{}{}", "", vthost::PathListSeparator, "/usr/bin");
    auto const exists = onlyTheseExist({ "contour", "/usr/bin/contour" });
    CHECK(fs::path { vthost::resolveExecutablePath("contour", search, exists) }.generic_string()
          == "/usr/bin/contour");
}
