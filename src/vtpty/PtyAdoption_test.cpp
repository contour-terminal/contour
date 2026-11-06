// SPDX-License-Identifier: Apache-2.0
#include <vtpty/InheritedPty.hpp>
#include <vtpty/MockPty.hpp>
#include <vtpty/Process.hpp>
#include <vtpty/PtyAdoption.hpp>
#include <vtpty/test/HandleTesting.hpp>
#include <vtpty/test/HandoverSupport.hpp>
#include <vtpty/test/ProcessTesting.hpp>
#include <vtpty/test/PtyReading.hpp>

#include <core/Escape.hpp>
#include <core/Utf8Path.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
    #include <Windows.h>
#else
    #include <fcntl.h>
    #include <unistd.h>
#endif

using vtpty::AdoptError;
using vtpty::testing::isOpen;
using vtpty::testing::requireHandoverCapable;

namespace
{

constexpr auto Size = vtpty::PageSize { .lines = vtpty::LineCount(24), .columns = vtpty::ColumnCount(80) };

#ifdef _WIN32
/// @return A program that says it is ready, then answers each line it reads (test/conpty_handover_probe.cpp).
vtpty::Process::ExecInfo answeringProgram()
{
    auto probe = std::filesystem::path { VTPTY_CONPTY_HANDOVER_PROBE };
    probe.make_preferred();
    return vtpty::testing::execInfo(core::pathToUtf8(probe), { "program" });
}

/// What the tests ask the program on an adopted PTY, and what it answers: its console's size.
constexpr auto Question = std::string_view { "size\r\n" };
constexpr auto Answer = std::string_view { "size=80x24" };

/// @return A handle of this process that nothing names: a fresh event.
vtpty::CrossingHandle unnamedHandle()
{
    return vtpty::CrossingHandle::of(CreateEventW(nullptr, TRUE, FALSE, nullptr));
}

/// Closes @p handle, which unnamedHandle() made.
void closeUnnamed(vtpty::CrossingHandle handle)
{
    CloseHandle(handle.native());
}
#else
/// @return A program that says it is ready, then echoes each line it reads.
vtpty::Process::ExecInfo answeringProgram()
{
    return vtpty::testing::execInfo("/bin/sh", { "-c", "echo ready; exec cat" });
}

/// What the tests ask the program on an adopted PTY, and what it answers: the line echoed back.
constexpr auto Question = std::string_view { "handed-over\n" };
constexpr auto Answer = std::string_view { "handed-over" };

/// @return A descriptor of this process that nothing names.
vtpty::CrossingHandle unnamedHandle()
{
    return vtpty::CrossingHandle::of(::dup(STDERR_FILENO));
}

/// Closes @p handle, which unnamedHandle() made.
void closeUnnamed(vtpty::CrossingHandle handle)
{
    ::close(handle.native());
}
#endif

/// What a test started or released from a Process and has not adopted yet, let go of as the test ends,
/// however it ends.
struct Unadopted
{
    Unadopted() = default;
    Unadopted(Unadopted const&) = delete;
    Unadopted(Unadopted&&) = delete;
    Unadopted& operator=(Unadopted const&) = delete;
    Unadopted& operator=(Unadopted&&) = delete;
    ~Unadopted()
    {
        if (running != nullptr)
            running->terminate(vtpty::Process::TerminationHint::Normal);
        if (pty)
            vtpty::closeInherited(*pty);
        if (child)
            vtpty::releaseUnadopted(*child);
    }

    /// The process until it gives its child up, ended should the test stop before: on Windows its destructor
    /// waits for its child, which a program that never started (the spawn falls back to the login shell)
    /// never ends.
    vtpty::Process* running = nullptr;
    std::optional<vtpty::AdoptedChild> child; ///< The child, until a Process adopts it.
    std::optional<vtpty::InheritedPty> pty;   ///< The PTY, until adoptPty() takes it.
};

/// @return @p pty with every handle and descriptor it names none.
vtpty::InheritedPty namingNothing(vtpty::InheritedPty pty)
{
    pty.masterFd = -1;
    if (pty.stdoutFastPipe)
        pty.stdoutFastPipe = std::pair { -1, -1 };
    if (pty.conpty)
        pty.conpty = vtpty::InheritedConPty {};
    return pty;
}

} // namespace

