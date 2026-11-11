// SPDX-License-Identifier: Apache-2.0
#include <vtpty/ProgramLookup.hpp>

#include <core/Environment.hpp>
#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>
#include <core/platform/NativeFileSystem.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#ifndef _WIN32
    #include <sys/types.h>

    #include <unistd.h>
#endif

#include <vthost/SpawnEnvironment.hpp>
#include <vthost/StderrTail.hpp>
#include <vthost/StdioChild.hpp>

using vthost::SpawnFailure;
using vthost::StderrTailLimit;

namespace
{

/// A loop and the system spawner, searching this process's PATH.
struct SpawnFixture
{
    std::string searchPath = vtpty::execSearchPath(core::defaultEnvironment().get("PATH"));
    core::platform::NativeFileSystem nativeFileSystem { core::platform::nativeRename() };
    std::unique_ptr<vthost::IStdioSpawner> spawner = vthost::makeSystemStdioSpawner(
        vtpty::ProgramSearch { .path = searchPath, .workingDirectory = {}, .fileSystem = nativeFileSystem });
    std::unique_ptr<core::net::IoBackend> source = core::net::makeDefaultBackend();
    core::net::EventLoop loop { *source };

    /// Spawns @p argv on the loop, which must succeed.
    [[nodiscard]] vthost::SpawnedStdioChild spawn(std::vector<std::string> const& argv,
                                                  vthost::SpawnEnvironment const& environment = {})
    {
        auto spawned = spawner->spawn(loop, argv, environment);
        INFO((spawned ? std::string {} : spawned.error().message));
        REQUIRE(spawned.has_value());
        return std::move(*spawned);
    }
};

/// Writes @p input, ends it, and reads what comes back until the child closes its stdout.
core::async::Task<std::vector<std::byte>> exchange(core::net::ISocket* socket, std::vector<std::byte> input)
{
    if (!input.empty())
    {
        auto const written = co_await socket->write(input);
        REQUIRE(written.has_value());
    }
    auto const shut = co_await socket->shutdownWrite();
    REQUIRE(shut.has_value());
    auto received = std::vector<std::byte> {};
    auto buffer = std::vector<std::byte>(4096);
    while (true)
    {
        auto const read = co_await socket->read(buffer);
        if (!read.has_value() || *read == 0)
            co_return received;
        received.insert(received.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(*read));
    }
}

[[nodiscard]] std::vector<std::byte> bytesOf(std::string_view text)
{
    auto const bytes = std::as_bytes(std::span { text });
    return { bytes.begin(), bytes.end() };
}

[[nodiscard]] std::string textOf(std::vector<std::byte> const& bytes)
{
    return std::ranges::to<std::string>(
        bytes | std::views::transform([](std::byte b) { return static_cast<char>(b); }));
}

/// Every byte value, twice.
[[nodiscard]] std::vector<std::byte> everyByteValue()
{
    auto bytes = std::vector<std::byte> {};
    for ([[maybe_unused]] auto const round: std::views::iota(0, 2))
        for (auto const value: std::views::iota(0, 256))
            bytes.push_back(static_cast<std::byte>(value));
    return bytes;
}

/// What the echo cases send. Words, not single letters: Windows' sort reads "b\na\n" as UTF-16, its bytes
/// alike enough every other one for IsTextUnicode() to take them for it, and prints two unknown characters.
constexpr auto EchoInput = std::string_view { "bravo\nalpha\n" };

/// The platform's commands for the cases both platforms run.
struct Commands
{
    std::vector<std::string> echo;     ///< Copies stdin to stdout, in some order.
    std::string echoed;                ///< What echo makes of EchoInput.
    std::vector<std::string> failing;  ///< Writes "oops" to stderr and exits 3.
    std::vector<std::string> sleeping; ///< Runs for 30 seconds.
};

[[nodiscard]] Commands platformCommands()
{
#ifdef _WIN32
    return Commands { .echo = { "cmd.exe", "/d", "/c", "sort" },
                      .echoed = "alpha\r\nbravo\r\n",
                      .failing = { "cmd.exe", "/d", "/c", "echo oops 1>&2 & exit /b 3" },
                      .sleeping = { "cmd.exe", "/d", "/c", "ping -n 30 127.0.0.1 > nul" } };
#else
    return Commands { .echo = { "cat" },
                      .echoed = "bravo\nalpha\n",
                      .failing = { "sh", "-c", "echo oops >&2; exit 3" },
                      .sleeping = { "sleep", "30" } };
#endif
}

} // namespace

