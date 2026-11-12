// SPDX-License-Identifier: Apache-2.0
#include <core/Utf8Path.hpp>
#include <core/log/LogSink.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/Sockets.hpp>
#include <core/net/Tls.hpp>
#include <core/net/WithTimeout.hpp>
#include <core/net/testing/CoroTestSupport.hpp>
#include <core/net/testing/InMemoryTransport.hpp>
#include <core/net/testing/SocketDecorator.hpp>
#include <core/platform/NativeFileSystem.hpp>
#include <core/platform/testing/InMemoryFileSystem.hpp>
#include <core/testing/ScopedTempDir.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <expected>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <ranges>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#ifndef _WIN32
    #include <unistd.h>
#endif

#include <vthost/DaemonIdentity.hpp>
#include <vthost/KnownDaemons.hpp>
#include <vthost/SocketPath.hpp>
#include <vthost/Transport.hpp>
#include <vthost/testing/CountingRandom.hpp>
#include <vthost/testing/RecordingDurableFiles.hpp>
#include <vthost/testing/TlsDaemon.hpp>

using vthost::retryOf;
using vthost::TransportErrorKind;
using vthost::TransportRetry;

namespace
{

/// A socket that records its own destruction.
class RecordingSocket final: public core::net::testing::SocketDecorator
{
  public:
    RecordingSocket(core::net::ISocket& inner, std::vector<std::string>& events):
        SocketDecorator(inner), _events(events)
    {
    }
    ~RecordingSocket() override { _events.emplace_back("socket"); }

    RecordingSocket(RecordingSocket const&) = delete;
    RecordingSocket& operator=(RecordingSocket const&) = delete;
    RecordingSocket(RecordingSocket&&) = delete;
    RecordingSocket& operator=(RecordingSocket&&) = delete;

  private:
    std::vector<std::string>& _events;
};

} // namespace

TEST_CASE("only unreachable and protocol errors are retried", "[vthost][transport]")
{
    static_assert(retryOf(TransportErrorKind::Unreachable) == TransportRetry::Transient);
    static_assert(retryOf(TransportErrorKind::Protocol) == TransportRetry::Transient);
    static_assert(retryOf(TransportErrorKind::HostKeyUnknown) == TransportRetry::Permanent);
    static_assert(retryOf(TransportErrorKind::Refused) == TransportRetry::Permanent);
    static_assert(retryOf(TransportErrorKind::AuthenticationRequired) == TransportRetry::Permanent);
    static_assert(retryOf(TransportErrorKind::Cancelled) == TransportRetry::Permanent);
    // A daemon that went away comes back (C2): an absent one is retried too.
    static_assert(retryOf(TransportErrorKind::DaemonAbsent) == TransportRetry::Transient);
    static_assert(retryOf(TransportErrorKind::Misconfigured) == TransportRetry::Permanent);
    static_assert(retryOf(TransportErrorKind::HostKeyChanged) == TransportRetry::Permanent);
}

TEST_CASE("a connection releases its lifetime only after its socket", "[vthost][transport]")
{
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto pair = *core::net::testing::makeSocketPair(loop);
    auto events = std::vector<std::string> {};
    {
        auto const connection = vthost::Connection {
            .lifetime =
                std::shared_ptr<void> { nullptr, [&events](void*) { events.emplace_back("lifetime"); } },
            .socket = std::make_unique<RecordingSocket>(*pair.second, events),
            .token = {},
        };
    }
    CHECK(events == std::vector<std::string> { "socket", "lifetime" });
}

