// SPDX-License-Identifier: Apache-2.0
#include <vtpty/MockPty.hpp>

#include <core/Utf8Path.hpp>
#include <core/async/StopToken.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/Sockets.hpp>
#include <core/net/WithTimeout.hpp>
#include <core/net/testing/CoroTestSupport.hpp>
#include <core/net/testing/InMemoryTransport.hpp>
#include <core/platform/Clock.hpp>
#include <core/platform/testing/InMemoryFileSystem.hpp>
#include <core/testing/Environment.hpp>
#include <core/testing/ScopedTempDir.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
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
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <vtcontrol/Api.hpp>
#include <vtcontrol/Events.hpp>
#include <vtcontrol/Json.hpp>
#include <vtcontrol/LegacySpelling.hpp>
#include <vthost/KeyAuth.hpp>
#include <vthost/KnownDaemons.hpp>
#include <vthost/NativeSession.hpp>
#include <vthost/SessionHost.hpp>
#include <vthost/SocketPath.hpp>
#include <vthost/Transport.hpp>
#include <vthost/client/ControlClient.hpp>
#include <vthost/control/ControlService.hpp>
#include <vthost/proto/ControlVersions.hpp>
#include <vthost/testing/CountingRandom.hpp>

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
                                      vthost::HandshakePolicy {
                                          .capabilities = proto::CapabilitySet::of(
                                              { proto::Capability::ControlApi, proto::Capability::Names }) }):
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

/// Writes @p pdu to @p socket under @p serial.
Task<void> sendPdu(core::net::ISocket* socket, uint64_t serial, proto::DecodedPdu pdu)
{
    auto writer = proto::Writer {};
    proto::encodePdu(writer, serial, pdu);
    auto const bytes = writer.view();
    std::ignore = co_await socket->write(std::span { bytes.data(), bytes.size() });
}

/// compat(0.9.0): a daemon of a release before the `names` capability: it shares the control API, answers
/// every ControlRequest with @p reply, an event in the old spelling first, and records what it was sent.
Task<void> daemonWithoutNames(core::net::ISocket* socket,
                              std::string reply,
                              std::vector<proto::DecodedFrame>* received)
{
    auto buffer = std::vector<std::byte> {};
    auto hello = co_await readOne(socket, &buffer);
    REQUIRE(hello);
    co_await sendPdu(socket,
                     hello->serial,
                     proto::DecodedPdu { proto::ServerHello {
                         .agreedVersion = proto::SupportedVersions.max,
                         .versions = proto::SupportedVersions,
                         .capabilities = proto::CapabilitySet::of({ proto::Capability::ControlApi }),
                         .hostname = "devbox" } });
    while (auto frame = co_await readOne(socket, &buffer))
    {
        received->push_back(*frame);
        auto const* request = std::get_if<proto::ControlRequest>(&frame->pdu);
        if (request == nullptr)
            continue;
        co_await sendPdu(
            socket,
            0,
            proto::DecodedPdu { proto::ControlEvent {
                .subscription = 1,
                .event =
                    R"({"type":"workspace.labelChanged","workspace":"workspace:018f3c4e-9a1b-7c2d-8e3f-0123456789ab","tab":"","pane":"","payload":{"label":"ops"}})" } });
        co_await sendPdu(socket,
                         frame->serial,
                         proto::DecodedPdu { proto::ControlReply { .requestId = request->requestId,
                                                                   .outcome = proto::ControlOutcome::Ok,
                                                                   .body = reply } });
    }
}

/// A transport whose every attempt fails the same way, and that counts them.
class CountingTransport final: public vthost::ITransportFactory
{
  public:
    explicit CountingTransport(vthost::TransportErrorKind failure): _failure(failure) {}

    [[nodiscard]] Task<std::expected<std::unique_ptr<vthost::Connection>, vthost::TransportError>> connect(
        core::net::EventLoop& /*loop*/) override
    {
        ++connects;
        return refuse(_failure);
    }
    [[nodiscard]] std::string describe() const override { return "tls://h:7297"; }

    int connects = 0; ///< How many attempts were made.

