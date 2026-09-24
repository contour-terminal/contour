// SPDX-License-Identifier: Apache-2.0
///
/// @file
/// The canary that proves a host-driven loop REFUSES what it cannot honour.
///
/// Three refusals, each an assertion, and an assertion cannot be asserted from inside a Catch case:
/// it aborts the process, which ends the binary rather than the case. So each mode is its own
/// process, judged by the marker it prints to stderr immediately before the forbidden operation
/// (`PASS_REGULAR_EXPRESSION` in `src/core/net/CMakeLists.txt`) and by the text it prints if that
/// operation RETURNED (`FAIL_REGULAR_EXPRESSION`) -- never by an exit code alone.
///
///   run, blockOn    `EventLoop::run()` and `blockOn()` on a host-driven loop. The alternative
///                   failure is silent and remote: a consumer that calls `run()` on a `PlatformLoop`
///                   in a WebAssembly build gets a turn that waits on a backend with nothing to wait
///                   on, forever, with the page frozen and no diagnostic anywhere.
///   closedPark      `armHostWake` with a closed park pending, which it asserts cannot happen
///                   because no host-driven backend has readiness. That premise is what the
///                   assertion is FOR -- the day a host-driven backend gains readiness it must fire,
///                   or a flow parked on a descriptor that closes is never resumed -- so this mode
///                   builds exactly that backend and watches it fire.
///
/// The thread-affinity refusal that used to share this program is `LoopAffinityCanary.cpp`'s now,
/// with one mode per loop-thread-only member instead of one mode for `spawn`.
///
/// Skips (exit 77) where assertions are compiled out: with `NDEBUG` the refusal is not there to
/// observe, and `SKIP_RETURN_CODE` takes precedence over both regular expressions.

#include <core/async/DetachedTask.hpp>
#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/HostDrivenBackend.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/testing/ManualHostScheduler.hpp>
#include <core/platform/Clock.hpp>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <optional>
#include <tuple>

// Everything below is what a build WITH assertions needs, and nothing else compiles it: with
// `NDEBUG` there is no refusal to provoke, `main` skips, and a helper left visible there is an
// unused function -- which is an error in this tree, as it should be.
#ifndef NDEBUG

namespace
{

/// What this process exits with once the refusal has fired.
///
/// **An `abort()` is not a failed exit code, and ctest tells them apart.** A signal is an
/// "exception" there, which no regular expression overrides -- so an assertion left to abort on
/// its own reports as a failure however it is registered. Converting it here keeps the assertion
/// real (it still had to fire to get here) and keeps a genuine crash distinguishable: SIGSEGV is
/// not handled, so it still arrives as the exception it is.
constexpr int RefusedExitCode = 1;

/// **Load-bearing for the ctest registration, not tidiness.** `PASS_REGULAR_EXPRESSION` and
/// `FAIL_REGULAR_EXPRESSION` -- and `WILL_FAIL` before them -- are each documented as unable to
/// override a system-level failure, and a raw `SIGABRT` is one -- so without this handler the
/// assertion arrives as a signal and every regex scheme here is defeated. A future cleanup
/// deleting "unused" abort handling would convert every canary in this binary into a false pass
/// silently. It also writes nothing: `_Exit` flushes no stream, and an abort handler may call
/// only async-signal-safe functions, which is why the markers are printed by the program BEFORE
/// the forbidden operation and on stderr, which is unbuffered.
/// Turns the refusal's abort into an exit code. `_exit`-shaped on purpose: an abort handler runs
/// with the process already committed to dying, so it must not unwind, flush or allocate.
/// @param signalNumber Ignored; only SIGABRT is handled.
extern "C" void onAbort(int signalNumber)
{
    std::ignore = signalNumber;
    std::_Exit(RefusedExitCode);
}

/// A flow that never completes on its own, so `blockOn` has to reach its refusal rather than
/// finishing first.
/// @param loop The loop to park on.
/// @return A task that waits an hour.
core::async::Task<void> parkForever(core::net::EventLoop* loop)
{
    co_await loop->delay(std::chrono::hours { 1 });
}

/// A host-driven backend that ACCEPTS readiness, which no real one does: the premise
/// `EventLoop::armHostWake` asserts, broken on purpose. Everything but the readiness is the real
/// backend's.
class ReadinessHostBackend final: public core::net::IoBackend
{
  public:
    /// @param inner The real host-driven backend to forward the pump to.
    explicit ReadinessHostBackend(core::net::HostDrivenBackend& inner) noexcept: _inner(inner) {}

