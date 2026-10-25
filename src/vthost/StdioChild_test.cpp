// SPDX-License-Identifier: Apache-2.0
#include <vtpty/ProgramLookup.hpp>

#include <core/Environment.hpp>
#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>
#include <core/platform/NativeFileSystem.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

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
    [[nodiscard]] vthost::SpawnedStdioChild spawn(std::vector<std::string> const& argv)
    {
        auto spawned = spawner->spawn(loop, argv);
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
        fixture.spawner->spawn(fixture.loop, std::vector<std::string> { "contour-no-such-program" });
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
