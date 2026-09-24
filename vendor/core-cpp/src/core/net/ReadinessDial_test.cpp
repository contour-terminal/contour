// SPDX-License-Identifier: Apache-2.0
#include <core/async/Cancellation.hpp>
#include <core/async/Task.hpp>
#include <core/async/WhenAny.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IListener.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/ReadinessDial.hpp>
#include <core/net/SocketAddress.hpp>
#include <core/net/Sockets.hpp>
#include <core/net/detail/DialPrimitives.hpp>
#include <core/net/detail/ScopeGuard.hpp>
#include <core/net/detail/StreamSocketOptions.hpp>
#include <core/net/testing/BackendMatrix.hpp>
#include <core/net/testing/CoroTestSupport.hpp>
#include <core/platform/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifndef _WIN32
    #include <sys/socket.h>

    #include <unistd.h>
#endif

using core::async::Task;
using core::net::EventLoop;
using core::net::ISocket;
using core::net::NetErrorCode;
using core::net::ResolvedEndpoint;
using core::net::SocketResult;
using core::net::detail::dialReadiness;
using core::net::detail::StreamSocketOptions;
using core::net::testing::BackendMatrix;
using core::platform::SteadyTimePoint;

namespace
{

/// The loopback endpoint for @p port, through the production resolver.
///
/// A literal, so nothing here depends on a name server; `SystemAddressResolver` is still the
/// thing that fills the sockaddr, so a case dials exactly the bytes production would.
///
/// **Called into a named local before the `co_await`, never inside its full-expression.** With
/// the call inside it, MSVC 14.51 at /O2 cannot emit the tail call a symmetric transfer requires
/// and reports C4737, which /WX makes fatal. A plain `ResolvedEndpoint {}` in the same place does
/// not trigger it; the call does.
[[nodiscard]] ResolvedEndpoint loopbackEndpoint(std::uint16_t port)
{
    auto resolver = core::net::SystemAddressResolver {};
    auto resolved = resolver.resolve("127.0.0.1", port);
    REQUIRE(resolved.has_value());
    REQUIRE_FALSE(resolved->empty());
    return resolved->front();
}

/// The lowest file descriptor the process would be handed next.
///
/// A leak check rather than a platform branch in logic: POSIX hands out the lowest free
/// descriptor, so a dial that abandoned one makes this number GROW. There is no equivalent
/// question to ask Windows — handles are not allocated lowest-first — so this abstains there and
/// the case falls back to the park-count assertion, which is portable and which is what actually
/// catches the leak that matters (a park nothing will retire).
[[nodiscard]] std::optional<int> nextDescriptor() noexcept
{
#ifdef _WIN32
    return std::nullopt;
#else
    auto const probe = ::socket(AF_INET, SOCK_STREAM, 0);
    if (probe < 0)
        return std::nullopt;
    ::close(probe);
    return probe;
#endif
}

/// How a non-blocking connect to @p endpoint is answered on this machine: at once, or later
/// through readiness.
///
/// Asked with the dial's OWN primitives, in the dial's own order (`openDialSocket`, then
/// `beginConnect`), so the answer is the path `dialReadiness` takes for the same endpoint rather
/// than a guess about the stack. The probe's socket is closed before anything is dialled for real.
/// @return True when the connect was left outstanding — the dial then parks and reads `SO_ERROR`
///         after the loop reports readiness; false when it was answered synchronously, in which
///         case the dial never parks at all.
[[nodiscard]] bool connectGoesThroughReadiness(ResolvedEndpoint const& endpoint)
{
    auto opened = core::net::detail::openDialSocket(endpoint);
    REQUIRE(opened.has_value());
    auto handles = *opened;
    auto const started = core::net::detail::beginConnect(handles, endpoint);
    core::net::detail::closeDialSocket(nullptr, handles);
    return started.has_value() && *started == core::net::detail::ConnectProgress::Pending;
}

/// A listening port that accepts nothing, plus enough pending connections to saturate its
/// backlog — so the NEXT dial to it stays outstanding instead of completing.
///
/// Arranged rather than waited for, and it can fail to arrange: a kernel that over-accepts, or a
/// Windows stack that completes the handshake regardless of the backlog, leaves nothing pending.
/// A case that cannot arrange it SKIPs rather than passing on a dial that quietly succeeded.
struct SaturatedListener
{
    std::unique_ptr<core::net::IListener> listener;
    std::vector<std::unique_ptr<ISocket>> pending;
    std::uint16_t port = 0;