  private:
    static Task<std::expected<std::unique_ptr<vthost::Connection>, vthost::TransportError>> refuse(
        vthost::TransportErrorKind failure)
    {
        co_return std::unexpected(vthost::TransportError { .kind = failure, .message = "nothing here" });
    }

    vthost::TransportErrorKind _failure;
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
    REQUIRE(list->workspaces.size() == 1);
    CHECK(list->workspaces[0].tabs.size() == 1);
}

namespace
{
/// Handshakes, calls @p method once, then takes the event that arrived meanwhile.
Task<void> callAgainstOldDaemon(ControlClient* client,
                                std::string method,
                                std::string params,
                                std::expected<std::string, ControlClientError>* reply,
                                std::expected<std::string, ControlClientError>* event,
                                std::optional<vtcontrol::Spelling>* spelling)
{
    auto const handshaken = co_await client->handshake();
    REQUIRE(handshaken);
    *spelling = client->spelling();
    *reply = co_await client->call(std::move(method), std::move(params), {});
    *event = co_await client->nextEvent();
    client->close();
}

struct OldDaemonExchange
{
    std::vector<proto::DecodedFrame> received;
    std::expected<std::string, ControlClientError> reply;
    std::expected<std::string, ControlClientError> event;
    std::optional<vtcontrol::Spelling> spelling;
};

/// Runs one call against daemonWithoutNames(), which answers it with @p reply.
OldDaemonExchange callOldDaemon(std::string method, std::string params, std::string reply)
{
    auto backend = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *backend };
    auto pair = *core::net::testing::makeSocketPair(loop);
    auto client = ControlClient { loop,
                                  std::make_unique<vthost::Connection>(vthost::Connection {
                                      .lifetime = {}, .socket = std::move(pair.second), .token = {} }) };
    auto exchange = OldDaemonExchange {};
    loop.blockOn(
        core::net::testing::allOf(daemonWithoutNames(pair.first.get(), std::move(reply), &exchange.received),
                                  callAgainstOldDaemon(&client,
                                                       std::move(method),
                                                       std::move(params),
                                                       &exchange.reply,
                                                       &exchange.event,
                                                       &exchange.spelling)));
    return exchange;
}
} // namespace

TEST_CASE("a daemon without the names capability is spoken to in the old spelling",
          "[vthost][client][control][compat-0.9.0]")
{
    auto const exchange = callOldDaemon("object.rename", R"({"object":"pane:logs","name":"tail"})", "{}");
    CHECK(exchange.spelling == vtcontrol::Spelling::Legacy);
    auto const request = std::ranges::find_if(exchange.received, [](proto::DecodedFrame const& frame) {
        return std::holds_alternative<proto::ControlRequest>(frame.pdu);
    });
    REQUIRE(request != exchange.received.end());
    auto const& sent = std::get<proto::ControlRequest>(request->pdu);
    CHECK(sent.method == "object.setLabel");
    CHECK(sent.params == R"({"object":"pane:logs","label":"tail"})");
    CHECK(exchange.reply == std::string { "{}" });
    // Its event reaches the caller in the new spelling, the workspace it names its object.
    REQUIRE(exchange.event);
    auto const event = vtcontrol::decodeEvent(*exchange.event);
    REQUIRE(event);
    auto const* renamed = std::get_if<vtcontrol::ObjectRenamedEvent>(&event->payload);
    REQUIRE(renamed != nullptr);
    CHECK(renamed->object == "workspace:018f3c4e-9a1b-7c2d-8e3f-0123456789ab");
    CHECK(renamed->name == "ops");
}

TEST_CASE("an old daemon's listing reaches the caller with name keys",
          "[vthost][client][control][compat-0.9.0]")
{
    auto const exchange = callOldDaemon(
        "workspace.list",
        "{}",
        R"({"workspaces":[{"id":"workspace:018f3c4e-9a1b-7c2d-8e3f-0123456789ab","label":"ops","position":1,"tabs":[]}]})");
    REQUIRE(exchange.reply);
    auto const list = vtcontrol::decodeJson<vtcontrol::ListReply>(*exchange.reply);
    REQUIRE(list);
    REQUIRE(list->workspaces.size() == 1);
    CHECK(list->workspaces[0].name == "ops");
}

