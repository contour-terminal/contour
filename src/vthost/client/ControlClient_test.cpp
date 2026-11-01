// SPDX-License-Identifier: Apache-2.0
#include <vtpty/MockPty.hpp>

#include <core/Utf8Path.hpp>
#include <core/async/StopToken.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/Sockets.hpp>
#include <core/net/testing/CoroTestSupport.hpp>
#include <core/net/testing/InMemoryTransport.hpp>
#include <core/platform/Clock.hpp>
#include <core/testing/Environment.hpp>
#include <core/testing/ScopedTempDir.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <iterator>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <vtcontrol/Api.hpp>
#include <vtcontrol/Events.hpp>
#include <vtcontrol/Json.hpp>
#include <vthost/NativeSession.hpp>
#include <vthost/SessionHost.hpp>
#include <vthost/SocketPath.hpp>
#include <vthost/Transport.hpp>
#include <vthost/client/ControlClient.hpp>
#include <vthost/control/ControlService.hpp>

using core::async::Task;
using vthost::client::ControlClient;
using vthost::client::ControlClientError;
using vthost::client::ControlFailure;
namespace proto = vthost::proto;

namespace
{

/// A daemon's control API over an in-process socket pair: a real NativeSession and ControlService on one
/// end, a ControlClient on the other, both on one loop.
struct ControlClientHarness
{
    /// @param handshake What the daemon's handshake admits; it offers the control API unless told otherwise.
    explicit ControlClientHarness(vthost::HandshakePolicy handshake =
                                      vthost::HandshakePolicy { .capabilities = proto::CapabilitySet::of(
                                                                    { proto::Capability::ControlApi }) }):
        session { std::make_unique<vthost::NativeSession>(
            loop,
            host,
            vthost::ConnectionId { .endpoint = "test", .index = 1 },
            std::move(pair.first),
            vthost::NativeSession::DefaultWriteQueueBytes,
            vthost::NativeSessionServices { .handshake = std::move(handshake), .control = control }) }
    {
    }

    std::unique_ptr<core::net::IoBackend> backend = core::net::makeDefaultBackend();
    core::net::EventLoop loop { *backend };
    vthost::SessionHost host { loop,
                               [](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const&)
                                   -> std::unique_ptr<vtpty::Pty> {
                                   return std::make_unique<vtpty::MockPty>(size);
                               },
                               vtbackend::Settings {},
                               core::defaultEnvironment(),
                               vthost::SessionHostConfig { .pumping = vthost::SessionPumping::Manual } };
    core::platform::ManualWallClock wall {};
    vthost::control::ControlService control {
        host,
        vthost::control::ControlServiceConfig {
            .version = "test", .socketPath = {}, .clock = wall, .maxReplyBytes = 65536 }
    };
    core::net::testing::SocketPair pair = *core::net::testing::makeSocketPair(loop);
    std::unique_ptr<vthost::NativeSession> session;
    ControlClient client { loop,
                           std::make_unique<vthost::Connection>(vthost::Connection {
                               .lifetime = {}, .socket = std::move(pair.second), .token = {} }) };

    /// Runs the daemon's side and @p script together until both are done; the script ends the connection.
    void run(Task<void> script)
    {
        loop.blockOn(core::net::testing::allOf(session->run(), std::move(script)));
    }
};

/// Handshakes, then calls @p method once.
Task<void> callOnce(ControlClientHarness* h,
                    std::string method,
                    std::string params,
                    std::expected<std::string, ControlClientError>* out)
{
    auto const handshaken = co_await h->client.handshake();
    REQUIRE(handshaken);
    *out = co_await h->client.call(std::move(method), std::move(params), {});
    h->client.close();
}

/// Handshakes and stores the outcome.
Task<void> handshakeOnly(ControlClientHarness* h, std::expected<void, ControlClientError>* out)
{
    *out = co_await h->client.handshake();
    h->client.close();
}

/// Reads one frame off @p socket.
Task<std::optional<proto::DecodedFrame>> readOne(core::net::ISocket* socket, std::vector<std::byte>* buffer)
{
    while (true)
    {
        if (auto decoded = proto::decodePdu(*buffer))
        {
            buffer->erase(buffer->begin(), buffer->begin() + static_cast<std::ptrdiff_t>(decoded->consumed));
            co_return std::move(*decoded);
        }
        auto const appended = co_await core::net::appendReadChunk(socket, buffer);
        if (!appended || *appended == 0)
            co_return std::nullopt;
    }
}

/// A daemon of a release that predates the control API: it accepts the hello without sharing it, then
/// records whatever else the client sends until it hangs up.
Task<void> daemonWithoutControlApi(core::net::ISocket* socket, std::vector<proto::DecodedFrame>* received)
{
    auto buffer = std::vector<std::byte> {};
    auto hello = co_await readOne(socket, &buffer);
    REQUIRE(hello);
    received->push_back(*hello);
    auto writer = proto::Writer {};
    proto::encodePdu(writer,
                     hello->serial,
                     proto::DecodedPdu { proto::ServerHello {
                         .agreedVersion = proto::SupportedVersions.max,
                         .versions = proto::SupportedVersions,
                         .capabilities = proto::CapabilitySet::of({ proto::Capability::Heartbeat }),
                         .hostname = "devbox" } });
    auto const bytes = writer.view();
    std::ignore = co_await socket->write(std::span { bytes.data(), bytes.size() });
    while (auto frame = co_await readOne(socket, &buffer))
        received->push_back(*frame);
}

/// A transport that only counts its attempts.
class CountingTransport final: public vthost::ITransportFactory
{
  public:
    explicit CountingTransport(vthost::PeerTrust trust): _trust(trust) {}