    /// True once a fill dial ran out of time rather than connecting — which is the proof that
    /// the NEXT dial will stay outstanding too, and the only thing that makes the cases below
    /// meaningful.
    bool saturated = false;
};

Task<void> saturate(EventLoop* loop, SaturatedListener* out)
{
    auto bound = core::net::listen(*loop, core::net::ListenOptions { .host = "127.0.0.1", .backlog = 1 });
    REQUIRE(bound.has_value());
    out->listener = std::move(*bound);
    out->port = out->listener->boundPort();

    // **Every fill dial is bounded, and the bound is what stops this helper from becoming the
    // hang it is arranging.** A dial with no deadline against a backlog that is already full
    // never returns — which is precisely the state being arranged, so the first one to reach it
    // would hang the case that wanted it.
    constexpr auto Fill = 32;
    constexpr auto PerDial = std::chrono::milliseconds { 300 };
    auto const endpoint = loopbackEndpoint(out->port);
    for ([[maybe_unused]] auto const attempt: std::views::iota(0, Fill))
    {
        auto dialled =
            co_await dialReadiness(loop, endpoint, loop->clock().now() + PerDial, StreamSocketOptions {});
        if (!dialled.has_value())
        {
            // A timeout means the backlog no longer takes a connection. Anything else — a
            // refusal, an unreachable route — means this stack does not behave the way the
            // arrangement needs, and the cases SKIP on `saturated` staying false.
            out->saturated = dialled.error().code == NetErrorCode::Timeout;
            co_return;
        }
        out->pending.push_back(std::move(*dialled));
    }
}

/// Dials @p port and reports what came back.
Task<void> dialOnce(EventLoop* loop, std::uint16_t port, std::chrono::milliseconds budget, SocketResult* out)
{
    auto const deadline =
        budget > std::chrono::milliseconds::zero() ? loop->clock().now() + budget : SteadyTimePoint::max();
    auto const endpoint = loopbackEndpoint(port);
    *out = co_await dialReadiness(loop, endpoint, deadline, StreamSocketOptions {});
}

/// Accepts one connection and reads @p expected.size() bytes from it.
Task<void> acceptAndRead(core::net::IListener* listener, std::string_view expected, bool* ok)
{
    auto accepted = co_await listener->accept();
    if (!accepted.has_value())
        co_return;
    auto buffer = std::array<std::byte, 64> {};
    auto const read = co_await (*accepted)->read(std::span<std::byte> { buffer }.first(expected.size()));
    *ok = read.has_value() && *read == expected.size()
          && std::string_view { reinterpret_cast<char const*>(buffer.data()), expected.size() } == expected;
}

/// Dials @p port, writes @p payload and reports whether every byte went.
Task<void> dialAndWrite(EventLoop* loop, std::uint16_t port, std::string_view payload, bool* ok)
{
    auto const endpoint = loopbackEndpoint(port);
    auto dialled = co_await dialReadiness(loop, endpoint, SteadyTimePoint::max(), StreamSocketOptions {});
    if (!dialled.has_value())
        co_return;
    auto const bytes =
        std::span<std::byte const> { reinterpret_cast<std::byte const*>(payload.data()), payload.size() };
    auto const written = co_await (*dialled)->write(bytes);
    *ok = written.has_value() && *written == payload.size();
}

} // namespace

TEST_CASE("a readiness dial produces a connected socket on every backend", "[net]")
{
    for (auto const& backend: BackendMatrix)
    {
        auto source = core::net::makeBackend(backend.kind);
        if (!source)
            continue; // not built on this platform

        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto loop = EventLoop { *source };
            auto bound = core::net::listen(loop, core::net::ListenOptions { .host = "127.0.0.1" });
            REQUIRE(bound.has_value());
            auto listener = std::move(*bound);

            auto served = false;
            auto wrote = false;
            loop.blockOn(
                core::net::testing::allOf(acceptAndRead(listener.get(), "hello", &served),
                                          dialAndWrite(&loop, listener->boundPort(), "hello", &wrote)));
            CHECK(wrote);
            CHECK(served);
        }
    }
}

