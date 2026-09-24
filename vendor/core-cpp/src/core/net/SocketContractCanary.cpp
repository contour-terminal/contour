// SPDX-License-Identifier: Apache-2.0
///
/// @file
/// The canaries that prove `core::net::contract`'s three guards actually fire, against a REAL
/// socket.
///
/// **What is driven here is the CALL SITE, not the guard.** Asserting the assertion would prove
/// `assert` works and say nothing about whether a transport ever reaches it — and every one of
/// these rules was, before it had a guard, a thing the code was simply expected to do. A guard with
/// no arriving caller is a comment with an `assert` in it.
///
/// Each mode is its own process, because an assertion cannot be asserted from inside a Catch case:
/// it aborts, which ends the binary rather than the case.
///
/// **Registered on MARKERS, not on `WILL_FAIL`, and that is the load-bearing part.** `WILL_FAIL`
/// inverts ANY non-zero exit, so a canary that dies before it reaches the guarded call — a bad
/// argument, a backend this platform does not build, a socket pair that could not be made — reads
/// exactly like one whose assertion fired. Worse, an uncaught throw reaches `abort()`, this
/// program's own SIGABRT handler and `_Exit(1)` with stderr EMPTY, because `core::testing_dialogs`
/// suppresses the abort text: exit 1 and silent, byte-identical to the answer we want. No
/// alternation of failure strings can catch that one, because there is no string to match.
///
/// So each mode prints `reached the guarded call` IMMEDIATELY BEFORE the forbidden operation, and
/// `SURVIVED` immediately after it. The PASS criterion is the first marker, which proves the
/// process got as far as the call; the FAIL criterion is the second, plus every early-exit text.
/// `PASS_REGULAR_EXPRESSION` ignores the exit code and `WILL_FAIL` inverts the pass criteria, so
/// the two are alternatives and never companions — `ctest` checks the fail expression first, which
/// is what lets the pass marker sit on both paths.
///
/// **The slot modes run in every build, Release included.** A second operation armed over a parked
/// one used to be refused by an `assert` alone, so under `NDEBUG` it displaced the parked one, which
/// was then never resumed: a silent hang in exactly the builds that ship. The slot guards now end
/// the process in every build, naming the direction (and the handle, where the socket has one), so
/// a Release leg that reaches `SURVIVED` is the defect back. `empty-read-buffer` is still an
/// assertion -- an empty read answers a false EOF rather than hanging -- and skips (exit 77) where
/// assertions are compiled out; `SKIP_RETURN_CODE` is evaluated ahead of the expressions, so a
/// Release run of that one mode abstains rather than failing for want of a marker.

#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/testing/InMemoryTransport.hpp>
#include <core/net/testing/ScriptedBackend.hpp>
#include <core/platform/Clock.hpp>

#include <array>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <ranges>
#include <span>
#include <tuple>
#include <vector>

