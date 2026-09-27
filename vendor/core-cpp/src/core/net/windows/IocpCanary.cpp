// SPDX-License-Identifier: Apache-2.0
///
/// @file
/// The canary that proves @c core::net::IocpBackend REFUSES the two things a completion
/// port would otherwise let you do silently.
///
/// - **`g1`**: a second thread calls `wait()` while another is dequeuing the port. IOCP
///   is designed to be drained by many threads — that is its selling point elsewhere —
///   and here it would resume one coroutine on two threads. Nothing about it fails on
///   its own: the second thread simply takes half the completions, and the defect shows
///   up as a socket whose reads sometimes run on the wrong thread.
/// - **`g4`**: a handle is associated with the port twice. The kernel refuses the
///   second association with `ERROR_INVALID_PARAMETER`, which is also what it answers
///   for a closed handle and for half a dozen ordinary mistakes, so a caller that read
///   it back would condemn a working connection. The loser then awaits completions that
///   are delivered to the winner: a hang with nothing in any log.
///
/// Both are assertions, and an assertion cannot be asserted from inside a Catch case:
/// it aborts the process, which ends the binary rather than the case. So each is its
/// own process, judged by the marker it prints to stderr immediately before the
/// forbidden call (`PASS_REGULAR_EXPRESSION`), and failed by the text it prints if the
/// call returned (`FAIL_REGULAR_EXPRESSION`) -- see `src/core/net/CMakeLists.txt`.
///
/// Skips (exit 77) where assertions are compiled out: with `NDEBUG` the refusal is not
/// there to observe, and `SKIP_RETURN_CODE` takes precedence over both regular
/// expressions, so ctest reads it as a skip.

// clang-format off
#include <winsock2.h>
#include <windows.h>
// clang-format on

#include <core/net/windows/IocpBackend.hpp>
#include <core/net/windows/WindowsLoopback.hpp>
#include <core/platform/WinsockInit.hpp>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <tuple>

// Everything below is what a build WITH assertions needs, and nothing else compiles it:
// with `NDEBUG` there is no refusal to provoke, `main` skips, and a helper left visible
// there is an unused function -- which is an error in this tree, as it should be.
#ifndef NDEBUG

    #include <array>
    #include <chrono>
    #include <optional>
    #include <thread>