TEST_CASE("a refused connect completes with the refusal on every backend", "[net]")
{
    // **Ruling R101, in its natural habitat.** A dial checks `SO_ERROR`; it never trusts which
    // callback fired. On Linux a failed connect can arrive as an error with neither direction
    // set, while on macOS the write filter fires with `EV_EOF` and the backend reports
    // `Writable` — so a dial that believed the callback would hand its caller a socket whose
    // FIRST WRITE fails, days later, on one platform only.
    for (auto const& backend: BackendMatrix)
    {
        auto source = core::net::makeBackend(backend.kind);
        if (!source)
            continue;

        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto loop = EventLoop { *source };

            // A port that was bound and then given up: the kernel answers a SYN to it with a
            // reset, which is the ordinary refusal every consumer will meet.
            auto closedPort = std::uint16_t { 0 };
            {
                auto bound = core::net::listen(loop, core::net::ListenOptions { .host = "127.0.0.1" });
                REQUIRE(bound.has_value());
                closedPort = (*bound)->boundPort();
            }
            REQUIRE(closedPort != 0);

            // **Which path the refusal takes, asked rather than assumed.** R101 exists for the
            // path through readiness: the dial parks, the loop reports the socket ready — on
            // kqueue as `EV_EOF` on the write filter, which the backend reports as `Writable` —
            // and only `SO_ERROR` says it was a refusal. A stack that refuses a loopback connect
            // SYNCHRONOUSLY never takes that path: the dial leaves through `beginConnect`'s errno
            // and the refusal below would be green without R101 having been exercised. BSD-derived
            // stacks may do exactly that, since the reset is processed inline.
            auto const throughReadiness = connectGoesThroughReadiness(loopbackEndpoint(closedPort));

            auto answer = SocketResult {};
            loop.blockOn(dialOnce(&loop, closedPort, std::chrono::milliseconds { 5000 }, &answer));

            REQUIRE_FALSE(answer.has_value());
            INFO("dialled the just-closed port " << closedPort << " and got: " << answer.error().context);
            INFO("the refusal was reported " << (throughReadiness ? "through readiness" : "synchronously"));
            CHECK(answer.error().code == NetErrorCode::ConnRefused);
            CHECK(loop.parkedWaiterCount() == 0);

            // Said out loud rather than passed quietly: on such a stack this section shows that a
            // refusal reaches the caller, and NOT that the dial ignores which callback fired.
            if (!throughReadiness)
                SKIP("backend=" << backend.name
                                << ": this stack refused the loopback connect synchronously, so the dial "
                                   "never parked and R101's readiness path was not exercised here");
        }
    }
}

TEST_CASE("a dial that cannot complete is ended by its deadline and takes its descriptor with it", "[net]")
{
    auto source = core::net::makeBackend(core::net::preferredBackendKind());
    REQUIRE(source != nullptr);
    auto loop = EventLoop { *source };

    auto saturated = SaturatedListener {};
    loop.blockOn(saturate(&loop, &saturated));
    if (!saturated.saturated)
        SKIP("this stack completes a dial whose listener never accepts, so no dial could be left "
             "outstanding to run out of time");

    auto const before = nextDescriptor();

    auto answer = SocketResult {};
    loop.blockOn(dialOnce(&loop, saturated.port, std::chrono::milliseconds { 500 }, &answer));

    REQUIRE_FALSE(answer.has_value());
    INFO("dialled a listener with a saturated backlog and got: " << answer.error().context);
    CHECK(answer.error().code == NetErrorCode::Timeout);
    // The park is what a leak would show as: a readiness registration nothing will ever retire.
    CHECK(loop.parkedWaiterCount() == 0);
    if (before.has_value())
        CHECK(nextDescriptor() == before); // POSIX hands out the lowest free fd; see nextDescriptor
}