namespace
{

/// Whether this build compiled `assert` out, which only `empty-read-buffer` still depends on.
#ifdef NDEBUG
constexpr auto AssertionsCompiledOut = true;
#else
constexpr auto AssertionsCompiledOut = false;
#endif

/// What this process exits with once a guard has fired.
///
/// **An `abort()` is not a failed exit code, and ctest tells them apart.** A signal is an
/// "exception" there, and a `FAIL_REGULAR_EXPRESSION` match is not consulted for one. Converting it
/// here keeps the assertion real -- it still had to fire to get here -- and keeps a genuine crash
/// distinguishable: SIGSEGV is not handled, so it still arrives as the exception it is.
constexpr int RefusedExitCode = 1;

/// One write's worth of the payload that fills the send window.
///
/// **Not "big enough that no send buffer takes it", which is what it used to claim**: a Windows
/// non-blocking send takes all 8MiB of it at once (measured, Task B7b), so a single write parks on
/// POSIX and completes on Windows. What makes a write park is the window being FULL, so the write
/// modes keep writing until one stays pending (@c parkAWrite), bounded by @c MaxFillWrites.
constexpr std::size_t UnsendablePayload = std::size_t { 8 } * 1024 * 1024;

/// How many payload-sized writes the write modes issue, at most, waiting for one to park: 256MiB
/// to a peer that never reads. A stack that takes all of it has no window this canary can fill,
/// and it SKIPs saying so rather than passing on a write that never parked.
constexpr int MaxFillWrites = 32;

/// The exit code ctest reads as "this configuration could not run the case".
constexpr auto SkipExitCode = 77;

/// Turns a guard's abort into an exit code.
///
/// `_exit`-shaped on purpose: an abort handler runs with the process already committed to dying, so
/// it must not unwind, flush or allocate, and only async-signal-safe calls are legal in it. That is
/// why the markers below flush when they are PRINTED rather than here -- `fflush` is not one of
/// them, and a buffer left for this handler to flush would die with the process.
/// @param signalNumber Ignored; only SIGABRT is handled.
extern "C" void onAbort(int signalNumber)
{
    std::ignore = signalNumber;
    std::_Exit(RefusedExitCode);
}

/// Prints the PASS marker: this process reached the guarded call, so whatever happens next is the
/// guard's answer and not an early exit.
///
/// Flushed here, because the very next statement is expected to `abort()` and an unflushed buffer
/// dies with the process -- which would leave ctest reading the same empty stderr that an uncaught
/// throw produces, and those are the two outcomes this registration exists to tell apart.
///
/// Emitted as literal pieces rather than one format string, as every other canary in this module
/// does: `modernize-use-std-print` rewrites a `fprintf` into `std::println`, and `<print>` is past
/// the libc++ 17 floor this tree builds against.
///
/// **It names the MODE, and that is what the registration matches on.** A marker shared by all
/// three modes would be printed by whichever guard the process actually reached, so a mode that
/// fell into the wrong branch -- or whose condition was inverted -- would still print it and still
/// pass. The mode-specific marker is what ties the verdict to the guard the test asked for.
/// @param mode Which mode is running, as ctest named it on the command line.
/// @param what Which guard is about to be provoked.
void announce(char const* mode, char const* what)
{
    std::fputs("socket-contract-canary: ", stderr);
    std::fputs(mode, stderr);
    std::fputs(": reached the guarded call (", stderr);
    std::fputs(what, stderr);
    std::fputs(")\n", stderr);
    std::fflush(stderr);
}

/// Prints the FAIL marker: the guarded call returned, so the guard did not fire.
/// @param what What the socket wrongly accepted.
void survived(char const* what)
{
    std::fputs("socket-contract-canary: SURVIVED -- ", stderr);
    std::fputs(what, stderr);
    std::fputs("\n", stderr);
    std::fflush(stderr);
}

/// Parks a readability watch and never resumes -- the stale parked wait a second read arms over.
/// @param sock The socket to watch.
/// @return A flow that suspends on the watch.
core::async::Task<void> watchForever(core::net::ISocket* sock)
{
    std::ignore = co_await sock->waitReadable();
}

/// Parks a write nothing will drain -- the stale parked write a second write arms over.
/// @param sock The socket to write to.
/// @param payload The bytes; must outlive the flow.
/// @return A flow that suspends on the write.
core::async::Task<void> writeForever(core::net::ISocket* sock, std::vector<std::byte> const* payload)
{
    std::ignore = co_await sock->write(std::span<std::byte const> { *payload });
}

/// Writes @p payload until one write stays PARKED, which is the state both write modes need.
///
/// Every write but the last completes -- the send buffer took it -- and its flow finishes; the one
/// that cannot is left parked on the socket's single write slot.
/// @param loop The loop the socket is driven by.
/// @param sock The socket to write to.
/// @param payload The bytes; must outlive every flow.
/// @return Whether a write parked within @c MaxFillWrites.
bool parkAWrite(core::net::EventLoop& loop, core::net::ISocket* sock, std::vector<std::byte> const* payload)
{
    for ([[maybe_unused]] auto const attempt: std::views::iota(0, MaxFillWrites))
    {
        loop.spawn(writeForever(sock, payload));
        std::ignore = loop.runOnce();
        if (loop.parkedWaiterCount() > 0)
            return true;
    }
    return false;
}

/// Reports that no write could be made to park, and says so as a SKIP rather than a pass.
/// @return The skip exit code.
int noWindowToFill()
{
    std::fputs("socket-contract-canary: SKIPPED -- no write parked after 32 writes of 8MiB to a peer "
               "that never reads, so there was no parked write to arm over\n",
               stderr);
    return SkipExitCode;
}

/// Counts the calls a frameless park's owner receives.
/// @param state A @c std::size_t counter.
/// @param wake Ignored.
void countWake(void* state, core::net::ParkWake wake)
{
    std::ignore = wake;
    ++*static_cast<std::size_t*>(state);
}

/// Files two parks watching @p interest on one handle's lifetime registration, through the loop's
/// own interface: the loop's half of the one-operation-per-direction rule, which a socket reaches
/// only past its own slot guard and a consumer's awaitable reaches directly.
/// @param mode Which mode is running, for the markers.
/// @param interest The direction both parks watch.
/// @return Never, where the guard fires; 0 after printing `SURVIVED` where it does not.
int parkTwiceOnOneWatch(char const* mode, core::net::Interest interest)
{
    auto clock = core::platform::ManualClock {};
    auto backend = core::net::testing::ScriptedBackend {};
    auto loop = core::net::EventLoop { backend, clock };
    // Value-initialised rather than InvalidHandle, so the park reaches the watch; the scripted
    // backend asks the OS nothing about it.
    auto const handle = core::platform::NativeHandle {};
    auto wakes = std::size_t { 0 };
    auto const park = [&] {
        return loop.registerPark(
            core::net::ParkEntry::onReadyCallback(&countWake,
                                                  &wakes,
                                                  handle,
                                                  core::net::DefaultHandleKind,
                                                  interest,
                                                  core::net::RegistrationLifetime::UntilClosed));
    };
    auto const first = park();
    if (!first)
    {
        std::fputs("socket-contract-canary: the first park was refused, so nothing was tested\n", stderr);
        return 2;
    }
    announce(mode, "a second park on one handle's watch in one direction");
    auto const second = park();
    survived("a second park displaced the first from the handle's watch");
    loop.unregisterPark(second);
    loop.unregisterPark(first);
    return 0;
}

} // namespace