TEST_CASE("a spawned child echoes its stdin", "[vthost][spawn][stdio]")
{
    auto fixture = SpawnFixture {};
    auto child = fixture.spawn(platformCommands().echo);
    auto const received = fixture.loop.blockOn(exchange(child.transport.get(), bytesOf(EchoInput)));
    CHECK(textOf(received) == platformCommands().echoed);
}

#ifndef _WIN32
TEST_CASE("a spawned child carries every byte value", "[vthost][spawn][stdio]")
{
    auto fixture = SpawnFixture {};
    auto child = fixture.spawn({ "cat" });
    auto const sent = everyByteValue();
    CHECK(fixture.loop.blockOn(exchange(child.transport.get(), sent)) == sent);
}
#else
TEST_CASE("the Windows bridge carries every byte value", "[vthost][spawn][stdio]")
{
    auto fixture = SpawnFixture {};
    auto child = fixture.spawn(
        { "powershell.exe",
          "-NoProfile",
          "-Command",
          "$i=[Console]::OpenStandardInput();$o=[Console]::OpenStandardOutput();$i.CopyTo($o)" });
    auto const sent = everyByteValue();
    CHECK(fixture.loop.blockOn(exchange(child.transport.get(), sent)) == sent);
}
#endif

TEST_CASE("stderr and the exit code are kept", "[vthost][spawn][stdio]")
{
    auto fixture = SpawnFixture {};
    auto child = fixture.spawn(platformCommands().failing);
    std::ignore = fixture.loop.blockOn(exchange(child.transport.get(), {}));
    CHECK(child.process->exitCode(std::chrono::seconds { 2 }) == 3);
    CHECK(child.process->stderrTail().contains("oops"));
}

#ifndef _WIN32
TEST_CASE("a spawned child gets the set variables on top of the parent's environment",
          "[vthost][spawn][stdio]")
{
    auto fixture = SpawnFixture {};
    auto const environment =
        vthost::SpawnEnvironment { .set = { { "CONTOUR_TEST_FOO", "bar" }, { "CONTOUR_TEST_FOO", "baz" } } };
    auto child = fixture.spawn({ "sh", "-c", R"(printf '%s|%s' "$CONTOUR_TEST_FOO" "${PATH:+has-path}")" },
                               environment);
    auto const received = fixture.loop.blockOn(exchange(child.transport.get(), {}));
    CHECK(textOf(received) == "baz|has-path");
}

TEST_CASE("stderr keeps only its tail", "[vthost][spawn][stdio]")
{
    auto fixture = SpawnFixture {};
    auto child = fixture.spawn({ "sh", "-c", "printf '%10000s' '' | tr ' ' x >&2; printf END >&2" });
    std::ignore = fixture.loop.blockOn(exchange(child.transport.get(), {}));
    REQUIRE(child.process->exitCode(std::chrono::seconds { 2 }) == 0);
    auto const tail = child.process->stderrTail();
    CHECK(tail.size() == StderrTailLimit);
    CHECK(tail.ends_with("xxxEND"));
}
#endif

TEST_CASE("a missing program is named", "[vthost][spawn][stdio]")
{
    auto fixture = SpawnFixture {};
    auto const spawned =
        fixture.spawner->spawn(fixture.loop, std::vector<std::string> { "contour-no-such-program" }, {});
    REQUIRE_FALSE(spawned.has_value());
    CHECK(spawned.error().kind == SpawnFailure::ProgramNotFound);
    CHECK(spawned.error().message.contains("contour-no-such-program"));
}