    [[nodiscard]] Task<std::expected<std::unique_ptr<vthost::Connection>, vthost::TransportError>> connect(
        core::net::EventLoop& /*loop*/) override
    {
        ++connects;
        return refuse();
    }
    [[nodiscard]] std::string describe() const override { return "tls://h:7297"; }
    [[nodiscard]] vthost::PeerTrust peerTrust() const noexcept override { return _trust; }

    int connects = 0; ///< How many attempts were made.

  private:
    static Task<std::expected<std::unique_ptr<vthost::Connection>, vthost::TransportError>> refuse()
    {
        co_return std::unexpected(vthost::TransportError { .kind = vthost::TransportErrorKind::Unreachable,
                                                           .message = "nothing here" });
    }

    vthost::PeerTrust _trust;
};

} // namespace

TEST_CASE("ControlClient calls a method", "[vthost][client][control]")
{
    auto h = ControlClientHarness {};
    std::ignore = h.host.createTab();
    auto reply = std::expected<std::string, ControlClientError> {};
    h.run(callOnce(&h, "workspace.list", "{}", &reply));
    REQUIRE(reply);
    auto const list = vtcontrol::decodeJson<vtcontrol::ListReply>(*reply);
    REQUIRE(list);
    REQUIRE(list->windows.size() == 1);
    CHECK(list->windows[0].tabs.size() == 1);
}

TEST_CASE("a remote error surfaces as ControlFailure::Remote", "[vthost][client][control]")
{
    auto h = ControlClientHarness {};
    std::ignore = h.host.createTab();
    auto reply = std::expected<std::string, ControlClientError> {};
    h.run(callOnce(&h, "pane.capture", R"({"pane":"pane:nope"})", &reply));
    REQUIRE_FALSE(reply);
    CHECK(reply.error().failure == ControlFailure::Remote);
    REQUIRE(reply.error().remote);
    CHECK(reply.error().remote->code == vtcontrol::ControlErrorCode::NotFound);
    CHECK(reply.error().message == reply.error().remote->message);
}

TEST_CASE("a daemon without the control API is named, not waited on", "[vthost][client][control]")
{
    // Review focus 5.
    auto backend = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *backend };
    auto pair = *core::net::testing::makeSocketPair(loop);
    auto client = ControlClient { loop,
                                  std::make_unique<vthost::Connection>(vthost::Connection {
                                      .lifetime = {}, .socket = std::move(pair.second), .token = {} }) };
    auto received = std::vector<proto::DecodedFrame> {};
    auto outcome = std::expected<void, ControlClientError> {};
    auto const script = [](ControlClient* c, std::expected<void, ControlClientError>* out) -> Task<void> {
        *out = co_await c->handshake();
    };
    loop.blockOn(core::net::testing::allOf(daemonWithoutControlApi(pair.first.get(), &received),
                                           script(&client, &outcome)));
    REQUIRE_FALSE(outcome);
    CHECK(outcome.error().failure == ControlFailure::Unsupported);
    CHECK(outcome.error().message.contains("does not offer the control API"));
    CHECK(outcome.error().message.contains("devbox"));
    // The client sent nothing after its ClientHello, and hung up.
    REQUIRE(received.size() == 1);
    CHECK(std::holds_alternative<proto::ClientHello>(received[0].pdu));
}