/// @param argc The argument count.
/// @param argv `read-slot`, `write-slot`, `write-slot-inline` or `empty-read-buffer`,
///        naming which guard to provoke.
/// @return Never, where the guard fires: it ends the process.
int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::fputs("usage: core-cpp-socket-contract-canary <read-slot|write-slot|write-slot-inline|"
                   "watch-read-slot|watch-write-slot|empty-read-buffer>\n",
                   stderr);
        return 2;
    }

    std::signal(SIGABRT, &onAbort);

    if (std::strcmp(argv[1], "watch-read-slot") == 0)
        return parkTwiceOnOneWatch("watch-read-slot", core::net::Interest::Read);
    if (std::strcmp(argv[1], "watch-write-slot") == 0)
        return parkTwiceOnOneWatch("watch-write-slot", core::net::Interest::Write);

    auto backend = core::net::makeDefaultBackend();
    if (!backend)
    {
        std::fputs("socket-contract-canary: no default backend on this platform\n", stderr);
        return 2;
    }
    auto loop = core::net::EventLoop { *backend };
    auto pair = core::net::testing::makeSocketPair(loop);
    if (!pair.has_value())
    {
        std::fputs("socket-contract-canary: could not make a socket pair\n", stderr);
        return 2;
    }
    auto* const sock = pair->first.get();

    if (std::strcmp(argv[1], "empty-read-buffer") == 0)
    {
        if (AssertionsCompiledOut)
        {
            std::fputs("socket-contract-canary: SKIPPED -- assertions are compiled out in this "
                       "configuration, and this guard is an assertion\n",
                       stderr);
            return SkipExitCode;
        }
        // No loop turn needed: the guard is at the top of the verb, before anything can park.
        announce("empty-read-buffer", "empty read buffer");
        auto const refused = sock->read(std::span<std::byte> {});
        std::ignore = refused.await_ready();
        survived("read() accepted an empty buffer");
        return 0;
    }

    if (std::strcmp(argv[1], "read-slot") == 0)
    {
        // The watch has to be genuinely PARKED, not merely created: the slot is claimed when the
        // operation arms, which is inside `await_suspend`. A canary that only called the verb twice
        // would trip nothing and pass while the rule was gone.
        loop.spawn(watchForever(sock));
        std::ignore = loop.runOnce();
        if (loop.parkedWaiterCount() == 0)
        {
            std::fputs("socket-contract-canary: the watch did not park, so nothing was tested\n", stderr);
            return 2;
        }
        auto buffer = std::array<std::byte, 16> {};
        announce("read-slot", "a read armed over a parked readability watch");
        auto const second = sock->read(buffer);
        std::ignore = second.await_ready();
        survived("read() armed over a parked readability watch");
        return 0;
    }

    if (std::strcmp(argv[1], "write-slot") == 0)
    {
        auto const payload = std::vector<std::byte>(UnsendablePayload, std::byte { 0xA5 });
        if (!parkAWrite(loop, sock, &payload))
            return noWindowToFill();
        announce("write-slot", "a write armed over a parked write");
        auto const second = sock->write(std::span<std::byte const> { payload });
        std::ignore = second.await_ready();
        survived("write() armed over a parked write");
        return 0;
    }

    if (std::strcmp(argv[1], "write-slot-inline") == 0)
    {
        // **The other branch of the same verb, and the one that had no tripwire on it.**
        // `write-slot` above drives parked -> PARKED: `trySend` answers `nullopt`, the operation
        // goes on to arm, and the guard sits on that path. This mode drives parked -> INLINE,
        // where `trySend` answers immediately and the verb returns without ever arming. That is
        // the likelier of the two in production -- a write parks only when the send window is full
        // and unparks when it drains -- and the version reviewed in round 1 ran `_write = {}` on
        // it, dropping the parked awaitable and leaking its park while telling the caller the
        // write had succeeded.
        auto const payload = std::vector<std::byte>(UnsendablePayload, std::byte { 0xA5 });
        if (!parkAWrite(loop, sock, &payload))
            return noWindowToFill();

        // Half-close, so the NEXT send fails at once with EPIPE rather than blocking on a window
        // that is still full. That is what forces the inline-completion branch deterministically:
        // draining the peer instead would race the loop's own pump for the freed window. This
        // breaks `shutdownWrite`'s own precondition (no write outstanding) on purpose, and only a
        // plain socket lets it: it claims no write slot, where a TLS half-close would trip the
        // guard here instead of below.
        // `shutdownWrite` is an awaitable, and this is `main`, not a coroutine. It completes INLINE
        // on a plain socket, so `await_ready()` is the whole of driving it -- the same shape the
        // guarded calls below use.
        auto halfClose = sock->shutdownWrite();
        std::ignore = halfClose.await_ready();

        announce("write-slot-inline", "a write completing INLINE over a parked write");
        auto const second = sock->write(std::span<std::byte const> { payload });
        std::ignore = second.await_ready();
        survived("write() completed inline over a parked write");
        return 0;
    }

    std::fputs("socket-contract-canary: unknown mode\n", stderr);
    return 2;
}