TEST_CASE("a PTY's handover is adopted again", "[vtpty][handover]")
{
    requireHandoverCapable();
    auto original = vtpty::testing::unplacedProcess(answeringProgram(), Size);
    // What was started or released and is not adopted yet, let go of should a REQUIRE end the test before it
    // is: left as it is, the program and its pseudoconsole would outlive the test, which on Windows then
    // hangs.
    auto unadopted = Unadopted {};
    unadopted.running = &original;
    REQUIRE(original.start().has_value());
    // The program is running, past where a POSIX child flushes its terminal, and the pseudoconsole's queries
    // at its start are answered.
    REQUIRE(vtpty::testing::readUntilAnswering(original, [](std::string_view text) {
                return text.contains("ready");
            }).contains("ready"));

    // The child first: on Windows its exit watcher closes the pseudoconsole, and stops with it.
    auto const child = original.releaseChild();
    REQUIRE(child.has_value());
    unadopted.running = nullptr;
    unadopted.child = child;
    auto const inherited = original.releaseHandover();
    REQUIRE(inherited.has_value());
    unadopted.pty = *inherited;
    CHECK(original.isClosed());

    auto pty = vtpty::adoptPty(*inherited);
    REQUIRE(pty.has_value());
    unadopted.pty.reset(); // the adopted PTY's to close now
    auto adopted = vtpty::Process::adopt(*child, std::move(*pty));
    REQUIRE(adopted.has_value());
    unadopted.child.reset();

    REQUIRE((*adopted)->write(Question) == static_cast<int>(Question.size()));
    auto const output = vtpty::testing::readUntilAnswering(
        **adopted, [](std::string_view text) { return text.contains(Answer); });
    INFO("output: " << core::escape(output));
    CHECK(output.contains(Answer));
    // Closed, as its owner closes it: on Windows a Process waits for its child until its pseudoconsole
    // closes.
    (*adopted)->close();
}

TEST_CASE("a PTY no one can adopt is closed once", "[vtpty][handover]")
{
    requireHandoverCapable();
    auto pty = vtpty::createPty(Size, std::nullopt);
    REQUIRE(pty->start().has_value());
    auto const inherited = pty->releaseHandover();
    REQUIRE(inherited.has_value());
    auto const named = inherited->handles();
    REQUIRE_FALSE(named.empty());
    for (auto const handle: named)
        REQUIRE(isOpen(handle));

    vtpty::closeInherited(*inherited);
    for (auto const handle: named)
    {
        INFO("handle " << handle.value);
        CHECK_FALSE(isOpen(handle));
    }

    // Named by nothing any more: what is opened now may take the numbers just freed, and stays open.
    auto const fresh = unnamedHandle();
    REQUIRE(isOpen(fresh));
    vtpty::closeInherited(namingNothing(*inherited));
    CHECK(isOpen(fresh));
    closeUnnamed(fresh);

#ifndef _WIN32
    // The standard streams are never a PTY's, whatever a corrupt image names.
    auto streams = namingNothing(*inherited);
    streams.masterFd = STDIN_FILENO;
    streams.stdoutFastPipe = std::pair { STDOUT_FILENO, STDERR_FILENO };
    vtpty::closeInherited(streams);
    for (auto const fd: { STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO })
        CHECK(isOpen(vtpty::CrossingHandle::of(fd)));
#endif
}

TEST_CASE("a device that cannot be handed over says so", "[vtpty][handover]")
{
    auto pty = vtpty::MockPty { Size };
    REQUIRE_FALSE(pty.handover().has_value());
    CHECK(pty.handover().error() == AdoptError::Unsupported);
    auto const released = pty.releaseHandover();
    REQUIRE_FALSE(released.has_value());
    CHECK(released.error() == AdoptError::Unsupported);
}

TEST_CASE("an unadopted child is let go", "[vtpty][handover]")
{
#ifdef _WIN32
    // Its handle is the only thing of it this process holds, and closed.
    auto process = HANDLE {};
    REQUIRE(
        DuplicateHandle(
            GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &process, SYNCHRONIZE, FALSE, 0)
        != FALSE);
    auto child = vtpty::AdoptedChild {};
    child.pid = static_cast<std::int64_t>(GetCurrentProcessId());
    child.processHandle = vtpty::CrossingHandle::of(process);
    vtpty::releaseUnadopted(child);
    CHECK_FALSE(isOpen(child.processHandle));
#else
    // Nothing of it is held but its process id, which is waited for by whoever reaps it.
    auto const fresh = unnamedHandle();
    auto child = vtpty::AdoptedChild {};
    child.pid = ::getpid();
    child.processHandle = fresh;
    vtpty::releaseUnadopted(child);
    CHECK(isOpen(fresh));
    closeUnnamed(fresh);
#endif
}

#ifndef _WIN32
TEST_CASE("a POSIX PTY names none of the pseudoconsole handles a corrupt image carries", "[vtpty][handover]")
{
    // An open descriptor whose number a corrupt image gives every pseudoconsole handle: no POSIX PTY has
    // one, so it is neither named nor closed.
    auto const fresh = unnamedHandle();
    REQUIRE(isOpen(fresh));
    auto pty = vtpty::InheritedPty {};
    pty.pageSize = Size;
    pty.conpty = vtpty::InheritedConPty {
        .outputReader = fresh, .inputWriter = fresh, .signal = fresh, .reference = fresh, .consoleHost = fresh
    };
    CHECK(pty.handles().empty());
    vtpty::closeInherited(pty);
    CHECK(isOpen(fresh));
    closeUnnamed(fresh);
}
#endif
