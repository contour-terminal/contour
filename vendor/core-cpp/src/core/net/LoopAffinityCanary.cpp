// SPDX-License-Identifier: Apache-2.0
///
/// @file
/// The canary that proves every loop-thread-only member of `EventLoop` REFUSES a second thread.
///
/// Twelve members assert `teardownIsSerialisedWithDispatch()` -- the destructor, the turn, and ten
/// that mutate scheduler state -- and an assertion cannot be asserted from inside a Catch case: it
/// aborts the process, which ends the binary rather than the case. So each member is its own mode
/// of this program, run as its own process: a loop is driven on a worker thread, and once a turn
/// has happened the main thread calls the member.
///
/// It is fastcached's `reactor-teardown-gate` (0708dd54) in this tree's shape. Upstream judged its
/// canary by a `cmake -P` script that kept five outcomes apart; here the registration does it,
/// with the discriminating part upstream measured as the one that matters: **the PASS expression
/// is the assertion's OWN text, naming the member**, not a marker printed before the call. A
/// process that died of something else on the way -- a segfault in a race, a failed thread spawn
/// -- prints no such text and fails, and a mode that reached the wrong member's guard names the
/// wrong member and fails too. The marker is printed as well, for the reader: it says which call
/// the process was making when it died.
///
/// Until this existed, one of the twelve had a canary (`spawn`, in `HostDrivenCanary.cpp`, which
/// this replaces), so a predicate inverted in any other member's guard would have gone unnoticed.
///
/// Skips (exit 77) where assertions are compiled out: with `NDEBUG` the refusal is not there to
/// observe, and `SKIP_RETURN_CODE` takes precedence over both regular expressions.

#include <core/async/ParkedWork.hpp>
#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>
#include <core/platform/Clock.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <print>
#include <string_view>
#include <thread>
#include <tuple>

// Everything below is what a build WITH assertions needs, and nothing else compiles it: with
// `NDEBUG` there is no refusal to provoke, `main` skips, and a helper left visible there is an
// unused function -- which is an error in this tree, as it should be.
#ifndef NDEBUG

    #include <algorithm>
    #include <array>
    #include <coroutine>
    #include <memory>

namespace
{

/// What this process exits with once the refusal has fired.
constexpr int RefusedExitCode = 1;

/// **Load-bearing for the ctest registration, not tidiness.** `PASS_REGULAR_EXPRESSION` and
/// `FAIL_REGULAR_EXPRESSION` are each documented as unable to override a system-level failure, and
/// a raw `SIGABRT` is one -- so without this handler the assertion arrives as a signal and the
/// scheme is defeated. It writes nothing: `_Exit` flushes no stream, and an abort handler may call
/// only async-signal-safe functions. The assertion's own text is written by the C runtime BEFORE it
/// raises the signal, on stderr, which is unbuffered.
/// @param signalNumber Ignored; only SIGABRT is handled.
extern "C" void onAbort(int signalNumber)
{
    std::ignore = signalNumber;
    std::_Exit(RefusedExitCode);
}

/// A timer callback that is never meant to run.
/// @param state Ignored.
void never(void* state)
{
    std::ignore = state;
}

/// A flow that does nothing: what is spawned does not matter, only which thread spawns it.
/// @return A task that completes at once.
core::async::Task<void> doNothing()
{
    co_return;
}

/// One loop-thread-only member, called from a thread that is not driving the loop.
struct Mode
{
    std::string_view name;                                ///< The command-line mode.
    void (*call)(std::unique_ptr<core::net::EventLoop>&); ///< The forbidden call.
};

// The loop is held by `unique_ptr` so that the destructor has a mode like every other member.
constexpr auto Modes = std::array {
    Mode { .name = "destroy", .call = [](std::unique_ptr<core::net::EventLoop>& loop) { loop.reset(); } },
    Mode { .name = "runOnce",
           .call = [](std::unique_ptr<core::net::EventLoop>& loop) {
               std::ignore = loop->runOnce(core::platform::SteadyDuration::zero());
           } },
    Mode { .name = "cancelPending",
           .call = [](std::unique_ptr<core::net::EventLoop>& loop) {
               std::ignore = loop->cancelPending(std::coroutine_handle<> {});
           } },
    Mode { .name = "requestStop", .call = [](std::unique_ptr<core::net::EventLoop>& loop) { loop->requestStop(); } },
    Mode { .name = "spawn", .call = [](std::unique_ptr<core::net::EventLoop>& loop) { loop->spawn(doNothing()); } },
    Mode { .name = "addTimer",
           .call = [](std::unique_ptr<core::net::EventLoop>& loop) {
               std::ignore = loop->addTimer(loop->clock().now(), &never, nullptr);
           } },
    Mode { .name = "cancelTimer",
           .call = [](std::unique_ptr<core::net::EventLoop>& loop) {
               std::ignore = loop->cancelTimer(core::net::TimerId::invalid());
           } },
    Mode { .name = "resumeSoon",
           .call = [](std::unique_ptr<core::net::EventLoop>& loop) { loop->resumeSoon(core::async::ParkedWork {}); } },
    Mode { .name = "registerPark",
           .call = [](std::unique_ptr<core::net::EventLoop>& loop) {
               std::ignore = loop->registerPark(core::net::ParkEntry {});
           } },
    Mode { .name = "unregisterPark",
           .call = [](std::unique_ptr<core::net::EventLoop>& loop) {
               loop->unregisterPark(core::net::ParkId::invalid());
           } },
    Mode { .name = "wakeReasonOf",
           .call = [](std::unique_ptr<core::net::EventLoop>& loop) {
               std::ignore = loop->wakeReasonOf(core::net::ParkId::invalid());
           } },
    Mode { .name = "notifyHandleClosing",
           .call = [](std::unique_ptr<core::net::EventLoop>& loop) {
               loop->notifyHandleClosing(core::platform::InvalidHandle, core::net::FdWakePolicy::Resume);
           } },
};

} // namespace