TEST_CASE("terminate reaps a running child", "[vthost][spawn][stdio]")
{
    auto fixture = SpawnFixture {};
    auto child = fixture.spawn(platformCommands().sleeping);
    CHECK_FALSE(child.process->exitCode(std::chrono::milliseconds { 0 }).has_value());
    child.process->terminate();
    CHECK(child.process->exitCode(std::chrono::milliseconds { 0 }).has_value());
    child.process->terminate(); // idempotent
}

#ifndef _WIN32
TEST_CASE("terminate ends the child's own children too", "[vthost][spawn][stdio]")
{
    // ssh with a ProxyJump runs the hop as a child of its own; it must not outlive the attempt.
    auto fixture = SpawnFixture {};
    auto const directory = std::filesystem::temp_directory_path()
                           / std::format("contour-pgroup-test-{}", static_cast<long>(::getpid()));
    std::filesystem::create_directories(directory);
    auto const pidFile = directory / "grandchild.pid";
    auto child =
        fixture.spawn({ "/bin/sh", "-c", std::format("sleep 30 & echo $! > '{}'; wait", pidFile.string()) });
    auto grandchild = pid_t { 0 };
    for ([[maybe_unused]] auto const attempt: std::views::iota(0, 200))
    {
        if (auto in = std::ifstream { pidFile }; in >> grandchild && grandchild > 0)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds { 10 });
    }
    REQUIRE(grandchild > 0);
    REQUIRE(::kill(grandchild, 0) == 0);

    child.process->terminate();

    // A killed grandchild is a zombie of init until reaped there; give that a moment.
    auto gone = false;
    for ([[maybe_unused]] auto const attempt: std::views::iota(0, 200))
    {
        gone = ::kill(grandchild, 0) != 0 && errno == ESRCH;
        if (gone)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds { 10 });
    }
    CHECK(gone);
    if (!gone)
        ::kill(grandchild, SIGKILL);
    std::filesystem::remove_all(directory);
}
#endif

#ifndef _WIN32
TEST_CASE("terminate ends a child's own child that outlived it", "[vthost][spawn][stdio]")
{
    // The leader is gone before terminate(), its child is not: the group is still swept.
    auto fixture = SpawnFixture {};
    auto const directory = std::filesystem::temp_directory_path()
                           / std::format("contour-pgroup-orphan-{}", static_cast<long>(::getpid()));
    std::filesystem::create_directories(directory);
    auto const pidFile = directory / "grandchild.pid";
    auto child = fixture.spawn(
        { "/bin/sh", "-c", std::format("sleep 30 >/dev/null 2>&1 & echo $! > '{}'", pidFile.string()) });
    auto grandchild = pid_t { 0 };
    for ([[maybe_unused]] auto const attempt: std::views::iota(0, 200))
    {
        if (auto in = std::ifstream { pidFile }; in >> grandchild && grandchild > 0)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds { 10 });
    }
    REQUIRE(grandchild > 0);
    REQUIRE(child.process->exitCode(std::chrono::milliseconds { 5000 }).has_value()); // the leader ended
    REQUIRE(::kill(grandchild, 0) == 0);

    child.process->terminate();

    auto gone = false;
    for ([[maybe_unused]] auto const attempt: std::views::iota(0, 200))
    {
        gone = ::kill(grandchild, 0) != 0 && errno == ESRCH;
        if (gone)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds { 10 });
    }
    CHECK(gone);
    if (!gone)
        ::kill(grandchild, SIGKILL);
    std::filesystem::remove_all(directory);
}
#endif

TEST_CASE("StderrTail keeps the last bytes up to its limit", "[vthost][spawn]")
{
    auto tail = vthost::StderrTail { 4 };
    tail.append("ab");
    CHECK(tail.text() == "ab");
    tail.append("cdef");
    CHECK(tail.text() == "cdef");
    tail.append("g");
    CHECK(tail.text() == "defg");
}