TEST_CASE("the flow's stop token cancels a dial in flight", "[net]")
{
    auto source = core::net::makeBackend(core::net::preferredBackendKind());
    REQUIRE(source != nullptr);
    auto loop = EventLoop { *source };

    auto saturated = SaturatedListener {};
    loop.blockOn(saturate(&loop, &saturated));
    if (!saturated.saturated)
        SKIP("this stack completes a dial whose listener never accepts, so no dial could be left "
             "outstanding to cancel");

    auto const before = nextDescriptor();

    // The dial has no deadline of its own: the only thing that can end it is the stop. If the
    // stop does not reach it, this case HANGS rather than failing — which is why the binary
    // carries a TIMEOUT and why the arrangement above is asserted before we get here.
    auto answer = SocketResult {};
    auto cancelled = false;
    auto dial = [](EventLoop* lp, std::uint16_t port, SocketResult* out, bool* threw) -> Task<void> {
        auto const endpoint = loopbackEndpoint(port);
        try
        {
            *out = co_await dialReadiness(lp, endpoint, SteadyTimePoint::max(), StreamSocketOptions {});
        }
        catch (core::async::OperationCancelled const&)
        {
            *threw = true;
        }
    };
    auto stopAfter = [](EventLoop* lp, std::chrono::milliseconds after) -> Task<void> {
        co_await lp->delay(after);
        lp->requestStop();
    };

    loop.blockOn(core::net::testing::allOf(dial(&loop, saturated.port, &answer, &cancelled),
                                           stopAfter(&loop, std::chrono::milliseconds { 100 })));

    // A cancel from the FLOW unwinds; a cancel from the RESOURCE is a value. Either is a report,
    // and what must NOT happen is a dial that resolved into a usable socket after a stop.
    CHECK((cancelled || !answer.has_value()));
    CHECK(loop.parkedWaiterCount() == 0);
    if (before.has_value())
        CHECK(nextDescriptor() == before);
}

TEST_CASE("a whenAny loser's stop cancels a parked dial through the dial's own callback", "[net]")
{
    // **The common real use, and the only arm that is the dial's own.** The two cases around this
    // one stop the loop's ROOT token: `requestStop()` also unparks everything, so it would end a
    // dial with no stop callback at all, and the cross-thread case may land before the park. A
    // `whenAny` loser is stopped through a CHILD token the loop knows nothing about — nothing
    // unparks it but the callback the dial registered on that token.
    //
    // The delay arm asserts the dial is parked at the moment it wins, so this case says which arm
    // it covered instead of leaving that to timing.
    auto source = core::net::makeBackend(core::net::preferredBackendKind());
    REQUIRE(source != nullptr);
    auto loop = EventLoop { *source };

    auto saturated = SaturatedListener {};
    loop.blockOn(saturate(&loop, &saturated));
    if (!saturated.saturated)
        SKIP("this stack completes a dial whose listener never accepts, so no dial could be left "
             "outstanding to cancel");

    auto const before = nextDescriptor();

    auto answer = SocketResult {};
    auto threw = false;
    auto parkedWhenStopped = std::size_t { 0 };
    auto winner = std::optional<std::size_t> {};

    auto dial = [](EventLoop* lp, std::uint16_t port, SocketResult* out, bool* cancelled) -> Task<void> {
        auto const endpoint = loopbackEndpoint(port);
        try
        {
            *out = co_await dialReadiness(lp, endpoint, SteadyTimePoint::max(), StreamSocketOptions {});
        }
        catch (core::async::OperationCancelled const&)
        {
            *cancelled = true;
            throw; // a loser unwinds; swallowing it would make it look like the winner
        }
    };
    auto delayArm = [](EventLoop* lp, std::size_t* parked) -> Task<void> {
        co_await lp->delay(std::chrono::milliseconds { 100 });
        *parked = lp->parkedWaiterCount();
    };
    auto race = [](Task<void> dialArm, Task<void> timerArm, std::optional<std::size_t>* won) -> Task<void> {
        auto arms = std::vector<Task<void>> {};
        arms.push_back(std::move(dialArm));
        arms.push_back(std::move(timerArm));
        *won = co_await core::async::whenAny(std::move(arms));
    };
    loop.blockOn(
        race(dial(&loop, saturated.port, &answer, &threw), delayArm(&loop, &parkedWhenStopped), &winner));

    CHECK(winner == std::optional<std::size_t> { 1 });
    CHECK(parkedWhenStopped == 1); // the dial was parked, so the callback is what reached it
    CHECK(threw);
    CHECK(loop.parkedWaiterCount() == 0);
    if (before.has_value())
        CHECK(nextDescriptor() == before);
}