// [afunix]: skips where AF_UNIX is missing. @see the tag's explanation in net/Socket_test.cpp.
TEST_CASE("the unix factory ensures the daemon before every attempt", "[vthost][transport][afunix]")
{
    auto const tmp = core::testing::ScopedTempDir { "contour-transport" };
    auto const controlSocket = tmp.path() / "sockets" / "control";
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    // Stands in for a serving daemon: reaching the listen backlog is a connected socket.
    auto listener = core::net::listenUnix(loop, core::pathToUtf8(vthost::nativeSocketPath(controlSocket)));
    if (!listener.has_value())
    {
        INFO("listenUnix failed: " << listener.error().toString());
        REQUIRE(listener.error().code == core::net::NetErrorCode::Unsupported);
        SKIP("AF_UNIX not supported on this platform");
    }

    auto ensured = 0;
    auto factory = vthost::UnixTransportFactory { controlSocket, [&ensured] {
                                                     ++ensured;
                                                     return std::expected<void, vthost::TransportError> {};
                                                 } };
    for ([[maybe_unused]] auto const attempt: { 1, 2 })
    {
        auto connection = loop.blockOn(factory.connect(loop));
        INFO((connection ? std::string {} : connection.error().message));
        REQUIRE(connection.has_value());
        CHECK((*connection)->token.empty());
    }
    CHECK(ensured == 2);
    CHECK(factory.describe().starts_with("unix:"));
}

TEST_CASE("the unix factory does not connect when the daemon cannot be ensured", "[vthost][transport]")
{
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto factory = vthost::UnixTransportFactory {
        "/nonexistent/contour/control",
        [] {
            return std::expected<void, vthost::TransportError> { std::unexpected { vthost::TransportError {
                .kind = TransportErrorKind::ProgramNotFound, .message = "no contour binary" } } };
        }
    };
    auto const connection = loop.blockOn(factory.connect(loop));
    REQUIRE_FALSE(connection.has_value());
    CHECK(connection.error().kind == TransportErrorKind::ProgramNotFound);
    CHECK(connection.error().message == "no contour binary");
}

TEST_CASE("the unix factory contains a daemon start that throws a non-std type", "[vthost][transport]")
{
    // An exception leaving the worker thread would be std::terminate.
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto factory =
        vthost::UnixTransportFactory { "/nonexistent/contour/control",
                                       []() -> std::expected<void, vthost::TransportError> { throw 42; } };
    auto const connection = loop.blockOn(factory.connect(loop));
    REQUIRE_FALSE(connection.has_value());
    CHECK(connection.error().kind == TransportErrorKind::Unreachable);
    CHECK(connection.error().message == "the daemon start threw");
}

TEST_CASE("the unix factory starts the daemon off the loop's thread", "[vthost][transport][threading]")
{
    // The start may wait seconds for a spawned daemon; the loop it would block is a client's reactor.
    using namespace std::chrono_literals;
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto const loopThread = std::this_thread::get_id();
    auto ensuredOn = std::thread::id {};
    auto loopTurned = false;
    auto turned = std::promise<void> {};
    auto factory = vthost::UnixTransportFactory {
        "/nonexistent/contour/control",
        [&] {
            ensuredOn = std::this_thread::get_id();
            // Blocks until the loop runs what it is posted, which it could not if this ran on it.
            loop.post([&turned] { turned.set_value(); });
            loopTurned = turned.get_future().wait_for(5s) == std::future_status::ready;
            return std::expected<void, vthost::TransportError> { std::unexpected { vthost::TransportError {
                .kind = TransportErrorKind::ProgramNotFound, .message = "no contour binary" } } };
        }
    };
    auto const connection = loop.blockOn(factory.connect(loop));

    CHECK(ensuredOn != loopThread);
    CHECK(loopTurned);
    REQUIRE_FALSE(connection.has_value());
    CHECK(connection.error().kind == TransportErrorKind::ProgramNotFound);
    CHECK(connection.error().message == "no contour binary");
}

TEST_CASE("a unix socket nobody serves reads as an absent daemon", "[vthost][transport][afunix]")
{
    auto const tmp = core::testing::ScopedTempDir { "contour-transport" };
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto factory = vthost::UnixTransportFactory { tmp.path() / "nobody", nullptr };
    auto const connection = loop.blockOn(factory.connect(loop));
    REQUIRE_FALSE(connection.has_value());
    CHECK(connection.error().kind == TransportErrorKind::DaemonAbsent);
}

using vthost::testing::TlsDaemon;

namespace
{

/// An IPinPrompt that answers as scripted and counts how often it was asked.
class ScriptedPinPrompt final: public vthost::IPinPrompt
{
  public:
    explicit ScriptedPinPrompt(vthost::PinDecision decision): _decision(decision) {}