TEST_CASE("a daemon with the names capability is spoken to as it is", "[vthost][client][control]")
{
    auto h = ControlClientHarness {};
    std::ignore = h.host.createTab();
    auto spelling = std::optional<vtcontrol::Spelling> {};
    h.run([](ControlClientHarness* h, std::optional<vtcontrol::Spelling>* out) -> Task<void> {
        auto const handshaken = co_await h->client.handshake();
        REQUIRE(handshaken);
        *out = h->client.spelling();
        h->client.close();
    }(&h, &spelling));
    CHECK(spelling == vtcontrol::Spelling::Names);
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
          == proto::describe(proto::HandshakeRefusal { .ours = proto::ControlVersions, .theirs = theirs },
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
    auto const files = core::platform::testing::InMemoryFileSystem {};
    auto random = vthost::testing::CountingRandom {};
    auto known = vthost::KnownDaemons { files, "/state/known_daemons", random };
    auto const describe = [&](vthost::client::ControlEndpointOptions options, core::Environment const& env) {
        options.knownDaemons = &known;
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

    SECTION("a daemon's name picks the local daemon of that name, over the environment")
    {
        auto const named = [&](std::string_view name) {
            return std::format(
                "unix:{}",
                core::pathToUtf8(vthost::muxSocketPath(vthost::daemonNameFromOption(name), {}, empty)));
        };
        auto const work =
            vthost::client::LocalDaemons { .contains = [](std::string_view name) { return name == "work"; } };
        CHECK(describe({ .daemon = "name:work" }, empty) == named("work"));
        CHECK(describe({ .daemon = "name:work" }, withDaemon) == named("work"));
        CHECK(describe({ .daemon = "work", .localDaemons = work }, empty) == named("work"));
        CHECK(describe({ .daemon = "name:work" }, empty) != describe({}, empty));
    }
    SECTION("a word naming both a local daemon and a saved host is refused, naming both spellings")
    {
        auto const everything = [](std::string_view) {
            return true;
        };
        auto const resolved =
            vthost::client::resolveControlEndpoint({ .daemon = "work",
                                                     .localDaemons = { .contains = everything },
                                                     .savedHosts = { .contains = everything } },
                                                   empty);
        REQUIRE_FALSE(resolved);
        CHECK(resolved.error().failure == ControlFailure::Misconfigured);
        CHECK(resolved.error().message.contains("--daemon name:work"));
        CHECK(resolved.error().message.contains("--daemon host:work"));
    }
    SECTION("a TCP endpoint carries the CA, and so is verified")
    {
        auto const pinned =
            vthost::client::resolveControlEndpoint({ .daemon = "h:7297",
                                                     .token = "t",
                                                     .tlsCaPem = "-----BEGIN CERTIFICATE-----",
                                                     .knownDaemons = &known },
                                                   empty);
        REQUIRE(pinned);
        auto const unpinned =
            vthost::client::resolveControlEndpoint({ .daemon = "h:7297", .knownDaemons = &known }, empty);
        REQUIRE(unpinned);
        // Told the daemon's fingerprint, or having pinned it, a daemon is verified too.
        auto const told = vthost::client::resolveControlEndpoint(
            { .daemon = "h:7297",
              .daemonFingerprint = "SHA256:47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU",
              .knownDaemons = &known },
            empty);
        REQUIRE(told);
        // Without a file to keep pins in, a HOST:PORT is refused outright.
        auto const nowhere = vthost::client::resolveControlEndpoint({ .daemon = "h:7297" }, empty);
        REQUIRE_FALSE(nowhere);
        CHECK(nowhere.error().failure == vthost::client::ControlFailure::Misconfigured);
    }
    SECTION("saved hosts go to the resolver, whose failure is Unreachable")
    {
        auto asked = std::vector<std::string> {};
        auto const options = vthost::client::ControlEndpointOptions {
            .daemon = "",
            .token = {},
            .tlsCaPem = {},
            .resolveSavedHost = [&](std::string_view name, std::string_view /*daemonFingerprint*/)
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

namespace
{
/// A transport whose connect takes a while (the user is being asked something) and then fails.
class SlowConnectTransport final: public vthost::ITransportFactory
{
  public:
    [[nodiscard]] Task<std::expected<std::unique_ptr<vthost::Connection>, vthost::TransportError>> connect(
        core::net::EventLoop& loop) override
    {
        return slowly(&loop);
    }
    [[nodiscard]] std::string describe() const override { return "tls://h:7297"; }

  private:
    static Task<std::expected<std::unique_ptr<vthost::Connection>, vthost::TransportError>> slowly(
        core::net::EventLoop* loop)
    {
        co_await loop->delay(std::chrono::milliseconds { 300 });
        co_return std::unexpected(vthost::TransportError { .kind = vthost::TransportErrorKind::Unreachable,
                                                           .message = "nobody there" });
    }
};
} // namespace

TEST_CASE("a call's timeout does not run while the transport connects", "[vthost][client][control]")
{
    // The connect may be waiting for the user to trust a daemon: a clock through it would end the call under
    // their hand. The timeout bounds what the daemon does once connected.
    auto transport = SlowConnectTransport {};
    auto const reply = vthost::client::runControlCall(
        transport, "workspace.list", "{}", {}, std::chrono::milliseconds { 50 });
    REQUIRE_FALSE(reply);
    CHECK(reply.error().failure == ControlFailure::Unreachable);
    CHECK(reply.error().message == "nobody there");
}

TEST_CASE("a daemon the transport does not trust is Untrusted, and a declined question is Declined",
          "[vthost][client][control]")
{
    struct Row
    {
        vthost::TransportErrorKind kind;
        ControlFailure failure;
    };
    // The trust is settled by the connect, which may ask the user: the prediction gates nothing.
    constexpr auto Rows = std::array {
        Row { .kind = vthost::TransportErrorKind::HostKeyUnknown, .failure = ControlFailure::Untrusted },
        Row { .kind = vthost::TransportErrorKind::HostKeyChanged, .failure = ControlFailure::Untrusted },
        Row { .kind = vthost::TransportErrorKind::Cancelled, .failure = ControlFailure::Declined },
    };
    for (auto const& row: Rows)
    {
        auto transport = CountingTransport { row.kind };
        auto const reply = vthost::client::runControlCall(transport, "workspace.list", "{}", {});
        REQUIRE_FALSE(reply);
        CHECK(reply.error().failure == row.failure);
        CHECK(reply.error().message == "nothing here");
        CHECK(transport.connects == 1);

        auto const stream = vthost::client::runEventStream(
            transport, "{}", {}, [](std::string_view) { return vthost::client::StreamStep::Stop; }, {});
        REQUIRE_FALSE(stream);
        CHECK(stream.error().failure == row.failure);
        CHECK(transport.connects == 2);
    }

    SECTION("any other failure is Unreachable")
    {
        auto transport = CountingTransport { vthost::TransportErrorKind::Unreachable };
        auto const attempted = vthost::client::runControlCall(transport, "workspace.list", "{}", {});
        REQUIRE_FALSE(attempted);
        CHECK(attempted.error().failure == ControlFailure::Unreachable);
        CHECK(transport.connects == 1);
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

namespace
{
/// A daemon that answers the hello and then the call, each only after @p wait.
Task<void> slowDaemon(core::net::EventLoop* loop,
                      std::unique_ptr<core::net::ISocket> socket,
                      std::chrono::milliseconds wait)
{
    auto buffer = std::vector<std::byte> {};
    auto const hello = co_await readOne(socket.get(), &buffer);
    if (!hello)
        co_return;
    co_await loop->delay(wait); // the client is unanswered meanwhile, and sends nothing
    std::ignore = co_await socket->write(framed(
        hello->serial,
        proto::ServerHello { .agreedVersion = proto::SupportedVersions.max,
                             .versions = proto::SupportedVersions,
                             .capabilities = proto::CapabilitySet::of({ proto::Capability::ControlApi }) }));
    auto const call = co_await readOne(socket.get(), &buffer);
    if (!call)
        co_return;
    co_await loop->delay(wait);
    auto const& request = std::get<proto::ControlRequest>(call->pdu);
    std::ignore = co_await socket->write(framed(call->serial,
                                                proto::ControlReply { .requestId = request.requestId,
                                                                      .outcome = proto::ControlOutcome::Ok,
                                                                      .body = "{}" }));
    while (co_await readOne(socket.get(), &buffer))
        ;
}

/// A local transport whose every connection is served by a slowDaemon on the caller's loop.
class SlowDaemonTransport final: public vthost::ITransportFactory
{
  public:
    explicit SlowDaemonTransport(std::chrono::milliseconds wait): _wait(wait) {}

    [[nodiscard]] Task<std::expected<std::unique_ptr<vthost::Connection>, vthost::TransportError>> connect(
        core::net::EventLoop& loop) override
    {
        auto pair = *core::net::testing::makeSocketPair(loop);
        loop.spawn(slowDaemon(&loop, std::move(pair.first), _wait));
        return settled(std::make_unique<vthost::Connection>(
            vthost::Connection { .lifetime = {}, .socket = std::move(pair.second), .token = {} }));
    }
    [[nodiscard]] std::string describe() const override { return "slow:"; }

  private:
    static Task<std::expected<std::unique_ptr<vthost::Connection>, vthost::TransportError>> settled(
        std::unique_ptr<vthost::Connection> connection)
    {
        co_return std::move(connection);
    }

    std::chrono::milliseconds _wait;
};
} // namespace

TEST_CASE("a call's timeout is one deadline for the handshake and the call together",
          "[vthost][client][control]")
{
    using namespace std::chrono_literals;
    // Each phase takes 70 ms, under the 100 ms given: a bound per phase would let this succeed, and the
    // two together take 140 ms.
    auto slow = SlowDaemonTransport { 70ms };
    auto const reply = vthost::client::runControlCall(slow, "workspace.list", "{}", {}, 100ms);
    REQUIRE_FALSE(reply);
    CHECK(reply.error().failure == ControlFailure::TimedOut);

    // Given time for both, the same daemon answers.
    auto patient = SlowDaemonTransport { 70ms };
    auto const answered = vthost::client::runControlCall(patient, "workspace.list", "{}", {}, 5s);
    REQUIRE(answered);
    CHECK(*answered == "{}");
}

namespace
{
/// A daemon that challenges first, then answers the hello and the call at once.
Task<void> challengingDaemon(std::unique_ptr<core::net::ISocket> socket)
{
    auto buffer = std::vector<std::byte> {};
    std::ignore = co_await socket->write(framed(1, proto::AuthChallenge { .nonce = {}, .methods = 1 }));
    auto const hello = co_await readOne(socket.get(), &buffer);
    if (!hello)
        co_return;
    std::ignore = co_await socket->write(framed(
        hello->serial,
        proto::ServerHello { .agreedVersion = proto::SupportedVersions.max,
                             .versions = proto::SupportedVersions,
                             .capabilities = proto::CapabilitySet::of({ proto::Capability::ControlApi }) }));
    auto const call = co_await readOne(socket.get(), &buffer);
    if (!call)
        co_return;
    auto const& request = std::get<proto::ControlRequest>(call->pdu);
    std::ignore = co_await socket->write(framed(call->serial,
                                                proto::ControlReply { .requestId = request.requestId,
                                                                      .outcome = proto::ControlOutcome::Ok,
                                                                      .body = "{}" }));
    while (co_await readOne(socket.get(), &buffer))
        ;
}

/// A key source that takes @c wait to list its keys, as an agent asking the user to confirm does, and then
/// has none.
class SlowKeySource final: public vthost::client::IClientKeySource
{
  public:
    SlowKeySource(core::net::EventLoop& loop, std::chrono::milliseconds wait): _loop(loop), _wait(wait) {}

    Task<std::expected<std::vector<core::net::SshPublicKey>, std::string>> candidates() override
    {
        co_await _loop.delay(_wait);
        co_return std::vector<core::net::SshPublicKey> {};
    }

    Task<std::expected<std::string, std::string>> sign(core::net::SshPublicKey const& /*key*/,
                                                       std::span<std::byte const> /*message*/) override
    {
        return refuse();
    }

  private:
    static Task<std::expected<std::string, std::string>> refuse()
    {
        co_return std::unexpected(std::string { "no key" });
    }

    core::net::EventLoop& _loop;
    std::chrono::milliseconds _wait;
};

/// A local transport whose every connection is challenged by a challengingDaemon, with a key source that
/// takes @c wait.
class ChallengingTransport final: public vthost::ITransportFactory
{
  public:
    explicit ChallengingTransport(std::chrono::milliseconds wait): _wait(wait) {}

    [[nodiscard]] Task<std::expected<std::unique_ptr<vthost::Connection>, vthost::TransportError>> connect(
        core::net::EventLoop& loop) override
    {
        auto pair = *core::net::testing::makeSocketPair(loop);
        loop.spawn(challengingDaemon(std::move(pair.first)));
        return settled(std::make_unique<vthost::Connection>(
            vthost::Connection { .lifetime = {},
                                 .socket = std::move(pair.second),
                                 .token = {},
                                 .keySource = std::make_shared<SlowKeySource>(loop, _wait) }));
    }
    [[nodiscard]] std::string describe() const override { return "challenging:"; }

  private:
    static Task<std::expected<std::unique_ptr<vthost::Connection>, vthost::TransportError>> settled(
        std::unique_ptr<vthost::Connection> connection)
    {
        co_return std::move(connection);
    }

    std::chrono::milliseconds _wait;
};
} // namespace

TEST_CASE("a call's timeout does not run while a challenge is being signed", "[vthost][client][control]")
{
    // The key takes 300 ms (a passphrase typed, an agent's confirmation), the call is given 100 ms: the
    // daemon itself answers at once, and the time is the user's.
    using namespace std::chrono_literals;
    auto transport = ChallengingTransport { 300ms };
    auto const reply = vthost::client::runControlCall(transport, "workspace.list", "{}", {}, 100ms);
    REQUIRE(reply);
    CHECK(*reply == "{}");
}

TEST_CASE("a call whose challenge is never signed fails within its bound", "[vthost][client][control]")
{
    // A signer that never answers (a pinentry on another terminal, a dead forwarded agent): the signing time
    // is excused up to the daemon's own pre-authentication deadline, and no further.
    using namespace std::chrono_literals;
    auto transport = ChallengingTransport { std::chrono::hours { 1 } };
    auto const started = std::chrono::steady_clock::now();
    auto const reply = vthost::client::runControlCall(transport, "workspace.list", "{}", {}, 100ms);
    auto const took = std::chrono::steady_clock::now() - started;
    REQUIRE_FALSE(reply);
    CHECK(reply.error().failure == ControlFailure::TimedOut);
    CHECK(took >= vthost::PreAuthTimeout);
    CHECK(took < vthost::PreAuthTimeout + 5s);
}

TEST_CASE("an untimed call whose challenge is never signed fails at the daemon's deadline",
          "[vthost][client][control]")
{
    // No timeout of the caller's own: the daemon's pre-authentication deadline is the bound all the same.
    using namespace std::chrono_literals;
    auto transport = ChallengingTransport { std::chrono::hours { 1 } };
    auto const started = std::chrono::steady_clock::now();
    auto const reply = vthost::client::runControlCall(transport, "workspace.list", "{}", {}, std::nullopt);
    auto const took = std::chrono::steady_clock::now() - started;
    REQUIRE_FALSE(reply);
    CHECK(reply.error().failure == ControlFailure::TimedOut);
    CHECK(took >= vthost::PreAuthTimeout);
    CHECK(took < vthost::PreAuthTimeout + 5s);
}

TEST_CASE("a stop given from another thread ends a call at once", "[vthost][client][control]")
{
    // The listing of a window or a launch whose owner goes away: the call ends, wherever it waits.
    using namespace std::chrono_literals;
    auto transport = ChallengingTransport { std::chrono::hours { 1 } };
    auto stop = core::async::StopSource {};
    auto const stopper = std::jthread { [&stop] {
        std::this_thread::sleep_for(100ms);
        stop.request_stop();
    } };
    auto const started = std::chrono::steady_clock::now();
    auto const reply =
        vthost::client::runControlCall(transport, "workspace.list", "{}", {}, std::nullopt, stop.get_token());
    REQUIRE_FALSE(reply);
    CHECK(reply.error().failure == ControlFailure::Declined);
    CHECK(std::chrono::steady_clock::now() - started < 5s);
}

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