#endif

/// @param argc The argument count.
/// @param argv The member to call from a second thread; see `Modes`.
/// @return Never, in a build with assertions: the refusal aborts.
int main(int argc, char** argv)
{
#ifdef NDEBUG
    /// The exit code ctest is told to read as "this configuration could not run the case".
    constexpr auto SkipExitCode = 77;
    std::ignore = argc;
    std::ignore = argv;
    std::fputs("loop-affinity-canary: SKIPPED -- assertions are compiled out in this configuration\n",
               stderr);
    return SkipExitCode;
#else
    if (argc != 2)
    {
        std::fputs("usage: core-cpp-loop-affinity-canary <member>\n", stderr);
        return 2;
    }
    auto const requested = std::string_view { argv[1] };
    auto const mode = std::ranges::find(Modes, requested, &Mode::name);
    if (mode == Modes.end())
    {
        std::fputs("loop-affinity-canary: unknown mode\n", stderr);
        return 2;
    }

    std::signal(SIGABRT, &onAbort);

    // A NATIVE backend: this is about which thread calls, not about who owns the wait, and a
    // host-driven loop refuses `run()` for a different reason entirely.
    auto const backend = core::net::makeDefaultBackend();
    if (!backend)
    {
        std::fputs("loop-affinity-canary: no default backend on this platform\n", stderr);
        return 2;
    }
    auto loop = std::make_unique<core::net::EventLoop>(*backend);
    auto* const driven = loop.get();

    auto entered = std::atomic<bool> { false };
    driven->post([&entered] { entered.store(true, std::memory_order_release); });
    auto worker = std::thread { [driven] { driven->run(); } };

    // Calling before the loop is genuinely running is the LEGITIMATE call -- nothing is driving it
    // yet -- so waiting for a turn to have happened is what makes this the violation rather than a
    // race. Bounded, and says what it waited for: a `post` or wake that never reaches the worker
    // would otherwise spin every mode into ctest's bare timeout, which names nothing. `_Exit`,
    // because the worker is still inside `run()` and a `return` would terminate on its `std::thread`.
    auto const clock = core::platform::SteadyClock {};
    auto const giveUpAt = clock.now() + std::chrono::seconds { 30 };
    while (!entered.load(std::memory_order_acquire))
    {
        if (clock.now() >= giveUpAt)
        {
            std::println(stderr,
                         "loop-affinity-canary: {}: the worker never ran a posted callback in 30 s",
                         requested);
            std::_Exit(1);
        }
        std::this_thread::yield();
    }

    std::println(stderr, "loop-affinity-canary: {}: about to call it from a second thread", requested);
    mode->call(loop);

    // Unreachable where assertions are on. Reaching it means the predicate answered true from a
    // second thread while another was driving, which is the defect. The loop may already be gone
    // (the `destroy` mode), so nothing below touches it except through `driven` while it exists.
    std::println(stderr, "loop-affinity-canary: {} from a second thread was accepted", requested);
    if (loop)
        loop->stop();
    worker.join();
    return 0;
#endif
}