    core::async::Task<vthost::PinDecision> confirm(std::string hostPort, std::string fingerprint) override
    {
        asked.emplace_back(std::move(hostPort), std::move(fingerprint));
        co_return _decision;
    }

    std::vector<std::pair<std::string, std::string>> asked; ///< Every question, as host:port and fingerprint.

  private:
    vthost::PinDecision _decision;
};

using ConnectOutcome = std::expected<std::unique_ptr<vthost::Connection>, vthost::TransportError>;

core::async::Task<void> connectInto(vthost::ITransportFactory* factory,
                                    core::net::EventLoop* loop,
                                    ConnectOutcome* out)
{
    *out = co_await factory->connect(*loop);
}

} // namespace

TEST_CASE("a CA the TLS factory cannot use is not retried", "[vthost][transport]")
{
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto const files = core::platform::testing::InMemoryFileSystem {};
    auto random = vthost::testing::CountingRandom {};
    auto known = vthost::KnownDaemons { files, "/state/known_daemons", random };
    auto listener = core::net::listen(loop, "127.0.0.1", 0);
    REQUIRE(listener.has_value());
    auto factory = vthost::TlsTransportFactory { vthost::TcpEndpoint { .host = "127.0.0.1",
                                                                       .port = (*listener)->boundPort(),
                                                                       .token = {},
                                                                       .caPem = "not a certificate" },
                                                 known,
                                                 nullptr,
                                                 {} };
    auto const connection = loop.blockOn(factory.connect(loop));
    REQUIRE_FALSE(connection.has_value());
    CHECK(connection.error().kind == TransportErrorKind::Misconfigured);
    CHECK(retryOf(connection.error().kind) == TransportRetry::Permanent);
}

TEST_CASE("the TLS factory names its endpoint", "[vthost][transport]")
{
    auto const files = core::platform::testing::InMemoryFileSystem {};
    auto random = vthost::testing::CountingRandom {};
    auto known = vthost::KnownDaemons { files, "/state/known_daemons", random };
    auto const factory = vthost::TlsTransportFactory {
        vthost::TcpEndpoint { .host = "127.0.0.1", .port = 4711, .token = "secret", .caPem = {} },
        known,
        nullptr,
        {}
    };
    CHECK(factory.describe() == "tls://127.0.0.1:4711");
}

TEST_CASE("a daemon met for the first time is pinned on the user's word, and not asked about again",
          "[vthost][transport][pin]")
{
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto daemon = TlsDaemon { loop };
    auto const files = core::platform::testing::InMemoryFileSystem {};
    auto random = vthost::testing::CountingRandom {};
    auto known = vthost::KnownDaemons { files, "/state/known_daemons", random };
    auto const prompt = std::make_shared<ScriptedPinPrompt>(vthost::PinDecision::Trust);
    auto factory = vthost::TlsTransportFactory { daemon.endpoint(), known, prompt, {} };

    auto first = ConnectOutcome { std::unexpected(vthost::TransportError {}) };
    auto second = ConnectOutcome { std::unexpected(vthost::TransportError {}) };
    loop.blockOn(core::net::testing::allOf(
        daemon.serve(&loop, 3), connectInto(&factory, &loop, &first), connectInto(&factory, &loop, &second)));

    REQUIRE(first);
    CHECK((*first)->token == "tok");
    REQUIRE(second);
    // Asked once, with what the daemon itself prints; pinned as that.
    REQUIRE(prompt->asked.size() == 1);
    CHECK(prompt->asked.front() == std::pair { daemon.hostPort(), daemon.fingerprint() });
    CHECK(known.pinOf(daemon.hostPort()) == daemon.fingerprint());
}

namespace
{

/// A pin prompt whose user takes @c delay to answer yes, on the loop's clock.
class SlowPinPrompt final: public vthost::IPinPrompt
{
  public:
    SlowPinPrompt(core::net::EventLoop& loop, std::chrono::milliseconds delay): _loop(&loop), _delay(delay) {}

