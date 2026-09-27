// SPDX-License-Identifier: Apache-2.0
#include <core/async/Task.hpp>
#include <core/async/WhenAny.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IAsyncAddressResolver.hpp>
#include <core/net/IConnector.hpp>
#include <core/net/IListener.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/SocketAddress.hpp>
#include <core/net/Sockets.hpp>
#include <core/net/ThreadedAddressResolver.hpp>
#include <core/net/detail/ScopeGuard.hpp>
#include <core/net/testing/BackendMatrix.hpp>
#include <core/net/testing/CoroTestSupport.hpp>
#include <core/net/testing/TestLoop.hpp>
#include <core/platform/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using core::async::Task;
using core::net::DialOptions;
using core::net::EventLoop;
using core::net::KeepAlive;
using core::net::NetErrorCode;
using core::net::ResolvedEndpoint;
using core::net::SocketResult;
using core::net::testing::BackendMatrix;

namespace
{

/// A blocking resolver that answers "this loopback port" for any name, and records the thread it
/// was asked on.
///
/// **The whole point of the seam.** `getaddrinfo` takes no timeout and a wedged resolver parks
/// whoever called it for as long as the platform's resolver library feels like; on an event loop
/// that is every connection on it, including the ones with nothing to do with the network. A
/// recording resolver is how that rule becomes an assertion instead of a paragraph.
class RecordingResolver final: public core::net::IAddressResolver
{
  public:
    explicit RecordingResolver(std::uint16_t port) noexcept: _port(port) {}

    [[nodiscard]] std::expected<std::vector<ResolvedEndpoint>, std::string> resolve(
        std::string_view /*host*/, std::uint16_t /*port*/) override
    {
        {
            auto const guard = std::scoped_lock { _mutex };
            _callers.push_back(std::this_thread::get_id());
        }
        auto system = core::net::SystemAddressResolver {};
        return system.resolve("127.0.0.1", _port);
    }

    /// @return The thread each lookup ran on, in call order.
    [[nodiscard]] std::vector<std::thread::id> callers() const
    {
        auto const guard = std::scoped_lock { _mutex };
        return _callers;
    }

  private:
    mutable std::mutex _mutex;
    std::vector<std::thread::id> _callers;
    std::uint16_t _port;
};

/// A blocking resolver that does not come back until it is told to: the nameserver that drops
/// packets, seen from the dial's side.
///
/// Held for the whole of a case, so no lookup through it returns while the dial is still waiting
/// for one. Released at the end so the resolver's worker can be joined.
class HeldResolver final: public core::net::IAddressResolver
{
  public:
    [[nodiscard]] std::expected<std::vector<ResolvedEndpoint>, std::string> resolve(
        std::string_view /*host*/, std::uint16_t /*port*/) override
    {
        auto lock = std::unique_lock { _mutex };
        ++_entered;
        _changed.notify_all();
        _changed.wait(lock, [this] { return _released; });
        return std::unexpected(std::string { "released after the case was over" });
    }

    /// Blocks until a lookup is inside @c resolve.
    void awaitEntered()
    {
        auto lock = std::unique_lock { _mutex };
        _changed.wait(lock, [this] { return _entered > 0; });
    }

    void release()
    {
        {
            auto const guard = std::scoped_lock { _mutex };
            _released = true;
        }
        _changed.notify_all();
    }

  private:
    std::mutex _mutex;
    std::condition_variable _changed;
    std::size_t _entered = 0;
    bool _released = false;
};

/// A decorator that records the thread a lookup RESUMES on — the thread the dial continues on
/// once its name is resolved.
///
/// **Measured here, at the resumption, and nowhere later.** A thread recorded once the whole dial
/// has returned says nothing: the dial parks on the loop again after resolving, so a flow that a
/// broken hand-back resumed on the resolver's worker is carried back to the loop's thread by the
/// NEXT resumption and looks healthy by the time anyone asks. That is not hypothetical — a
/// `settle` that called `resume()` inline passed a check made after the dial, in a Release build
/// where the loop's own affinity assert is compiled out.
class ResumptionRecorder final: public core::net::IAsyncAddressResolver
{
  public:
    explicit ResumptionRecorder(core::net::IAsyncAddressResolver& inner) noexcept: _inner(inner) {}

    [[nodiscard]] Task<core::net::ResolveResult> resolve(std::string host,
                                                         std::uint16_t port,
                                                         EventLoop* loop) override
    {
        auto answer = co_await _inner.resolve(std::move(host), port, loop);
        {
            auto const guard = std::scoped_lock { _mutex };
            _resumedOn.push_back(std::this_thread::get_id());
        }
        co_return answer;
    }