    [[nodiscard]] core::net::BackendKind kind() const noexcept override { return _inner.kind(); }

    [[nodiscard]] std::expected<void, core::net::NetError> attach(
        core::net::ReadinessHandler& handler) override
    {
        std::ignore = handler;
        return {};
    }

    [[nodiscard]] std::expected<void, core::net::NetError> setInterest(core::net::ReadinessHandler& handler,
                                                                       core::net::Interest interest) override
    {
        std::ignore = handler;
        std::ignore = interest;
        return {};
    }

    void detach(core::net::ReadinessHandler& handler) noexcept override { std::ignore = handler; }

    [[nodiscard]] core::net::WaitResult wait(std::optional<core::platform::SteadyDuration> timeout) override
    {
        return _inner.wait(timeout);
    }

    void wake() noexcept override { _inner.wake(); }

    [[nodiscard]] bool isHostDriven() const noexcept override { return true; }

    void armWakeAt(std::optional<core::platform::SteadyTimePoint> deadline) noexcept override
    {
        _inner.armWakeAt(deadline);
    }

    void setPump(core::net::HostCallback pump, void* state) noexcept override { _inner.setPump(pump, state); }

  private:
    core::net::HostDrivenBackend& _inner; ///< The real backend, for everything but readiness.
};

/// Parks on a handle's readability, from a flow nobody owns, inline at the call.
/// @param loop The loop to park on.
/// @param handle The handle to watch.
core::async::DetachedTask parkOnHandle(core::net::EventLoop* loop, core::platform::NativeHandle handle)
{
    co_await loop->waitReadable(handle);
}

/// A timer callback that is never meant to run.
/// @param state Ignored.
void never(void* state)
{
    std::ignore = state;
}

} // namespace

#endif

/// @param argc The argument count.
/// @param argv `run`, `blockOn` or `closedPark`, naming which refusal to provoke.
/// @return Never, in a build with assertions: the refusal aborts.
int main(int argc, char** argv)
{
#ifdef NDEBUG
    /// The exit code ctest is told to read as "this configuration could not run the case".
    constexpr auto SkipExitCode = 77;
    std::ignore = argc;
    std::ignore = argv;
    std::fputs("hostdriven-canary: SKIPPED -- assertions are compiled out in this configuration\n", stderr);
    return SkipExitCode;
#else
    if (argc != 2)
    {
        std::fputs("usage: core-cpp-hostdriven-canary <run|blockOn|closedPark>\n", stderr);
        return 2;
    }

    std::signal(SIGABRT, &onAbort);

    auto clock = core::platform::ManualClock {};
    auto host = core::net::testing::ManualHostScheduler {};
    auto backend = core::net::HostDrivenBackend { host, clock };
    auto loop = core::net::EventLoop { backend, clock };

    if (std::strcmp(argv[1], "run") == 0)
    {
        std::fputs("hostdriven-canary: run: about to enter run() on a host-driven loop\n", stderr);
        loop.run();
        std::fputs("hostdriven-canary: run() returned on a host-driven loop\n", stderr);
        return 0;
    }
    if (std::strcmp(argv[1], "blockOn") == 0)
    {
        std::fputs("hostdriven-canary: blockOn: about to block on a host-driven loop\n", stderr);
        loop.blockOn(parkForever(&loop));
        std::fputs("hostdriven-canary: blockOn() returned on a host-driven loop\n", stderr);
        return 0;
    }

    if (std::strcmp(argv[1], "closedPark") == 0)
    {
        auto readiness = ReadinessHostBackend { backend };
        auto readinessLoop = core::net::EventLoop { readiness, clock };
        // A value-initialised handle is not `InvalidHandle`; this backend never asks the OS about it.
        auto const handle = core::platform::NativeHandle {};
        parkOnHandle(&readinessLoop, handle); // off-turn: runs inline and parks
        readinessLoop.notifyHandleClosing(handle, core::net::FdWakePolicy::Resume);

        std::fputs("hostdriven-canary: closedPark: about to arm the host with a closed park pending\n",
                   stderr);
        // Filed off-turn, so `registerPark` asks `armHostWake` for the turn -- with `_closedParks`
        // holding the park closed above, which is what the assertion refuses.
        std::ignore = readinessLoop.addTimer(clock.now() + std::chrono::milliseconds { 50 }, &never, nullptr);
        std::fputs(
            "hostdriven-canary: armHostWake with a closed park pending returned on a host-driven loop\n",
            stderr);
        return 0;
    }

    std::fputs("hostdriven-canary: unknown mode\n", stderr);
    return 2;
#endif
}