TEST_CASE("mergeEnvironment lets the last of two set names win and replaces the parent's value",
          "[vthost][spawn]")
{
    using vthost::EntryOrder;
    using vthost::NameComparison;
    auto const inherited = std::vector<std::string> { "PATH=/bin", "FOO=old", "HOME=/home/me" };
    auto const set = std::vector<std::pair<std::string, std::string>> { { "FOO", "one" },
                                                                        { "NEW", "x" },
                                                                        { "FOO", "two" } };
    auto const merged =
        vthost::mergeEnvironment<char>(inherited, set, NameComparison::CaseSensitive, EntryOrder::AsFound);
    CHECK(merged == std::vector<std::string> { "PATH=/bin", "HOME=/home/me", "NEW=x", "FOO=two" });
}

TEST_CASE("mergeEnvironment on Windows terms: case-blind names, sorted, per-drive names kept",
          "[vthost][spawn]")
{
    using vthost::EntryOrder;
    using vthost::NameComparison;
    auto const inherited =
        std::vector<std::wstring> { L"Path=C:\\bin", L"=C:=C:\\work", L"SystemRoot=C:\\Windows" };
    auto const set = std::vector<std::pair<std::wstring, std::wstring>> { { L"PATH", L"D:\\bin" },
                                                                          { L"askpass", L"1" },
                                                                          { L"path", L"E:\\bin" } };
    auto const merged = vthost::mergeEnvironment<wchar_t>(
        inherited, set, NameComparison::CaseInsensitive, EntryOrder::SortedByName);
    // Sorted by name without regard to case; of the two spellings of PATH only the last survives.
    CHECK(merged
          == std::vector<std::wstring> {
              L"=C:=C:\\work", L"askpass=1", L"path=E:\\bin", L"SystemRoot=C:\\Windows" });
}

TEST_CASE("an environment name must be non-empty and hold no equals sign", "[vthost][spawn]")
{
    CHECK_FALSE(vthost::invalidEnvironmentName({ .set = { { "A", "b=c" } } }).has_value());
    CHECK(vthost::invalidEnvironmentName({ .set = { { "A", "b" }, { "X=Y", "z" } } }) == "X=Y");
    CHECK(vthost::invalidEnvironmentName({ .set = { { "", "z" } } }) == "");
}

TEST_CASE("a spawn with a name holding an equals sign is refused", "[vthost][spawn][stdio]")
{
    auto fixture = SpawnFixture {};
    auto const spawned = fixture.spawner->spawn(
        fixture.loop, platformCommands().echo, vthost::SpawnEnvironment { .set = { { "A=B", "c" } } });
    REQUIRE_FALSE(spawned.has_value());
    CHECK(spawned.error().kind == SpawnFailure::SystemError);
    CHECK(spawned.error().message.contains("A=B"));
}

TEST_CASE("mergeEnvironment sorts case-blind names in upper-case order, as Windows does", "[vthost][spawn]")
{
    using vthost::EntryOrder;
    using vthost::NameComparison;
    // Upper-cased, '_' (0x5F) sorts after 'B' (0x42); lower-cased it would sort before 'b' (0x62).
    auto const inherited = std::vector<std::wstring> { L"A_B=1", L"ab=2" };
    auto const merged = vthost::mergeEnvironment<wchar_t>(
        inherited, {}, NameComparison::CaseInsensitive, EntryOrder::SortedByName);
    CHECK(merged == std::vector<std::wstring> { L"ab=2", L"A_B=1" });
}

TEST_CASE("environmentBlock ends every entry with a NUL and the block with one more", "[vthost][spawn]")
{
    auto const entries = std::vector<std::wstring> { L"A=1", L"B=2" };
    CHECK(vthost::environmentBlock<wchar_t>(entries) == std::wstring { L"A=1\0B=2\0\0", 9 });
    CHECK(vthost::environmentBlock<wchar_t>({}) == std::wstring { L"\0\0", 2 });
}