TEST_CASE("a version refusal is reported verbatim", "[vthost][client][control]")
{
    auto const theirs = proto::VersionRange { .min = proto::SupportedVersions.max + 1,
                                              .max = proto::SupportedVersions.max + 1 };
    auto h = ControlClientHarness { vthost::HandshakePolicy {
        .versions = theirs, .capabilities = proto::CapabilitySet::of({ proto::Capability::ControlApi }) } };
    auto outcome = std::expected<void, ControlClientError> {};
    h.run(handshakeOnly(&h, &outcome));
    REQUIRE_FALSE(outcome);
    CHECK(outcome.error().failure == ControlFailure::Refused);
    CHECK(outcome.error().message
          == proto::describe(proto::HandshakeRefusal { .ours = proto::SupportedVersions, .theirs = theirs },
                             proto::PeerRole::Daemon));
}

TEST_CASE("a wrong token is Refused", "[vthost][client][control]")
{
    auto h = ControlClientHarness { vthost::HandshakePolicy {
        .acceptedTokens = { { .token = "right" } },
        .capabilities = proto::CapabilitySet::of({ proto::Capability::ControlApi }) } };
    auto outcome = std::expected<void, ControlClientError> {};
    h.run(handshakeOnly(&h, &outcome));
    REQUIRE_FALSE(outcome);
    CHECK(outcome.error().failure == ControlFailure::Refused);
}

namespace
{
/// Subscribes to bells, rings one, and reads the event that follows.
Task<void> subscribeAndRing(ControlClientHarness* h,
                            vtworkspace::SessionId session,
                            std::expected<std::string, ControlClientError>* out)
{
    auto const handshaken = co_await h->client.handshake();
    REQUIRE(handshaken);
    auto const subscribed = co_await h->client.call("events.subscribe", R"({"kinds":["pane.bell"]})", {});
    REQUIRE(subscribed);
    h->host.terminal(session)->writeToScreen("\a");
    *out = co_await h->client.nextEvent();
    h->client.close();
}
} // namespace

TEST_CASE("nextEvent yields subscribed events", "[vthost][client][control]")
{
    auto h = ControlClientHarness {};
    auto* tab = h.host.createTab();
    REQUIRE(tab != nullptr);
    auto event = std::expected<std::string, ControlClientError> {};
    h.run(subscribeAndRing(&h, tab->rootPane()->session(), &event));
    REQUIRE(event);
    CHECK(event->contains("pane.bell"));
}

TEST_CASE("resolveControlEndpoint precedence", "[vthost][client][control]")
{
    auto const describe = [](vthost::client::ControlEndpointOptions const& options,
                             core::Environment const& env) {
        auto const transport = vthost::client::resolveControlEndpoint(options, env);
        REQUIRE(transport);
        return (*transport)->describe();
    };
    auto const withDaemon = core::testing::FakeEnvironment { { { "CONTOUR_DAEMON", "/run/env.sock" } } };
    auto const empty = core::testing::FakeEnvironment {};

    CHECK(describe({ .daemon = "/tmp/x" }, withDaemon) == "unix:/tmp/x");
    CHECK(describe({}, withDaemon) == "unix:/run/env.sock");
    CHECK(describe({}, empty)
          == std::format("unix:{}", core::pathToUtf8(vthost::muxSocketPath({}, {}, empty))));
    CHECK(describe({ .daemon = "h:7297" }, empty) == "tls://h:7297");
    // A path with a colon in it is still a path.
    CHECK(describe({ .daemon = "/run/user/1000/x:1" }, empty) == "unix:/run/user/1000/x:1");
    CHECK(describe({ .daemon = "./x:1" }, empty) == "unix:./x:1");

    SECTION("a TCP endpoint carries the CA, and so is verified")
    {
        auto const pinned = vthost::client::resolveControlEndpoint(
            { .daemon = "h:7297", .token = "t", .tlsCaPem = "-----BEGIN CERTIFICATE-----" }, empty);
        REQUIRE(pinned);
        CHECK((*pinned)->peerTrust() == vthost::PeerTrust::Verified);
        auto const unpinned = vthost::client::resolveControlEndpoint({ .daemon = "h:7297" }, empty);
        REQUIRE(unpinned);
        CHECK((*unpinned)->peerTrust() == vthost::PeerTrust::Unverified);
    }
    SECTION("saved hosts go to the resolver, whose failure is Unreachable")
    {
        auto asked = std::vector<std::string> {};
        auto const options = vthost::client::ControlEndpointOptions {
            .daemon = "",
            .token = {},
            .tlsCaPem = {},
            .resolveSavedHost = [&](std::string_view name)
                -> std::expected<std::shared_ptr<vthost::ITransportFactory>, std::string> {
                asked.emplace_back(name);
                return std::unexpected(std::format("no saved host '{}'", name));
            },
        };
        for (auto const* spec: { "devbox", "ssh://devbox" })
        {
            auto named = options;
            named.daemon = spec;
            auto const resolved = vthost::client::resolveControlEndpoint(named, empty);
            REQUIRE_FALSE(resolved);
            CHECK(resolved.error().failure == ControlFailure::Unreachable);
            CHECK(resolved.error().message.contains(spec));
        }
        CHECK(asked == std::vector<std::string> { "devbox", "ssh://devbox" });
        // A path is a path, resolver or not.
        auto path = options;
        path.daemon = "/tmp/x";
        CHECK(describe(path, empty) == "unix:/tmp/x");
        // Without a resolver a bare word is a path too.
        CHECK(describe({ .daemon = "devbox" }, empty) == "unix:devbox");
    }
}