    /// @return The thread each lookup resumed its awaiting flow on, in call order.
    [[nodiscard]] std::vector<std::thread::id> resumedOn() const
    {
        auto const guard = std::scoped_lock { _mutex };
        return _resumedOn;
    }

  private:
    core::net::IAsyncAddressResolver& _inner;
    mutable std::mutex _mutex;
    std::vector<std::thread::id> _resumedOn;
};

/// Dials `silent.test` through @p connector and records what came back, and that it came back.
Task<void> dialInto(core::net::IConnector* connector, DialOptions options, SocketResult* out, bool* done)
{
    *out = co_await connector->connect("silent.test", 80, options);
    *done = true;
}

/// Sleeps until @p at on @p loop's clock.
Task<void> sleepTo(EventLoop* loop, core::platform::SteadyTimePoint at)
{
    co_await loop->sleepUntil(at);
}

/// Accepts one connection and reads @p expected from it.
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

/// Dials through @p connector and writes @p payload.
Task<void> dialAndWrite(core::net::IConnector* connector,
                        std::string host,
                        std::uint16_t port,
                        std::string_view payload,
                        bool* ok)
{
    auto dialled = co_await connector->connect(std::move(host), port, DialOptions {});
    if (!dialled.has_value())
        co_return;
    auto const bytes =
        std::span<std::byte const> { reinterpret_cast<std::byte const*>(payload.data()), payload.size() };
    auto const written = co_await (*dialled)->write(bytes);
    *ok = written.has_value() && *written == payload.size();
}

} // namespace

TEST_CASE("connect() never resolves a name on the loop's thread", "[net]")
{
    // **The case this task exists for.** An injected resolver records the thread that called it,
    // and the dial asserts that thread is not the loop's. Written against contour's inline
    // `getaddrinfo` it could not even be expressed: there was no seam to inject through.
    //
    // And the other half: the flow that asked comes back on the LOOP's thread, recorded at the
    // moment the lookup resumes it (see `ResumptionRecorder` for why no later moment will do).
    auto source = core::net::makeBackend(core::net::preferredBackendKind());
    REQUIRE(source != nullptr);
    auto loop = EventLoop { *source };

    auto bound = core::net::listen(loop, core::net::ListenOptions { .host = "127.0.0.1" });
    REQUIRE(bound.has_value());
    auto listener = std::move(*bound);

    // A NAME, so the fast path for literals does not apply and the lookup is genuinely offloaded.
    // The inner answers from the listener's port, so no name server is involved anywhere.
    auto inner = RecordingResolver { listener->boundPort() };
    auto resolver = core::net::ThreadedAddressResolver { inner };
    auto recorder = ResumptionRecorder { resolver };
    auto connector = core::net::makeConnector(loop, recorder);

    auto served = false;
    auto wrote = false;
    loop.blockOn(core::net::testing::allOf(acceptAndRead(listener.get(), "hello", &served),
                                           dialAndWrite(connector.get(), "peer.test", 80, "hello", &wrote)));

    CHECK(wrote);
    CHECK(served);

    auto const callers = resolver.offloaded();
    INFO("the resolver offloaded " << callers << " lookup(s)");
    CHECK(callers == 1);

    // `blockOn` drives the loop on THIS thread, so this thread is the loop's.
    auto const loopThread = std::this_thread::get_id();
    REQUIRE(inner.callers().size() == 1);
    CHECK(inner.callers().front() != loopThread); // the lookup ran somewhere else
    REQUIRE(recorder.resumedOn().size() == 1);
    CHECK(recorder.resumedOn().front() == loopThread); // and handed the flow back to the loop
}

TEST_CASE("a dial to a literal costs no resolver thread at all", "[net]")
{
    // Load-bearing rather than an optimisation: every internal dial in the consuming projects is
    // to a literal, and it is also what lets the whole connect path be exercised with no thread
    // existing.
    auto source = core::net::makeBackend(core::net::preferredBackendKind());
    REQUIRE(source != nullptr);
    auto loop = EventLoop { *source };

    auto bound = core::net::listen(loop, core::net::ListenOptions { .host = "127.0.0.1" });
    REQUIRE(bound.has_value());
    auto listener = std::move(*bound);

    auto inner = RecordingResolver { listener->boundPort() };
    auto resolver = core::net::ThreadedAddressResolver { inner };
    auto connector = core::net::makeConnector(loop, resolver);

    auto served = false;
    auto wrote = false;
    loop.blockOn(core::net::testing::allOf(
        acceptAndRead(listener.get(), "hello", &served),
        dialAndWrite(connector.get(), "127.0.0.1", listener->boundPort(), "hello", &wrote)));

    CHECK(wrote);
    CHECK(served);
    CHECK(resolver.offloaded() == 0);
    // It still went through the injected resolver — inline, on this thread — rather than around it.
    REQUIRE(inner.callers().size() == 1);
    CHECK(inner.callers().front() == std::this_thread::get_id());
}