    core::async::Task<vthost::PinDecision> confirm(std::string /*hostPort*/,
                                                   std::string /*fingerprint*/) override
    {
        ++asked;
        co_await _loop->delay(_delay);
        co_return vthost::PinDecision::Trust;
    }

    int asked = 0;

  private:
    core::net::EventLoop* _loop;
    std::chrono::milliseconds _delay;
};

/// What an impatient daemon did: it closes a connection that has not sent anything within its patience, as
/// the daemon's pre-authentication deadline does.
struct ImpatientOutcome
{
    int closedForSilence = 0;       ///< Connections it closed for want of a hello.
    std::size_t bytesOnTheLast = 0; ///< What the last connection delivered before it ended.
};

/// Accepts one connection, closes it after @p patience, then accepts another and reads what it carries.
core::async::Task<void> serveImpatiently(TlsDaemon* daemon,
                                         core::net::EventLoop* loop,
                                         std::chrono::milliseconds patience,
                                         ImpatientOutcome* outcome)
{
    auto accepted = co_await daemon->listener->accept();
    if (!accepted)
        co_return;
    auto first = daemon->context->wrap(std::move(*accepted), *loop);
    std::ignore = co_await first->handshakeIfNeeded();
    co_await loop->delay(patience);
    first->close();
    ++outcome->closedForSilence;

    accepted = co_await daemon->listener->accept();
    if (!accepted)
        co_return;
    auto second = daemon->context->wrap(std::move(*accepted), *loop);
    std::ignore = co_await second->handshakeIfNeeded();
    auto buffer = std::vector<std::byte>(16);
    auto const count = co_await second->read(buffer);
    outcome->bytesOnTheLast = count ? *count : 0;
}

/// Connects, then sends one byte over what it got: only a connection the daemon still holds delivers it.
core::async::Task<void> connectAndSend(vthost::ITransportFactory* factory,
                                       core::net::EventLoop* loop,
                                       ConnectOutcome* out)
{
    *out = co_await factory->connect(*loop);
    if (*out)
    {
        auto const byte = std::array { std::byte { 'x' } };
        std::ignore = co_await (**out)->socket->write(byte);
    }
}

} // namespace

TEST_CASE("a daemon the user trusted is dialled again, once, with the pin in place",
          "[vthost][transport][pin]")
{
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto daemon = TlsDaemon { loop };
    auto const files = core::platform::testing::InMemoryFileSystem {};
    auto random = vthost::testing::CountingRandom {};
    auto known = vthost::KnownDaemons { files, "/state/known_daemons", random };
    auto const prompt = std::make_shared<ScriptedPinPrompt>(vthost::PinDecision::Trust);
    auto factory = vthost::TlsTransportFactory { daemon.endpoint(), known, prompt, {} };
    auto outcome = ConnectOutcome { std::unexpected(vthost::TransportError {}) };

    loop.blockOn(core::net::testing::allOf(daemon.serve(&loop, 2), connectInto(&factory, &loop, &outcome)));

    REQUIRE(outcome);
    // The first connection waited for the answer and is let go; the one handed out is the second.
    CHECK(daemon.open.size() == 2);
    CHECK(prompt->asked.size() == 1);
    CHECK(known.pinOf(daemon.hostPort()) == daemon.fingerprint());
}

TEST_CASE("a trust that cannot be kept is refused, naming the file", "[vthost][transport][pin]")
{
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto daemon = TlsDaemon { loop };
    auto files = vthost::testing::RecordingDurableFiles {};
    files.failing = vthost::testing::DurableOp::Replace; // nothing can be written
    auto random = vthost::testing::CountingRandom {};
    auto known = vthost::KnownDaemons { files, "/state/known_daemons", random };
    auto const prompt = std::make_shared<ScriptedPinPrompt>(vthost::PinDecision::Trust);
    auto factory = vthost::TlsTransportFactory { daemon.endpoint(), known, prompt, {} };
    auto outcome = ConnectOutcome { std::unexpected(vthost::TransportError {}) };
    auto received = std::size_t { 0 };

    loop.blockOn(core::net::testing::allOf(daemon.serveCounting(&loop, &received),
                                           connectInto(&factory, &loop, &outcome)));

    REQUIRE_FALSE(outcome);
    CHECK(outcome.error().kind == TransportErrorKind::Misconfigured);
    CHECK(outcome.error().message.contains("was trusted but cannot be remembered"));
    CHECK(outcome.error().message.contains("/state/known_daemons"));
    CHECK(received == 0);
    CHECK(prompt->asked.size() == 1);
}