TEST_CASE("an unverified TLS endpoint is refused before connecting", "[vthost][client][control]")
{
    // Review focus 6.
    auto transport = CountingTransport { vthost::PeerTrust::Unverified };
    auto const reply = vthost::client::runControlCall(transport, "workspace.list", "{}", {});
    REQUIRE_FALSE(reply);
    CHECK(reply.error().failure == ControlFailure::Untrusted);
    CHECK(reply.error().message.contains("--tls-ca"));
    CHECK(reply.error().message.contains("tls://h:7297"));
    CHECK(transport.connects == 0);

    auto const stream = vthost::client::runEventStream(
        transport, "{}", {}, [](std::string_view) { return vthost::client::StreamStep::Stop; }, {});
    REQUIRE_FALSE(stream);
    CHECK(stream.error().failure == ControlFailure::Untrusted);
    CHECK(transport.connects == 0);

    SECTION("a verified one is connected")
    {
        auto verified = CountingTransport { vthost::PeerTrust::Verified };
        auto const attempted = vthost::client::runControlCall(verified, "workspace.list", "{}", {});
        REQUIRE_FALSE(attempted);
        CHECK(attempted.error().failure == ControlFailure::Unreachable);
        CHECK(verified.connects == 1);
    }
}

TEST_CASE("an unreachable socket is Unreachable", "[vthost][client][control]")
{
    auto const dir = core::testing::ScopedTempDir { "control-client" };
    auto transport = vthost::UnixTransportFactory { dir.path() / "nobody", {} };
    auto const reply =
        vthost::client::runControlCall(transport, "workspace.list", "{}", {}, std::chrono::seconds(5));
    REQUIRE_FALSE(reply);
    CHECK(reply.error().failure == ControlFailure::Unreachable);
    CHECK_FALSE(reply.error().message.empty());
}

namespace
{
/// @return @p pdu framed under @p serial.
std::vector<std::byte> framed(uint64_t serial, proto::DecodedPdu const& pdu)
{
    auto writer = proto::Writer {};
    proto::encodePdu(writer, serial, pdu);
    return { writer.view().begin(), writer.view().end() };
}

/// What a streamingDaemon does once its events are sent.
enum class AfterEvents : uint8_t
{
    Wait = 0, ///< Reads on until the client hangs up.
    HangUp,   ///< Closes the connection.
};

/// A daemon that serves one subscription: it accepts the hello sharing the control API, answers the
/// subscribe, sends @p events bell events, then does what @p after says.
Task<void> streamingDaemon(std::unique_ptr<core::net::ISocket> socket, int events, AfterEvents after)
{
    auto buffer = std::vector<std::byte> {};
    auto const hello = co_await readOne(socket.get(), &buffer);
    REQUIRE(hello);
    auto const accepted = framed(
        hello->serial,
        proto::ServerHello { .agreedVersion = proto::SupportedVersions.max,
                             .versions = proto::SupportedVersions,
                             .capabilities = proto::CapabilitySet::of({ proto::Capability::ControlApi }) });
    std::ignore = co_await socket->write(accepted);
    auto const subscribe = co_await readOne(socket.get(), &buffer);
    REQUIRE(subscribe);
    auto const& request = std::get<proto::ControlRequest>(subscribe->pdu);
    CHECK(request.method == "events.subscribe");
    auto bytes = framed(subscribe->serial,
                        proto::ControlReply { .requestId = request.requestId,
                                              .outcome = proto::ControlOutcome::Ok,
                                              .body = R"({"subscription":1})" });
    for ([[maybe_unused]] auto const index: std::views::iota(0, events))
        std::ranges::copy(
            framed(0,
                   proto::ControlEvent { .subscription = 1,
                                         .event = vtcontrol::encodeEvent(vtcontrol::Event {
                                             .origin = {}, .payload = vtcontrol::BellEvent {} }) }),
            std::back_inserter(bytes));
    std::ignore = co_await socket->write(bytes);
    if (after == AfterEvents::HangUp)
    {
        socket->close();
        co_return;
    }
    while (co_await readOne(socket.get(), &buffer))
        ;
}

/// A local transport whose every connection is served by a streamingDaemon on the caller's loop.
class StreamingTransport final: public vthost::ITransportFactory
{
  public:
    StreamingTransport(int events, AfterEvents after = AfterEvents::Wait): _events(events), _after(after) {}