TEST_CASE("the free connect() reaches a listener on every backend", "[net]")
{
    // contour's `connect(loop, host, port)` keeps its signature and its behaviour; what changed
    // underneath is that it is now `makeConnector` plus the process resolver.
    for (auto const& backend: BackendMatrix)
    {
        auto source = core::net::makeBackend(backend.kind);
        if (!source)
            continue;

        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto loop = EventLoop { *source };
            auto bound = core::net::listen(loop, core::net::ListenOptions { .host = "127.0.0.1" });
            REQUIRE(bound.has_value());
            auto listener = std::move(*bound);

            auto served = false;
            auto client = [](EventLoop* lp, std::uint16_t port, bool* ok) -> Task<void> {
                auto dialled = co_await core::net::connect(lp, "127.0.0.1", port);
                if (!dialled.has_value())
                    co_return;
                constexpr auto Payload = std::string_view { "hello" };
                auto const bytes =
                    std::span<std::byte const> { reinterpret_cast<std::byte const*>(Payload.data()),
                                                 Payload.size() };
                auto const written = co_await (*dialled)->write(bytes);
                *ok = written.has_value() && *written == Payload.size();
            };
            auto wrote = false;
            loop.blockOn(core::net::testing::allOf(acceptAndRead(listener.get(), "hello", &served),
                                                   client(&loop, listener->boundPort(), &wrote)));
            CHECK(wrote);
            CHECK(served);
        }
    }
}

TEST_CASE("the free connect() reports a refusal on every backend", "[net]")
{
    // Ruling R101 through the public surface: a dial checks `SO_ERROR` and never trusts which
    // callback fired, so a closed port produces `ConnRefused` and not a socket whose first write
    // fails.
    for (auto const& backend: BackendMatrix)
    {
        auto source = core::net::makeBackend(backend.kind);
        if (!source)
            continue;

        DYNAMIC_SECTION("backend=" << backend.name)
        {
            auto loop = EventLoop { *source };
            auto closedPort = std::uint16_t { 0 };
            {
                auto bound = core::net::listen(loop, core::net::ListenOptions { .host = "127.0.0.1" });
                REQUIRE(bound.has_value());
                closedPort = (*bound)->boundPort();
            }
            REQUIRE(closedPort != 0);

            auto answer = SocketResult {};
            auto client = [](EventLoop* lp, std::uint16_t port, SocketResult* out) -> Task<void> {
                *out = co_await core::net::connect(lp, "127.0.0.1", port);
            };
            loop.blockOn(client(&loop, closedPort, &answer));

            REQUIRE_FALSE(answer.has_value());
            INFO("dialled the just-closed port " << closedPort << ": " << answer.error().context);
            CHECK(answer.error().code == NetErrorCode::ConnRefused);
        }
    }
}

TEST_CASE("a connector honours the per-call budget", "[net]")
{
    // The budget is a parameter rather than a policy: the OS default connect timeout runs to
    // minutes on some systems, and a lookup against a dead nameserver to about 30 s, so a caller
    // that wants to notice a dead peer — or simply to shut down — cannot be made to wait for them.
    //
    // **Through `makeConnector`, end to end**, because that is the wiring nothing else covers:
    // `ConnectFlow_test` calls the flow directly and hands it a clock itself, so a connector that
    // passed no clock — which disables every budget, silently — would pass every case there.
    //
    // The lookup is held for the whole case, so the ONLY thing that can end this dial is the
    // budget. Driven a turn at a time on a `ManualClock`: a regression is a red CHECK, not a hang.
    auto clock = core::platform::ManualClock {};
    auto loop = core::net::testing::TestLoop { clock };
    auto inner = HeldResolver {};
    auto resolver = core::net::ThreadedAddressResolver { inner, { .threads = 1 } };
    auto const release = core::net::detail::ScopeGuard { [&]() noexcept {
        inner.release();
        resolver.stop();
    } };
    auto connector = core::net::makeConnector(loop, resolver);

    auto answer = SocketResult {};
    auto done = false;
    loop.spawn(dialInto(connector.get(),
                        DialOptions { .connectTimeout = std::chrono::milliseconds { 2000 } },
                        &answer,
                        &done));
    loop.drain();
    inner.awaitEntered();

    clock.advance(std::chrono::milliseconds { 1999 });
    loop.drain();
    CHECK_FALSE(done);

    clock.advance(std::chrono::milliseconds { 1 });
    loop.drain();
    CHECK(done);
    if (done)
    {
        REQUIRE_FALSE(answer.has_value());
        CHECK(answer.error().code == NetErrorCode::Timeout);
    }

    loop.requestStop(); // a dial the budget failed to end is still parked; unwind it
    loop.drain();
}