namespace
{

/// What this process exits with once the refusal has fired.
///
/// **An `abort()` is not a failed exit code, and ctest tells them apart.** A signal is
/// an "exception" there, which no regular expression overrides -- so an assertion left
/// to abort on its own reports as a failure however it is registered. Converting it here keeps the assertion
/// real (it still had to fire to get here) and keeps a genuine crash distinguishable: SIGSEGV is not handled,
/// so it still arrives as the exception it is.
constexpr int RefusedExitCode = 1;

/// **Load-bearing for the ctest registration, not tidiness.** `PASS_REGULAR_EXPRESSION` and
/// `FAIL_REGULAR_EXPRESSION` -- and `WILL_FAIL` before them -- are each documented as unable to
/// override a system-level failure, and a raw `SIGABRT` is one -- so without this handler the
/// assertion arrives as a signal and every regex scheme here is defeated. A future cleanup
/// deleting "unused" abort handling would convert every canary in this binary into a false pass
/// silently. It also writes nothing: `_Exit` flushes no stream, and an abort handler may call
/// only async-signal-safe functions, which is why the markers are printed by the program BEFORE
/// the forbidden operation and on stderr, which is unbuffered.
/// Turns the refusal's abort into an exit code. `_exit`-shaped on purpose: an abort
/// handler runs with the process already committed to dying, so it must not unwind,
/// flush or allocate.
/// @param signalNumber Ignored; only SIGABRT is handled.
extern "C" void onAbort(int signalNumber)
{
    std::ignore = signalNumber;
    std::_Exit(RefusedExitCode);
}

/// Drives the G1 violation: a second thread dequeues a port another thread is inside.
/// @return 0 if the refusal did NOT fire, which is the regression.
int provokeG1()
{
    auto backend = core::net::IocpBackend {};

    // The first thread blocks for ever: nothing is registered and nothing posts, so the
    // only thing that can end its wait is the `wake()` below -- which is reached only
    // when the assertion did not fire.
    auto worker = std::jthread { [&backend] { std::ignore = backend.wait(std::nullopt); } };

    // Spun on rather than slept through. A sleep long enough to be reliable on a cold
    // two-core runner is one nobody wants in a program that runs on every build, and one
    // short enough not to notice is a canary that passes for the wrong reason
    // (`.agent/rules/testing.md`). `dequeuerRunning()` is the observation that makes the
    // question answerable instead of timed.
    while (!backend.dequeuerRunning())
        std::this_thread::yield();

    // The POSITIVE marker, and it is what the registration matches on. Printed immediately
    // before the forbidden call, so its presence proves the canary reached the mechanism --
    // which a negative alternation cannot, because a path that prints nothing at all (this
    // backend's constructor throwing) exits 1 exactly like the refusal firing.
    //
    // stderr, not stdout: `onAbort` calls `std::_Exit`, which flushes nothing, and stderr is
    // unbuffered -- the same marker on stdout would be lost on the very path it exists for.
    std::fputs("iocp-canary: g1: about to dequeue from a second thread\n", stderr);
    std::ignore = backend.wait(std::chrono::milliseconds { 0 }); // must abort

    std::fputs("iocp-canary: a second thread dequeued the port and nothing refused it\n", stderr);
    // Let the first thread out before `~jthread` joins it, or the regression reports as
    // a hang rather than as the exit 0 it is.
    backend.wake();
    return 0;
}

/// Drives the G4 violation: one handle, two associations with the same port.
/// @return 0 if the refusal did NOT fire, which is the regression.
int provokeG4()
{
    auto backend = core::net::IocpBackend {};
    auto* const port = backend.completionPort();
    if (port == nullptr)
    {
        std::fputs("iocp-canary: the IOCP backend offered no completion port\n", stderr);
        return 2;
    }

    // A SOCKET, and it has to be one: `CreateIoCompletionPort` takes only a handle
    // opened for overlapped I/O -- a file, a pipe, a mailslot or a socket. An event is
    // none of those, and a first attempt written with one SKIPPED rather than aborting,
    // which is a canary reporting that it could not ask the question as though the
    // answer had been fine.
    core::platform::ensureWinsockInitialized();
    auto sockets = std::array<SOCKET, 2> {};
    if (!core::net::makeLoopbackPair(sockets))
    {
        std::fputs("iocp-canary: SKIPPED -- no loopback socket pair on this machine\n", stderr);
        return 77;
    }
    auto* const handle = reinterpret_cast<void*>(sockets[0]);

    if (!port->associate(handle).has_value())
    {
        std::fputs("iocp-canary: could not associate a loopback socket with the port at all\n", stderr);
        closesocket(sockets[0]);
        closesocket(sockets[1]);
        return 2;
    }

    // The positive marker for this guarantee; see provokeG1 for why it is here and on stderr.
    std::fputs("iocp-canary: g4: about to associate a handle with a second port\n", stderr);
    std::ignore = port->associate(handle); // must abort

    std::fputs("iocp-canary: a handle was associated with one port twice and nothing refused it\n", stderr);
    closesocket(sockets[0]);
    closesocket(sockets[1]);
    return 0;
}

} // namespace

#endif

/// @param argc The argument count.
/// @param argv `g1` or `g4`, naming which refusal to provoke.
/// @return Never, in a build with assertions: the refusal aborts.
int main(int argc, char** argv)
{
#ifdef NDEBUG
    /// The exit code ctest is told to read as "this configuration could not run the case".
    constexpr auto SkipExitCode = 77;
    std::ignore = argc;
    std::ignore = argv;
    std::fputs("iocp-canary: SKIPPED -- assertions are compiled out in this configuration\n", stderr);
    return SkipExitCode;
#else
    if (argc != 2)
    {
        std::fputs("usage: core-cpp-iocp-canary <g1|g4>\n", stderr);
        return 2;
    }

    std::signal(SIGABRT, &onAbort);

    if (std::strcmp(argv[1], "g1") == 0)
        return provokeG1();
    if (std::strcmp(argv[1], "g4") == 0)
        return provokeG4();

    std::fputs("iocp-canary: unknown mode\n", stderr);
    return 2;
#endif
}