namespace
{
/// Connects to @p daemon with its pins kept in @p file, and asks a user who would say yes.
/// @return What came of it, and how often the user was asked.
[[nodiscard]] std::pair<ConnectOutcome, std::size_t> connectWithPinsIn(std::filesystem::path const& file)
{
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto daemon = TlsDaemon { loop };
    auto random = vthost::testing::CountingRandom {};
    auto known = vthost::KnownDaemons { core::platform::NativeFileSystem::instance(), file, random };
    auto const prompt = std::make_shared<ScriptedPinPrompt>(vthost::PinDecision::Trust);
    auto factory = vthost::TlsTransportFactory { daemon.endpoint(), known, prompt, {} };
    auto outcome = ConnectOutcome { std::unexpected(vthost::TransportError {}) };
    auto received = std::size_t { 0 };
    loop.blockOn(core::net::testing::allOf(daemon.serveCounting(&loop, &received),
                                           connectInto(&factory, &loop, &outcome)));
    CHECK(received == 0);
    return { std::move(outcome), prompt->asked.size() };
}
} // namespace

TEST_CASE("a pin store that cannot be read refuses every daemon and asks nobody", "[vthost][transport][pin]")
{
    auto const dir = core::testing::ScopedTempDir { "contour-known-daemons" };
    auto const file = dir.path() / "known_daemons";

    SECTION("a directory where the file should be")
    {
        std::filesystem::create_directory(file);
    }
#ifndef _WIN32
    SECTION("a file this user may not read")
    {
        // Root reads it anyway, and so cannot stand in for a user who may not.
        if (::geteuid() == 0)
            SKIP("root reads a file of mode 000");
        std::ofstream { file } << "studio:7297 SHA256:47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU\n";
        std::filesystem::permissions(
            file, std::filesystem::perms::none, std::filesystem::perm_options::replace);
    }
#endif

    auto const [outcome, asked] = connectWithPinsIn(file);
    REQUIRE_FALSE(outcome);
    CHECK(outcome.error().kind == TransportErrorKind::Misconfigured);
    CHECK(retryOf(outcome.error().kind) == TransportRetry::Permanent);
    CHECK(outcome.error().message.contains(core::pathToUtf8(file)));
    CHECK(asked == 0);
#ifndef _WIN32
    std::filesystem::permissions(
        file, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
#endif
}

TEST_CASE("a fingerprint given beforehand connects though its pin cannot be kept", "[vthost][transport][pin]")
{
    // As ssh connects to a host it was told the key of, warning that it cannot add it to known_hosts.
    auto const capture = core::log::ScopedCapture {};
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto daemon = TlsDaemon { loop };
    auto files = vthost::testing::RecordingDurableFiles {};
    files.failing = vthost::testing::DurableOp::Replace; // nothing can be written
    auto random = vthost::testing::CountingRandom {};
    auto known = vthost::KnownDaemons { files, "/state/known_daemons", random };
    auto endpoint = daemon.endpoint();
    endpoint.expectedFingerprint = daemon.fingerprint();
    auto factory = vthost::TlsTransportFactory { endpoint, known, nullptr, {} };
    auto outcome = ConnectOutcome { std::unexpected(vthost::TransportError {}) };

    loop.blockOn(core::net::testing::allOf(daemon.serve(&loop, 1), connectInto(&factory, &loop, &outcome)));

    REQUIRE(outcome);
    CHECK(known.pinOf(daemon.hostPort()) == std::nullopt);
    CHECK(capture.contains("is not remembered"));
    CHECK(capture.contains("/state/known_daemons"));
}

TEST_CASE("an IPv6 daemon is pinned under its bracketed address", "[vthost][transport][pin]")
{
    auto const files = core::platform::testing::InMemoryFileSystem {};
    auto random = vthost::testing::CountingRandom {};
    auto known = vthost::KnownDaemons { files, "/state/known_daemons", random };
    auto const factory = vthost::TlsTransportFactory {
        vthost::TcpEndpoint { .host = "::1", .port = 7297, .token = {}, .caPem = {} }, known, nullptr, {}
    };
    CHECK(factory.describe() == "tls://[::1]:7297");
    CHECK(vthost::pinKey("::1", 7297) == "[::1]:7297");
    CHECK(vthost::pinKeyOf("[::1]:7297") == "[::1]:7297");
}

TEST_CASE("an answer that comes after the daemon's pre-authentication deadline still connects",
          "[vthost][transport][pin]")
{
    // The daemon gives a connection 150 ms to say something; the user takes 400 ms. The connection that
    // waited is closed by then, and the one handed out is a second, made once the user said yes.
    using namespace std::chrono_literals;
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto daemon = TlsDaemon { loop };
    auto const files = core::platform::testing::InMemoryFileSystem {};
    auto random = vthost::testing::CountingRandom {};
    auto known = vthost::KnownDaemons { files, "/state/known_daemons", random };
    auto const prompt = std::make_shared<SlowPinPrompt>(loop, 400ms);
    auto factory = vthost::TlsTransportFactory { daemon.endpoint(), known, prompt, {} };
    auto outcome = ConnectOutcome { std::unexpected(vthost::TransportError {}) };
    auto daemonSaw = ImpatientOutcome {};

    // Bounded, so that a connect that stopped dialling again reports a failure rather than hanging the suite.
    auto const finished = loop.blockOn(
        core::net::withTimeout(&loop,
                               core::net::testing::allOf(serveImpatiently(&daemon, &loop, 150ms, &daemonSaw),
                                                         connectAndSend(&factory, &loop, &outcome)),
                               10s));
    REQUIRE(finished);

    REQUIRE(outcome);
    CHECK(prompt->asked == 1);
    CHECK(daemonSaw.closedForSilence == 1);
    CHECK(daemonSaw.bytesOnTheLast == 1);
}

namespace
{
/// Accepts two connections on @p daemon's listener, the first with its identity and the second with @p
/// swap's.
core::async::Task<void> serveSwapping(TlsDaemon* daemon, TlsDaemon* swap, core::net::EventLoop* loop)
{
    for (auto* const identity: { daemon, swap })
    {
        auto accepted = co_await daemon->listener->accept();
        if (!accepted)
            co_return;
        auto socket = identity->context->wrap(std::move(*accepted), *loop);
        std::ignore = co_await socket->handshakeIfNeeded();
        daemon->open.push_back(std::move(socket));
    }
}
} // namespace

TEST_CASE("a daemon met again after the user trusted it must be the same one", "[vthost][transport][pin]")
{
    // The daemon that answers the second dial presents another certificate than the one just trusted: that
    // is a changed daemon, which the first answer never covered.
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto daemon = TlsDaemon { loop };
    auto impostor = TlsDaemon { loop };
    auto const files = core::platform::testing::InMemoryFileSystem {};
    auto random = vthost::testing::CountingRandom {};
    auto known = vthost::KnownDaemons { files, "/state/known_daemons", random };
    auto const prompt = std::make_shared<ScriptedPinPrompt>(vthost::PinDecision::Trust);
    auto factory = vthost::TlsTransportFactory { daemon.endpoint(), known, prompt, {} };
    auto outcome = ConnectOutcome { std::unexpected(vthost::TransportError {}) };

    loop.blockOn(core::net::testing::allOf(serveSwapping(&daemon, &impostor, &loop),
                                           connectInto(&factory, &loop, &outcome)));

    REQUIRE_FALSE(outcome);
    CHECK(outcome.error().kind == TransportErrorKind::HostKeyChanged);
    CHECK(prompt->asked.size() == 1);
}

TEST_CASE("a daemon the user declines is not connected to, nor pinned", "[vthost][transport][pin]")
{
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto daemon = TlsDaemon { loop };
    auto const files = core::platform::testing::InMemoryFileSystem {};
    auto random = vthost::testing::CountingRandom {};
    auto known = vthost::KnownDaemons { files, "/state/known_daemons", random };
    auto factory = vthost::TlsTransportFactory {
        daemon.endpoint(), known, std::make_shared<ScriptedPinPrompt>(vthost::PinDecision::Decline), {}
    };
    auto outcome = ConnectOutcome { std::unexpected(vthost::TransportError {}) };
    auto received = std::size_t { 0 };
    loop.blockOn(core::net::testing::allOf(daemon.serveCounting(&loop, &received),
                                           connectInto(&factory, &loop, &outcome)));
    CHECK(received == 0); // nothing reached the daemon before it was trusted

    REQUIRE_FALSE(outcome);
    CHECK(outcome.error().kind == TransportErrorKind::Cancelled);
    CHECK(outcome.error().message == std::format("the daemon at {} was not trusted", daemon.hostPort()));
    CHECK(known.pinOf(daemon.hostPort()) == std::nullopt);
}

TEST_CASE("a daemon whose certificate changed is refused with both fingerprints", "[vthost][transport][pin]")
{
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto daemon = TlsDaemon { loop };
    auto const files = core::platform::testing::InMemoryFileSystem {};
    auto random = vthost::testing::CountingRandom {};
    auto known = vthost::KnownDaemons { files, "/state/known_daemons", random };
    auto const stale = std::string { "SHA256:47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU" };
    REQUIRE(known.pin(daemon.hostPort(), stale));
    // The prompt would say yes: no answer of the user's makes a changed certificate safe.
    auto const prompt = std::make_shared<ScriptedPinPrompt>(vthost::PinDecision::Trust);
    auto factory = vthost::TlsTransportFactory { daemon.endpoint(), known, prompt, {} };
    auto outcome = ConnectOutcome { std::unexpected(vthost::TransportError {}) };
    auto received = std::size_t { 0 };
    loop.blockOn(core::net::testing::allOf(daemon.serveCounting(&loop, &received),
                                           connectInto(&factory, &loop, &outcome)));
    CHECK(received == 0); // nothing reached the daemon before it was trusted

    REQUIRE_FALSE(outcome);
    CHECK(outcome.error().kind == TransportErrorKind::HostKeyChanged);
    CHECK(outcome.error().message
          == std::format("The daemon at {0} presents {1}, but {2} is pinned. If that daemon was reinstalled, "
                         "run `contour daemon forget-pin {0}` and connect again.",
                         daemon.hostPort(),
                         daemon.fingerprint(),
                         stale));
    CHECK(prompt->asked.empty());
    CHECK(known.pinOf(daemon.hostPort()) == stale);
}

TEST_CASE("a fingerprint given beforehand decides a first contact", "[vthost][transport][pin]")
{
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto daemon = TlsDaemon { loop };
    auto const files = core::platform::testing::InMemoryFileSystem {};
    auto random = vthost::testing::CountingRandom {};
    auto known = vthost::KnownDaemons { files, "/state/known_daemons", random };

    auto expected = daemon.endpoint();
    expected.expectedFingerprint = daemon.fingerprint();
    auto right = vthost::TlsTransportFactory { expected, known, nullptr, {} };
    auto accepted = ConnectOutcome { std::unexpected(vthost::TransportError {}) };
    loop.blockOn(core::net::testing::allOf(daemon.serve(&loop, 1), connectInto(&right, &loop, &accepted)));
    REQUIRE(accepted);
    CHECK(known.pinOf(daemon.hostPort()) == daemon.fingerprint());

    // Another daemon, the same fingerprint expected: refused, and nothing pinned for it.
    auto impostor = TlsDaemon { loop };
    auto wrong = impostor.endpoint();
    wrong.expectedFingerprint = daemon.fingerprint();
    auto refused = vthost::TlsTransportFactory { wrong, known, nullptr, {} };
    auto outcome = ConnectOutcome { std::unexpected(vthost::TransportError {}) };
    auto received = std::size_t { 0 };
    loop.blockOn(core::net::testing::allOf(impostor.serveCounting(&loop, &received),
                                           connectInto(&refused, &loop, &outcome)));
    CHECK(received == 0); // nothing reached the daemon before it was trusted
    REQUIRE_FALSE(outcome);
    CHECK(outcome.error().kind == TransportErrorKind::Refused);
    CHECK(outcome.error().message.contains(impostor.fingerprint()));
    CHECK(known.pinOf(impostor.hostPort()) == std::nullopt);
}

TEST_CASE("with no way to ask and no fingerprint, a daemon not pinned yet is refused",
          "[vthost][transport][pin]")
{
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto daemon = TlsDaemon { loop };
    auto const files = core::platform::testing::InMemoryFileSystem {};
    auto random = vthost::testing::CountingRandom {};
    auto known = vthost::KnownDaemons { files, "/state/known_daemons", random };
    auto factory = vthost::TlsTransportFactory { daemon.endpoint(), known, nullptr, {} };
    auto outcome = ConnectOutcome { std::unexpected(vthost::TransportError {}) };
    auto received = std::size_t { 0 };
    loop.blockOn(core::net::testing::allOf(daemon.serveCounting(&loop, &received),
                                           connectInto(&factory, &loop, &outcome)));
    CHECK(received == 0); // nothing reached the daemon before it was trusted

    REQUIRE_FALSE(outcome);
    CHECK(outcome.error().kind == TransportErrorKind::HostKeyUnknown);
    CHECK(retryOf(outcome.error().kind) == TransportRetry::Permanent);
    CHECK(outcome.error().message.contains("--daemon-fingerprint"));
    CHECK(outcome.error().message.contains("contour daemon status"));
    CHECK(known.pinOf(daemon.hostPort()) == std::nullopt);
}

TEST_CASE("a CA-verified daemon needs no pin", "[vthost][transport][pin]")
{
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto daemon = TlsDaemon { loop };
    auto const files = core::platform::testing::InMemoryFileSystem {};
    auto random = vthost::testing::CountingRandom {};
    auto known = vthost::KnownDaemons { files, "/state/known_daemons", random };
    auto endpoint = daemon.endpoint();
    endpoint.caPem = daemon.identity.certPem; // the self-signed certificate is its own anchor
    auto factory = vthost::TlsTransportFactory { endpoint, known, nullptr, {} };
    auto outcome = ConnectOutcome { std::unexpected(vthost::TransportError {}) };
    loop.blockOn(core::net::testing::allOf(daemon.serve(&loop, 1), connectInto(&factory, &loop, &outcome)));

    REQUIRE(outcome);
    CHECK(known.pinOf(daemon.hostPort()) == std::nullopt);
}

TEST_CASE("an expected fingerprint is never overridden by a pin", "[vthost][transport][pin]")
{
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto daemon = TlsDaemon { loop };
    auto const files = core::platform::testing::InMemoryFileSystem {};
    auto random = vthost::testing::CountingRandom {};
    auto known = vthost::KnownDaemons { files, "/state/known_daemons", random };
    // The pin on record is the daemon's own, but the user was told another fingerprint out of band.
    REQUIRE(known.pin(daemon.hostPort(), daemon.fingerprint()));
    auto endpoint = daemon.endpoint();
    endpoint.expectedFingerprint = "SHA256:47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU";
    auto factory = vthost::TlsTransportFactory { endpoint, known, nullptr, {} };
    auto outcome = ConnectOutcome { std::unexpected(vthost::TransportError {}) };
    auto received = std::size_t { 0 };
    loop.blockOn(core::net::testing::allOf(daemon.serveCounting(&loop, &received),
                                           connectInto(&factory, &loop, &outcome)));

    REQUIRE_FALSE(outcome);
    CHECK(outcome.error().kind == TransportErrorKind::Refused);
    CHECK(received == 0);
    CHECK(known.pinOf(daemon.hostPort()) == daemon.fingerprint());
}