TEST_CASE("whenAny(connect, delay) ends a dial parked on a lookup when the delay wins", "[net]")
{
    // The composition a caller reaches for when a dial has no budget of its own, and the one that
    // used to wait for the lookup anyway: a race resumes its caller only once EVERY child has
    // finished, so a loser that cannot be cancelled holds the winner's answer for as long as the
    // nameserver holds the lookup. The loser here is the dial, parked on a lookup that is held for
    // the whole case.
    auto clock = core::platform::ManualClock {};
    auto loop = core::net::testing::TestLoop { clock };
    auto inner = HeldResolver {};
    auto resolver = core::net::ThreadedAddressResolver { inner, { .threads = 1 } };
    auto const release = core::net::detail::ScopeGuard { [&]() noexcept {
        inner.release();
        resolver.stop();
    } };
    auto connector = core::net::makeConnector(loop, resolver);

    auto answer = SocketResult {};
    auto dialled = false;
    auto winner = std::optional<std::size_t> {};
    auto raced = false;
    auto race = [](EventLoop* lp,
                   core::platform::SteadyTimePoint at,
                   core::net::IConnector* c,
                   SocketResult* out,
                   bool* dialDone,
                   std::optional<std::size_t>* won,
                   bool* finished) -> Task<void> {
        auto arms = std::vector<Task<void>> {};
        arms.push_back(dialInto(c, DialOptions {}, out, dialDone));
        arms.push_back(sleepTo(lp, at));
        *won = co_await core::async::whenAny(std::move(arms));
        *finished = true;
    };
    loop.spawn(race(&loop,
                    clock.now() + std::chrono::seconds { 2 },
                    connector.get(),
                    &answer,
                    &dialled,
                    &winner,
                    &raced));
    loop.drain();
    inner.awaitEntered();

    clock.advance(std::chrono::seconds { 2 });
    loop.drain();

    CHECK(raced);
    CHECK(winner == std::optional<std::size_t> { 1 });
    CHECK_FALSE(dialled); // the dial was cancelled, not answered

    loop.requestStop(); // a race the stop failed to end is still parked; unwind it
    loop.drain();
}

TEST_CASE("the free connect() sends a NAME to the process resolver's pool", "[net]")
{
    // The task's named requirement, through the entry point contour actually calls. Every other
    // free-`connect()` case dials a literal, which never reaches the pool, so if the free
    // `connect` were wired to an inline resolver (the old stall under a new name) none of them
    // would notice. `localhost` is a name answered from the hosts file: no nameserver is involved
    // and the case does not depend on the network.
    auto source = core::net::makeBackend(core::net::preferredBackendKind());
    REQUIRE(source != nullptr);
    auto loop = EventLoop { *source };

    auto bound = core::net::listen(loop, core::net::ListenOptions { .host = "127.0.0.1" });
    REQUIRE(bound.has_value());
    auto listener = std::move(*bound);

    auto const offloadedBefore = core::net::defaultAsyncResolver().offloaded();

    // `localhost` may answer `::1` first, which this IPv4 listener refuses, and then `127.0.0.1`,
    // which it takes. Nothing here reads from the connection, so no accept is needed for the dial
    // to complete: the backlog holds it.
    auto answer = SocketResult {};
    auto client = [](EventLoop* lp, std::uint16_t port, SocketResult* out) -> Task<void> {
        *out = co_await core::net::connect(lp, "localhost", port);
    };
    loop.blockOn(client(&loop, listener->boundPort(), &answer));

    CHECK(core::net::defaultAsyncResolver().offloaded() == offloadedBefore + 1);
    if (!answer.has_value())
        WARN("the dial to localhost did not connect (" << answer.error().context
                                                       << "); the offload above is what this case asserts");
}

TEST_CASE("a listener reports the port it actually bound", "[net]")
{
    // A bind to port 0 means "pick a free one", so the port an operator, a log line or a test
    // needs is the one the kernel chose and not the one that was asked for. `boundPort`, where
    // contour spelled it `localPort`.
    auto source = core::net::makeBackend(core::net::preferredBackendKind());
    REQUIRE(source != nullptr);
    auto loop = EventLoop { *source };

    auto bound = core::net::listen(loop, core::net::ListenOptions { .host = "127.0.0.1", .port = 0 });
    REQUIRE(bound.has_value());
    CHECK((*bound)->boundPort() != 0);
}
