// SPDX-License-Identifier: Apache-2.0
//
// The blocking half of the dial seam. What matters is not that a successful dial works -- a listener
// two lines away proves that -- but that every way it can FAIL comes back as a distinguishable,
// bounded outcome, and that the task never suspends, because `syncRun` is how its callers drive it.
//
// Origin: fastcached `src/FastCache/Net/BlockingConnector_test.cpp`
// (`0708dd54dc7ee72622c8c0783c2bd4a06f0e9b21`). Its listeners were `BlockingListener`s; here they
// are this platform's loop listeners, which a blocking dial reaches identically.
#include <core/async/SyncRun.hpp>
#include <core/net/BlockingConnector.hpp>
#include <core/net/IListener.hpp>
#include <core/net/PlatformLoop.hpp>
#include <core/net/SocketAddress.hpp>
#include <core/net/Sockets.hpp>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
using core::async::syncRun;
using core::net::BlockingConnector;
using core::net::DialOptions;
using core::net::IAddressResolver;
using core::net::NetErrorCode;
using core::net::ResolvedEndpoint;

namespace
{

/// A resolver that answers with whatever it was told to, so a test can produce the "resolution
/// failed" branch -- which no real hostname reliably reproduces -- and a dead first candidate.
class ScriptedResolver final: public IAddressResolver
{
  public:
    explicit ScriptedResolver(IAddressResolver& inner) noexcept: _inner { inner } {}

    void failWith(std::string reason) { _failure = std::move(reason); }

    /// Prepends a candidate to be tried before the real ones.
    void prependCandidate(std::string_view host, std::uint16_t port)
    {
        auto const extra = _inner.resolve(host, port);
        if (extra.has_value() && !extra->empty())
            _prefix.push_back(extra->front());
    }

    [[nodiscard]] std::expected<std::vector<ResolvedEndpoint>, std::string> resolve(
        std::string_view host, std::uint16_t port) override
    {
        if (!_failure.empty())
            return std::unexpected { _failure };
        auto resolved = _inner.resolve(host, port);
        if (!resolved.has_value())
            return resolved;
        auto out = _prefix;
        out.insert(out.end(), resolved->begin(), resolved->end());
        return out;
    }

  private:
    IAddressResolver& _inner;
    std::string _failure;
    std::vector<ResolvedEndpoint> _prefix;
};

/// A loopback listener on an ephemeral port, and the loop it belongs to.
struct Listening
{
    core::net::PlatformLoop loop;
    std::unique_ptr<core::net::IListener> listener;
};

/// @return The port a listener had, after closing it: nothing listens there now.
[[nodiscard]] std::uint16_t closedPort()
{
    auto loop = core::net::PlatformLoop {};
    auto listener = core::net::listen(loop, "127.0.0.1", 0);
    REQUIRE(listener.has_value());
    auto const port = (*listener)->boundPort();
    (*listener)->close();
    return port;
}

} // namespace

TEST_CASE("A blocking connector reaches a listener that is up, without suspending",
          "[net][connector][blocking]")
{
    auto up = Listening();
    auto listener = core::net::listen(up.loop, "127.0.0.1", 0);
    REQUIRE(listener.has_value());
    up.listener = std::move(*listener);

    auto connector = BlockingConnector {};
    // `syncRun` throws if the task is left suspended, so reaching the REQUIRE is the "never
    // suspends" half of the contract.
    auto const socket = syncRun(
        connector.connect("127.0.0.1", up.listener->boundPort(), DialOptions { .connectTimeout = 2s }));
    INFO("dial: " << (socket.has_value() ? std::string { "connected" } : socket.error().toString()));
    REQUIRE(socket.has_value());
    CHECK(*socket != nullptr);
    CHECK_FALSE((*socket)->isClosed());
    CHECK((*socket)->peerAddress().contains("127.0.0.1"));
}

TEST_CASE("A blocking dial that cannot succeed fails within its budget, and says why",
          "[net][connector][blocking]")
{
    // The property the non-blocking dial exists for. What the failure is CALLED is deliberately not
    // pinned: a closed loopback port answers with a reset on a bare host and is silently dropped
    // behind a host firewall -- Windows drops it -- so the same dial is ConnRefused on one machine
    // and Timeout on the next. What must hold on both is that it fails, quickly, and says which.
    auto const port = closedPort();
    constexpr auto Budget = 300ms;
    auto connector = BlockingConnector {};

    auto const started = std::chrono::steady_clock::now();
    auto const socket =
        syncRun(connector.connect("127.0.0.1", port, DialOptions { .connectTimeout = Budget }));
    auto const elapsed = std::chrono::steady_clock::now() - started;

    REQUIRE_FALSE(socket.has_value());
    CAPTURE(socket.error().toString());
    CHECK(elapsed < Budget * 20); // "it returned rather than parking", not a latency measurement
    auto const code = socket.error().code;
    CHECK((code == NetErrorCode::ConnRefused || code == NetErrorCode::Timeout));
    CHECK_FALSE(socket.error().context.empty());
}

TEST_CASE("A blocking dial reports a resolution failure without dialling, naming host and port",
          "[net][connector][blocking]")
{
    auto resolver = ScriptedResolver { core::net::defaultAddressResolver() };
    resolver.failWith("scripted resolution failure");

    auto connector = BlockingConnector { resolver };
    auto const socket = syncRun(connector.connect("some-host", 6674, DialOptions { .connectTimeout = 1s }));
    REQUIRE_FALSE(socket.has_value());
    CAPTURE(socket.error().toString());
    // The reason and the target travel with the refusal: a bare code leaves an operator unable to
    // tell a typo in a peer address from a DNS outage.
    CHECK(socket.error().context.contains("scripted resolution failure"));
    CHECK(socket.error().context.contains("some-host"));
    CHECK(socket.error().context.contains("6674"));
}

TEST_CASE("A dead first candidate does not condemn the host, on the blocking dial",
          "[net][connector][blocking]")
{
    // A name that resolves to several addresses -- an AAAA on a machine with no IPv6 route is the
    // ordinary case -- must be reached through whichever one works, or a healthy peer is reported
    // down for a reason that is about this machine.
    auto up = Listening();
    auto listener = core::net::listen(up.loop, "127.0.0.1", 0);
    REQUIRE(listener.has_value());
    up.listener = std::move(*listener);

    auto resolver = ScriptedResolver { core::net::defaultAddressResolver() };
    resolver.prependCandidate("127.0.0.1", closedPort());

    auto connector = BlockingConnector { resolver };
    auto const socket = syncRun(
        connector.connect("127.0.0.1", up.listener->boundPort(), DialOptions { .connectTimeout = 2s }));
    INFO("dial: " << (socket.has_value() ? std::string { "connected" } : socket.error().toString()));
    REQUIRE(socket.has_value());
    CHECK(*socket != nullptr);
}