    [[nodiscard]] Task<std::expected<std::unique_ptr<vthost::Connection>, vthost::TransportError>> connect(
        core::net::EventLoop& loop) override
    {
        auto pair = *core::net::testing::makeSocketPair(loop);
        loop.spawn(streamingDaemon(std::move(pair.first), _events, _after));
        return settled(std::make_unique<vthost::Connection>(
            vthost::Connection { .lifetime = {}, .socket = std::move(pair.second), .token = {} }));
    }
    [[nodiscard]] std::string describe() const override { return "streaming:"; }
    [[nodiscard]] vthost::PeerTrust peerTrust() const noexcept override { return vthost::PeerTrust::Local; }

  private:
    static Task<std::expected<std::unique_ptr<vthost::Connection>, vthost::TransportError>> settled(
        std::unique_ptr<vthost::Connection> connection)
    {
        co_return std::move(connection);
    }

    int _events;
    AfterEvents _after;
};
} // namespace

TEST_CASE("runEventStream hands events over until told to stop", "[vthost][client][control]")
{
    SECTION("by the callback")
    {
        auto transport = StreamingTransport { 3 };
        auto seen = 0;
        auto const ended =
            vthost::client::runEventStream(transport,
                                           R"({"kinds":["pane.bell"]})",
                                           {},
                                           [&](std::string_view event) {
                                               CHECK(event.contains("pane.bell"));
                                               return ++seen == 2 ? vthost::client::StreamStep::Stop
                                                                  : vthost::client::StreamStep::Continue;
                                           },
                                           {});
        CHECK(ended);
        CHECK(seen == 2);
    }
    SECTION("by the stop token, while waiting for an event")
    {
        auto transport = StreamingTransport { 1 };
        auto stop = core::async::StopSource {};
        auto seen = 0;
        auto const ended = vthost::client::runEventStream(
            transport,
            "{}",
            {},
            [&](std::string_view) {
                ++seen;
                stop.request_stop(); // the stream then waits for an event that never comes, until the stop
                return vthost::client::StreamStep::Continue;
            },
            stop.get_token());
        CHECK(ended);
        CHECK(seen == 1);
    }
    SECTION("by a stop requested before it starts, before any event")
    {
        auto transport = StreamingTransport { 3 };
        auto stop = core::async::StopSource {};
        stop.request_stop();
        auto seen = 0;
        auto const ended = vthost::client::runEventStream(
            transport,
            "{}",
            {},
            [&](std::string_view) {
                ++seen;
                return vthost::client::StreamStep::Continue;
            },
            stop.get_token());
        CHECK(ended);
        CHECK(seen == 0);
    }
    SECTION("a daemon that hangs up is Disconnected")
    {
        auto transport = StreamingTransport { 1, AfterEvents::HangUp };
        auto seen = 0;
        auto const ended = vthost::client::runEventStream(transport,
                                                          "{}",
                                                          {},
                                                          [&](std::string_view) {
                                                              ++seen;
                                                              return vthost::client::StreamStep::Continue;
                                                          },
                                                          {});
        REQUIRE_FALSE(ended);
        CHECK(ended.error().failure == ControlFailure::Disconnected);
        CHECK(seen == 1);
    }
}