TEST_CASE("a stop from ANOTHER thread cancels a dial in flight", "[net]")
{
    // **Built rather than argued.** The case above stops the flow from the loop's own thread, so
    // the stop callback runs on the loop and every access to the dial's park id is ordered by
    // construction. That is the arrangement under which a race between the callback's READ of the
    // park id and the loop thread's WRITE of it cannot occur — so a green ThreadSanitizer over
    // that case alone says nothing about the cross-thread path, which is the path
    // `ResultAwaitable::cancelThrough` exists for: a watchdog, a signal handler, a peer's thread.
    //
    // This case is that path. It is also what the dial's own comment is now checkable against: the
    // park id is written ONCE, before the callback can be registered, and `settleDial` retires the
    // park without clearing it.
    auto source = core::net::makeBackend(core::net::preferredBackendKind());
    REQUIRE(source != nullptr);
    auto loop = EventLoop { *source };

    auto saturated = SaturatedListener {};
    loop.blockOn(saturate(&loop, &saturated));
    if (!saturated.saturated)
        SKIP("this stack completes a dial whose listener never accepts, so no dial could be left "
             "outstanding to cancel");

    auto const before = nextDescriptor();

    // Set by the dialling flow immediately before it dials, so the stopping thread waits for the
    // flow to exist rather than for a fixed time. What is left after it is a socket() and a
    // connect() — microseconds — and the margin below covers them.
    auto dialling = std::atomic<bool> { false };
    auto answer = SocketResult {};
    auto cancelled = false;

    auto dial = [](EventLoop* lp,
                   std::uint16_t port,
                   std::atomic<bool>* started,
                   SocketResult* out,
                   bool* threw) -> Task<void> {
        started->store(true, std::memory_order_release);
        auto const endpoint = loopbackEndpoint(port);
        try
        {
            *out = co_await dialReadiness(lp, endpoint, SteadyTimePoint::max(), StreamSocketOptions {});
        }
        catch (core::async::OperationCancelled const&)
        {
            *threw = true;
        }
    };

    // **The margin is a margin, not a synchronisation, and the case cannot flake on it.** A stop
    // that lands before the dial parks is observed by `await_suspend`, which returns without
    // arming anything and reports the same cancellation; the case still passes, it just covers
    // the pre-park arm instead of the parked one. Only the parked arm can produce the race this
    // exists for, and 50ms after the flow has started is ample for a `connect` to `EINPROGRESS`.
    auto stopper = std::thread { [&loop, &dialling] {
        while (!dialling.load(std::memory_order_acquire))
            std::this_thread::sleep_for(std::chrono::milliseconds { 1 });
        std::this_thread::sleep_for(std::chrono::milliseconds { 50 });
        // `rootStopSource()` rather than `requestStop()`: the latter must be called on the loop's
        // thread, and the whole point here is that this one is not. The stop reaches the parked
        // dial through the callback it registered, which routes to `EventLoop::requestCancel` —
        // documented safe from any thread.
        loop.rootStopSource().request_stop();
    } };
    // Joined before any assertion, so a failed CHECK cannot leave a thread running into a
    // destroyed loop — a red turned into a crash is worse than the red.
    auto const join = core::net::detail::ScopeGuard { [&stopper]() noexcept {
        if (stopper.joinable())
            stopper.join();
    } };

    loop.blockOn(dial(&loop, saturated.port, &dialling, &answer, &cancelled));

    CHECK((cancelled || !answer.has_value()));
    CHECK(loop.parkedWaiterCount() == 0);
    if (before.has_value())
        CHECK(nextDescriptor() == before);
}
