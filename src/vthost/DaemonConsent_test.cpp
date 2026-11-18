// SPDX-License-Identifier: Apache-2.0
//
// A daemon-scoped extension's grant, asked of the windows attached: real connections, each over its own
// NativeSession, a fake daemon world behind the host. Only a connection that can answer is asked, only one
// that was asked may answer, and the first answer settles the question everywhere.
#include <vtbackend/screen/Settings.hpp>

#include <vtpty/MockPty.hpp>

#include <core/Environment.hpp>
#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/testing/CoroTestSupport.hpp>
#include <core/net/testing/InMemoryTransport.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <vthost/ConnectionId.hpp>
#include <vthost/DaemonConsentKey.hpp>
#include <vthost/ExtensionHosting.hpp>
#include <vthost/HostedShell.hpp>
#include <vthost/NativeSession.hpp>
#include <vthost/PermissionWire.hpp>
#include <vthost/SessionHost.hpp>
#include <vthost/proto/Negotiation.hpp>
#include <vthost/proto/Pdu.hpp>
#include <vthost/testing/CountingRandom.hpp>
#include <vthost/testing/FakeDaemonWorldHosting.hpp>

using core::async::Task;
using vthost::testing::FakeDaemonWorldHosting;
namespace proto = vthost::proto;

namespace
{

/// The longest a client waits, in 1 ms loop ticks, for what it counts on.
constexpr auto WaitTicks = 20'000;

/// How long a client that must receive nothing more listens for it, in loop ticks.
constexpr auto QuietTicks = 300;

/// The question the fake daemon world holds open.
[[nodiscard]] vthost::DaemonExtensionRequest orchestratorRequest()
{
    return vthost::DaemonExtensionRequest {
        .name = "orchestrator",
        .grantDigest = "9a1b",
        .title = "“orchestrator” wants to run in the background",
        .lines = { { "Module: orchestrator@/x.wasm", uint8_t { 0 } },
                   { "gh-auth (short, in repos): gh auth status", uint8_t { 1 } } },
    };
}

/// @return A hello that can, or cannot, answer a daemon extension's question.
[[nodiscard]] proto::DecodedPdu helloOf(proto::CapabilitySet capabilities)
{
    return proto::DecodedPdu { proto::ClientHello { .capabilities = std::move(capabilities) } };
}

/// @return A hello from a window: it can answer. Heartbeat, so that a client can tell when what it sent
///         before was read (its echo comes back after).
[[nodiscard]] proto::DecodedPdu windowHello()
{
    return helloOf(proto::CapabilitySet::of(
        { proto::Capability::DaemonExtensionConsent, proto::Capability::Heartbeat }));
}

/// The socket the harness's daemon is at, as its consent proofs are bound to it.
constexpr auto DaemonSocket = std::string_view { "/run/test/contour" };

/// The consent key of the harness's daemon.
[[nodiscard]] vthost::ConsentKey daemonKey()
{
    auto key = vthost::ConsentKey {};
    key.fill(std::byte { 0x42 });
    return key;
}

/// What a client proves after the daemon's hello.
enum class Proving : uint8_t
{
    Nothing = 0, ///< It proves nothing: what a process that offers the capability without the key does.
    RightKey,    ///< It proves the daemon's key over its own nonce: a window.
    WrongKey,    ///< It proves another key.
    Replayed,    ///< It sends a proof another connection made over that connection's nonce.
};

/// @return A hello from what cannot answer, as the CLI's native clients.
[[nodiscard]] proto::DecodedPdu cliHello()
{
    return helloOf({});
}

/// @return @p answer to the orchestrator's question.
[[nodiscard]] proto::DecodedPdu replyOf(vthost::GrantState answer)
{
    return proto::DecodedPdu { proto::ExtensionPermissionReply {
        .name = "orchestrator", .grantDigest = "9a1b", .grantState = std::to_underlying(answer) } };
}

/// One step of a client's script: once @ref when holds, send @ref send.
struct Step
{
    std::function<bool()> when;
    std::vector<proto::DecodedPdu> send {};
    /// Makes what is sent instead of @ref send, once @ref when holds: for what depends on what arrived.
    std::function<std::vector<proto::DecodedPdu>()> make {};
};

/// One connection to the host: the client's end and what it decoded, and the server's NativeSession.
struct Client
{
    core::net::testing::SocketPair pair;
    std::unique_ptr<vthost::NativeSession> server;
    std::vector<proto::DecodedFrame> received {};
    Proving proving = Proving::RightKey;
    std::optional<vthost::ConsentProofMac> replayed {}; ///< For Proving::Replayed: the proof to send.
    std::optional<vthost::ConsentProofMac> proved {};   ///< The proof it sent, once it did.

    /// @return The nonce its ServerHello carried; none before it, or when it carried none.
    [[nodiscard]] std::optional<vthost::ConsentNonce> nonce() const
    {
        for (auto const& frame: received)
            if (auto const* hello = std::get_if<proto::ServerHello>(&frame.pdu))
                return hello->consentNonce;
        return std::nullopt;
    }

    /// @return The run its ServerHello named.
    [[nodiscard]] uint64_t instance() const
    {
        for (auto const& frame: received)
            if (auto const* hello = std::get_if<proto::ServerHello>(&frame.pdu))
                return hello->daemonInstance;
        return 0;
    }

    /// @return How many Heartbeat echoes came back: what was sent before each has been read.
    [[nodiscard]] std::size_t echoes() const { return count<proto::Heartbeat>(); }

    /// @return Whether what it sent with its proof was read: the Heartbeat sent after it came back.
    [[nodiscard]] bool proofRead() const { return count<proto::Heartbeat>() != 0; }

    /// @return How many frames holding a @p Pdu arrived.
    template <typename Pdu>
    [[nodiscard]] std::size_t count() const
    {
        return static_cast<std::size_t>(std::ranges::count_if(received, [](proto::DecodedFrame const& frame) {
            return std::holds_alternative<Pdu>(frame.pdu);
        }));
    }

    /// @return Whether the attach snapshot arrived: the connection reached the point where it is asked.
    [[nodiscard]] bool attached() const
    {
        return std::ranges::any_of(received, [](proto::DecodedFrame const& frame) {
            auto const* delta = std::get_if<proto::Delta>(&frame.pdu);
            return delta != nullptr && delta->snapshot != 0 && delta->completesSnapshot();
        });
    }
};

/// @return @p pdus, encoded one after the other.
[[nodiscard]] std::vector<std::byte> encoded(std::vector<proto::DecodedPdu> const& pdus)
{
    auto request = proto::Writer {};
    auto serial = uint64_t { 1 };
    for (auto const& pdu: pdus)
        proto::encodePdu(request, serial++, pdu);
    return { request.view().begin(), request.view().end() };
}

/// Decodes what the server sends into @p client until the stream ends (or the read is cancelled).
Task<void> readAll(Client* client)
{
    auto buffer = std::vector<std::byte> {};
    auto scratch = std::vector<std::byte>(4096);
    while (true)
    {
        auto decoded = proto::decodePdu(buffer);
        if (decoded)
        {
            buffer.erase(buffer.begin(), buffer.begin() + static_cast<long>(decoded->consumed));
            client->received.push_back(std::move(*decoded));
            continue;
        }
        if (decoded.error() != proto::DecodeError::NeedMoreData)
            co_return;
        auto const n = co_await client->pair.second->read(scratch);
        if (!n.has_value() || *n == 0)
            co_return;
        buffer.insert(buffer.end(), scratch.begin(), scratch.begin() + static_cast<long>(*n));
    }
}

/// Writes @p pdus from @p client's end.
Task<void> write(Client* client, std::vector<proto::DecodedPdu> pdus)
{
    auto const bytes = encoded(pdus);
    std::ignore =
        co_await client->pair.second->write(std::span<std::byte const> { bytes.data(), bytes.size() });
}

/// Runs @p steps from @p client's end, then waits until @p done (listening @p quietTicks more), and returns.
Task<void> drive(core::net::EventLoop* loop,
                 Client* client,
                 std::vector<Step> steps,
                 std::function<bool()> done,
                 int quietTicks)
{
    for (auto& step: steps)
    {
        std::ignore = co_await core::net::testing::waitUntil(loop, step.when, WaitTicks);
        co_await write(client, step.make ? step.make() : std::move(step.send));
    }
    std::ignore = co_await core::net::testing::waitUntil(loop, done, WaitTicks);
    if (quietTicks != 0)
        std::ignore = co_await core::net::testing::waitUntil(loop, [] { return false; }, quietTicks);
}

/// Proves what the client proves once its hello was answered with a nonce, and a Heartbeat behind it.
Task<void> prove(core::net::EventLoop* loop, Client* client)
{
    if (client->proving == Proving::Nothing)
        co_return;
    std::ignore = co_await core::net::testing::waitUntil(
        loop, [client] { return client->nonce().has_value(); }, WaitTicks);
    auto const nonce = client->nonce();
    if (!nonce)
        co_return;
    auto otherKey = vthost::ConsentKey {};
    otherKey.fill(std::byte { 0x17 });
    auto mac = vthost::ConsentProofMac {};
    auto const context = vthost::ConsentContext { .daemonInstance = client->instance(),
                                                  .socket = std::string { DaemonSocket } };
    switch (client->proving)
    {
        case Proving::RightKey: mac = vthost::consentProofOf(daemonKey(), context, *nonce); break;
        case Proving::WrongKey: mac = vthost::consentProofOf(otherKey, context, *nonce); break;
        case Proving::Replayed: mac = client->replayed.value_or(mac); break;
        case Proving::Nothing: break;
    }
    client->proved = mac;
    co_await write(client,
                   { proto::DecodedPdu { proto::ConsentProof { .mac = mac } },
                     proto::DecodedPdu { proto::Heartbeat { .nonce = 7 } } });
}

/// One client's whole part: reads while its script runs, then closes its end, which ends its server.
Task<void> play(core::net::EventLoop* loop,
                Client* client,
                std::vector<Step> steps,
                std::function<bool()> done,
                int quietTicks = 0)
{
    co_await core::net::testing::anyOf(
        readAll(client),
        core::net::testing::allOf(prove(loop, client),
                                  drive(loop, client, std::move(steps), std::move(done), quietTicks)));
    client->pair.second->close();
}

/// Runs @p job once @p when holds.
Task<void> runWhen(core::net::EventLoop* loop, std::function<bool()> when, std::function<void()> job)
{
    std::ignore = co_await core::net::testing::waitUntil(loop, std::move(when), WaitTicks);
    job();
}

/// A host whose daemon world is a fake holding the orchestrator's question open, and its connections.
struct ConsentHarness
{
    ConsentHarness()
    {
        world->pending = { orchestratorRequest() };
        std::ignore = host.createTab();
    }

    /// @return A new connection to the host, of a daemon holding daemonKey(); its client proves @p proving.
    [[nodiscard]] std::unique_ptr<Client> connect(Proving proving = Proving::RightKey)
    {
        auto client = std::make_unique<Client>(Client {
            .pair = *core::net::testing::makeSocketPair(loop), .server = nullptr, .proving = proving });
        auto services = vthost::NativeSessionServices {};
        services.handshake.consent = vthost::ConsentGate { .key = daemonKey(),
                                                           .socket = std::string { DaemonSocket },
                                                           .random = random };
        client->server = std::make_unique<vthost::NativeSession>(
            loop,
            host,
            vthost::ConnectionId { .endpoint = "test", .index = ++connections },
            std::move(client->pair.first),
            vthost::NativeSession::DefaultWriteQueueBytes,
            std::move(services));
        return client;
    }

    /// @return The answers the daemon world recorded.
    [[nodiscard]] std::vector<std::string> answers() const
    {
        auto recorded = std::vector<std::string> {};
        std::ranges::copy_if(*journal, std::back_inserter(recorded), [](std::string const& entry) {
            return entry.starts_with("answer ");
        });
        return recorded;
    }

    std::unique_ptr<core::net::IoBackend> source = core::net::makeDefaultBackend();
    core::net::EventLoop loop { *source };
    std::shared_ptr<std::vector<std::string>> journal = std::make_shared<std::vector<std::string>>();
    /// Made by the host's factory, which hands it what the host hears from it.
    std::shared_ptr<FakeDaemonWorldHosting> world;
    vthost::SessionHost host { loop,
                               [](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const&) {
                                   return std::make_unique<vtpty::MockPty>(size);
                               },
                               vtbackend::Settings {},
                               core::defaultEnvironment(),
                               vthost::SessionHostConfig {
                                   .pumping = vthost::SessionPumping::Manual,
                                   .daemonWorld = [this](vthost::DaemonWorldEvents events,
                                                         vthost::OwnerPost const& /*post*/) {
                                       world = std::make_shared<FakeDaemonWorldHosting>(
                                           journal,
                                           [](std::function<void()> const& /*job*/) {},
                                           std::move(events));
                                       return world;
                                   } } };
    uint64_t connections = 0;
    vthost::testing::CountingRandom random;
};

} // namespace

TEST_CASE("DaemonConsent: the first window's answer is recorded and settles the question elsewhere",
          "[daemon][consent]")
{
    auto h = ConsentHarness {};
    auto first = h.connect();
    auto second = h.connect();
    auto* const a = first.get();
    auto* const b = second.get();
    h.loop.blockOn(core::net::testing::allOf(
        a->server->run(),
        b->server->run(),
        // The first answers once both were asked; the second answers too, after the question settled.
        play(&h.loop,
             a,
             { Step { .when = [] { return true; }, .send = { windowHello() } },
               Step { .when =
                          [a, b] {
                              return a->count<proto::ExtensionPermissionRequest>() == 1
                                     && b->count<proto::ExtensionPermissionRequest>() == 1;
                          },
                      .send = { replyOf(vthost::GrantState::Granted) } } },
             [b] { return b->count<proto::ExtensionPermissionSettled>() == 1; }),
        play(
            &h.loop,
            b,
            { Step { .when = [] { return true; }, .send = { windowHello() } },
              Step { .when = [b] { return b->count<proto::ExtensionPermissionSettled>() == 1; },
                     .send = { replyOf(vthost::GrantState::Denied) } } },
            [] { return true; },
            QuietTicks)));

    REQUIRE(a->count<proto::ExtensionPermissionRequest>() == 1);
    REQUIRE(b->count<proto::ExtensionPermissionRequest>() == 1);
    auto const request = std::ranges::find_if(a->received, [](proto::DecodedFrame const& frame) {
        return std::holds_alternative<proto::ExtensionPermissionRequest>(frame.pdu);
    });
    CHECK(std::get<proto::ExtensionPermissionRequest>(request->pdu)
          == proto::ExtensionPermissionRequest {
              .name = "orchestrator",
              .grantDigest = "9a1b",
              .title = "“orchestrator” wants to run in the background",
              .lines = { proto::WireConsentLine { .text = "Module: orchestrator@/x.wasm", .weight = 0 },
                         proto::WireConsentLine { .text = "gh-auth (short, in repos): gh auth status",
                                                  .weight = 1 } } });
    // The answering window is not told its own answer settled; the other one is.
    CHECK(a->count<proto::ExtensionPermissionSettled>() == 0);
    CHECK(b->count<proto::ExtensionPermissionSettled>() == 1);
    // The second answer came after the question settled, and is ignored.
    CHECK(h.answers() == std::vector<std::string> { "answer orchestrator 9a1b 1" });
}

TEST_CASE("DaemonConsent: a reply from a connection without the capability is ignored", "[daemon][consent]")
{
    auto h = ConsentHarness {};
    auto cli = h.connect(Proving::Nothing);
    auto* const c = cli.get();
    h.loop.blockOn(
        core::net::testing::allOf(c->server->run(),
                                  play(
                                      &h.loop,
                                      c,
                                      { Step { .when = [] { return true; }, .send = { cliHello() } },
                                        Step { .when = [c] { return c->attached(); },
                                               .send = { replyOf(vthost::GrantState::Granted) } } },
                                      [] { return true; },
                                      QuietTicks)));

    // Never asked, and its answer -- naming the question exactly -- records nothing.
    CHECK(c->attached());
    CHECK(c->count<proto::ExtensionPermissionRequest>() == 0);
    CHECK(h.answers().empty());
    CHECK(h.world->pending.size() == 1);
}

TEST_CASE("DaemonConsent: a reply from a connection never sent the request is ignored", "[daemon][consent]")
{
    // A proven window whose question opened without it being told (no offer reached it) holds no ticket: its
    // reply, naming the question exactly, is refused although the question is open -- the ticket alone
    // stands between it and the answer, SessionHost's own check passing.
    auto h = ConsentHarness {};
    h.world->pending.clear();
    auto late = h.connect();
    auto* const l = late.get();
    auto opened = false;
    h.loop.blockOn(
        core::net::testing::allOf(l->server->run(),
                                  runWhen(
                                      &h.loop,
                                      [l] { return l->attached() && l->proofRead(); },
                                      [&] {
                                          h.world->pending = { orchestratorRequest() };
                                          opened = true;
                                      }),
                                  play(
                                      &h.loop,
                                      l,
                                      { Step { .when = [] { return true; }, .send = { windowHello() } },
                                        Step { .when = [&opened] { return opened; },
                                               .send = { replyOf(vthost::GrantState::Granted) } } },
                                      [] { return true; },
                                      QuietTicks)));

    CHECK(l->proofRead());
    CHECK(l->count<proto::ExtensionPermissionRequest>() == 0);
    CHECK(h.answers().empty());
    CHECK(h.world->pending.size() == 1);
}

TEST_CASE("DaemonConsent: a connection that offers the capability without proving the key is never asked",
          "[daemon][consent]")
{
    struct Row
    {
        std::string_view name;
        Proving proving;
    };
    for (auto const& row: { Row { .name = "no proof", .proving = Proving::Nothing },
                            Row { .name = "a proof of another key", .proving = Proving::WrongKey } })
    {
        INFO(row.name);
        auto h = ConsentHarness {};
        auto forger = h.connect(row.proving);
        auto* const f = forger.get();
        h.loop.blockOn(
            core::net::testing::allOf(f->server->run(),
                                      play(
                                          &h.loop,
                                          f,
                                          { Step { .when = [] { return true; }, .send = { windowHello() } },
                                            Step { .when = [f] { return f->attached(); },
                                                   .send = { replyOf(vthost::GrantState::Granted) } } },
                                          [] { return true; },
                                          QuietTicks)));

        CHECK(f->attached());
        CHECK(f->nonce().has_value()); // it was offered the proof, and did not make it
        CHECK(f->count<proto::ExtensionPermissionRequest>() == 0);
        CHECK(h.answers().empty());
    }
}

TEST_CASE("DaemonConsent: a proof replayed on another connection does not hold", "[daemon][consent]")
{
    auto h = ConsentHarness {};
    auto window = h.connect();
    auto* const w = window.get();
    h.loop.blockOn(core::net::testing::allOf(
        w->server->run(),
        play(&h.loop, w, { Step { .when = [] { return true; }, .send = { windowHello() } } }, [w] {
            return w->count<proto::ExtensionPermissionRequest>() == 1;
        })));
    REQUIRE(w->proved.has_value());
    REQUIRE(w->count<proto::ExtensionPermissionRequest>() == 1);

    // The window's own proof, sent again over a new connection, whose hello carried a fresh nonce.
    auto replay = h.connect(Proving::Replayed);
    auto* const r = replay.get();
    r->replayed = w->proved;
    h.loop.blockOn(
        core::net::testing::allOf(r->server->run(),
                                  play(
                                      &h.loop,
                                      r,
                                      { Step { .when = [] { return true; }, .send = { windowHello() } },
                                        Step { .when = [r] { return r->attached() && r->proofRead(); },
                                               .send = { replyOf(vthost::GrantState::Granted) } } },
                                      [] { return true; },
                                      QuietTicks)));

    CHECK(r->nonce() != w->nonce());
    CHECK(r->count<proto::ExtensionPermissionRequest>() == 0);
    CHECK(h.answers().empty());
}

TEST_CASE("DaemonConsent: a pane's environment names no consent key", "[daemon][consent]")
{
    // The key's file is the window's to read: nothing the daemon hands a shell names it.
    auto environments = std::vector<vtpty::Process::Environment> {};
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto host =
        vthost::SessionHost { loop,
                              [&](vtbackend::PageSize size,
                                  std::optional<vtpty::Process::ExecInfo> const& spawn) {
                                  if (spawn)
                                      environments.push_back(spawn->env);
                                  return std::make_unique<vtpty::MockPty>(size);
                              },
                              vtbackend::Settings {},
                              core::defaultEnvironment(),
                              vthost::SessionHostConfig { .pumping = vthost::SessionPumping::Manual } };
    std::ignore = host.createTab();
    REQUIRE_FALSE(environments.empty());
    for (auto const& environment: environments)
        for (auto const& [name, value]: environment)
        {
            INFO(name << "=" << value);
            CHECK_FALSE(value.contains("consent"));
            CHECK_FALSE(value.ends_with(".key"));
            if (name.starts_with("CONTOUR_"))
                CHECK(std::ranges::contains(vthost::HostedShellVariables, std::string_view { name }));
        }
}

TEST_CASE("DaemonConsent: a window attaching while the question is open is asked; a CLI is not",
          "[daemon][consent]")
{
    auto h = ConsentHarness {};
    auto window = h.connect();
    auto cli = h.connect(Proving::Nothing);
    auto* const w = window.get();
    auto* const c = cli.get();
    h.loop.blockOn(
        core::net::testing::allOf(w->server->run(),
                                  c->server->run(),
                                  play(&h.loop,
                                       w,
                                       { Step { .when = [] { return true; }, .send = { windowHello() } } },
                                       [w] { return w->count<proto::ExtensionPermissionRequest>() == 1; }),
                                  play(
                                      &h.loop,
                                      c,
                                      { Step { .when = [] { return true; }, .send = { cliHello() } } },
                                      [c] { return c->attached(); },
                                      QuietTicks)));

    CHECK(w->count<proto::ExtensionPermissionRequest>() == 1);
    CHECK(c->count<proto::ExtensionPermissionRequest>() == 0);
    CHECK(h.world->pending.size() == 1); // asked, not answered
}

TEST_CASE("DaemonConsent: asking again re-sends to every window, and says how many", "[daemon][consent]")
{
    auto h = ConsentHarness {};
    auto first = h.connect();
    auto second = h.connect();
    auto cli = h.connect(Proving::Nothing);
    auto* const a = first.get();
    auto* const b = second.get();
    auto* const c = cli.get();
    auto asked = std::optional<vthost::AskResult> {};
    auto unasked = std::optional<vthost::AskResult> {};
    h.loop.blockOn(core::net::testing::allOf(
        a->server->run(),
        b->server->run(),
        c->server->run(),
        play(&h.loop,
             a,
             { Step { .when = [] { return true; }, .send = { windowHello() } } },
             [a] { return a->count<proto::ExtensionPermissionRequest>() == 2; }),
        play(&h.loop,
             b,
             { Step { .when = [] { return true; }, .send = { windowHello() } } },
             [b] { return b->count<proto::ExtensionPermissionRequest>() == 2; }),
        play(
            &h.loop,
            c,
            { Step { .when = [] { return true; }, .send = { cliHello() } } },
            [&asked] { return asked.has_value(); },
            QuietTicks),
        runWhen(
            &h.loop,
            [a, b, c] {
                return a->count<proto::ExtensionPermissionRequest>() == 1
                       && b->count<proto::ExtensionPermissionRequest>() == 1 && c->attached();
            },
            [&] {
                asked = h.host.ask("orchestrator");
                unasked = h.host.ask("other");
            })));

    REQUIRE(asked.has_value());
    CHECK(*asked == vthost::AskResult { .outcome = vthost::AskOutcome::Asked, .windows = 2 });
    CHECK(*unasked == vthost::AskResult { .outcome = vthost::AskOutcome::NotPending, .windows = 0 });
    CHECK(a->count<proto::ExtensionPermissionRequest>() == 2);
    CHECK(b->count<proto::ExtensionPermissionRequest>() == 2);
    CHECK(c->count<proto::ExtensionPermissionRequest>() == 0);
    // Asking is not answering.
    CHECK(h.answers().empty());
}

TEST_CASE("DaemonConsent: with nothing pending, asking sends nothing", "[daemon][consent]")
{
    auto h = ConsentHarness {};
    h.world->pending.clear();
    CHECK(h.host.ask("orchestrator")
          == vthost::AskResult { .outcome = vthost::AskOutcome::NotPending, .windows = 0 });
    CHECK(h.host.pendingDaemonExtensionRequests().empty());
}

TEST_CASE("DaemonConsent: a second proof changes nothing", "[daemon][consent]")
{
    // One proof per connection: a window that proved is not unproved by a wrong one, and one refused is not
    // proved by a right one later -- nor is either an error a peer can fill the log with.
    struct Row
    {
        std::string_view name;
        Proving first;
        Proving second;
        std::size_t asked;
    };
    for (auto const& row:
         { Row {
               .name = "after Proven", .first = Proving::RightKey, .second = Proving::WrongKey, .asked = 1 },
           Row { .name = "after Refused",
                 .first = Proving::WrongKey,
                 .second = Proving::RightKey,
                 .asked = 0 } })
    {
        INFO(row.name);
        auto h = ConsentHarness {};
        h.world->pending.clear();
        auto window = h.connect(row.first);
        auto* const w = window.get();
        auto const again = [w, second = row.second] {
            auto const nonce = w->nonce().value_or(vthost::ConsentNonce {});
            auto const context = vthost::ConsentContext { .daemonInstance = w->instance(),
                                                          .socket = std::string { DaemonSocket } };
            auto otherKey = vthost::ConsentKey {};
            otherKey.fill(std::byte { 0x17 });
            auto const mac =
                vthost::consentProofOf(second == Proving::RightKey ? daemonKey() : otherKey, context, nonce);
            return std::vector<proto::DecodedPdu> { proto::DecodedPdu { proto::ConsentProof { .mac = mac } },
                                                    proto::DecodedPdu { proto::Heartbeat { .nonce = 8 } } };
        };
        auto opened = false;
        h.loop.blockOn(
            core::net::testing::allOf(w->server->run(),
                                      runWhen(
                                          &h.loop,
                                          [w] { return w->echoes() == 2; },
                                          [&] {
                                              // The question opens once both proofs were read, and is offered
                                              // to who may be asked.
                                              h.world->pending = { orchestratorRequest() };
                                              std::ignore = h.host.ask("orchestrator");
                                              opened = true;
                                          }),
                                      play(
                                          &h.loop,
                                          w,
                                          { Step { .when = [] { return true; }, .send = { windowHello() } },
                                            Step { .when = [w] { return w->proofRead(); }, .make = again } },
                                          [&opened] { return opened; },
                                          QuietTicks)));

        CHECK(w->echoes() == 2);
        CHECK(w->count<proto::ExtensionPermissionRequest>() == row.asked);
    }
}

// {{{ the daemon extensions' palette rows (Capability::ExtensionPresence)

namespace
{

/// @return A hello from a window with a palette for the daemon extensions' rows, which can answer their
///         questions too.
[[nodiscard]] proto::DecodedPdu paletteHello()
{
    return helloOf(proto::CapabilitySet::of({ proto::Capability::ExtensionPresence,
                                              proto::Capability::DaemonExtensionConsent,
                                              proto::Capability::Heartbeat }));
}

/// @return The orchestrator's palette rows, as the fake daemon world holds them, in @p state.
[[nodiscard]] vthost::ExtensionActionRows orchestratorRows(vthost::DaemonExtensionState state)
{
    return vthost::ExtensionActionRows { .extension = "orchestrator",
                                         .title = "Agents",
                                         .state = state,
                                         .rows = { { "open", "Open Agents" },
                                                   { "refresh", "Refresh Agents" } } };
}

/// @return An invocation of @p action of @p extension.
[[nodiscard]] proto::DecodedPdu invocationOf(std::string extension, std::string action)
{
    return proto::DecodedPdu { proto::InvokeExtensionAction { .extension = std::move(extension),
                                                              .action = std::move(action) } };
}

/// @return Every ExtensionPresence @p client received, in order.
[[nodiscard]] std::vector<proto::ExtensionPresence> presencesOf(Client const& client)
{
    auto presences = std::vector<proto::ExtensionPresence> {};
    for (auto const& frame: client.received)
        if (auto const* presence = std::get_if<proto::ExtensionPresence>(&frame.pdu))
            presences.push_back(*presence);
    return presences;
}

/// @return Whether @p journal holds @p entry.
[[nodiscard]] bool journaled(std::vector<std::string> const& journal, std::string_view entry)
{
    return std::ranges::contains(journal, entry);
}

/// @return The journal's entries of what an invocation does: hints, starts and restarts.
[[nodiscard]] std::vector<std::string> invocationEffects(std::vector<std::string> const& journal)
{
    auto effects = std::vector<std::string> {};
    std::ranges::copy_if(journal, std::back_inserter(effects), [](std::string const& entry) {
        return entry.starts_with("hint ") || entry.starts_with("start ") || entry.starts_with("restart ");
    });
    return effects;
}

} // namespace

TEST_CASE("ExtensionPresence: a client without extension-presence is never sent ExtensionPresence",
          "[daemon][presence]")
{
    auto h = ConsentHarness {};
    h.world->rows = { orchestratorRows(vthost::DaemonExtensionState::Running) };
    auto window = h.connect();
    auto* const w = window.get();
    h.loop.blockOn(core::net::testing::allOf(
        w->server->run(),
        runWhen(
            &h.loop,
            [w] { return w->attached() && w->proofRead(); },
            [&] { h.world->changePresence({ orchestratorRows(vthost::DaemonExtensionState::Disabled) }); }),
        play(
            &h.loop,
            w,
            { Step { .when = [] { return true; }, .send = { windowHello() } } },
            [w] { return w->attached() && w->proofRead(); },
            QuietTicks)));

    CHECK(w->attached());
    CHECK(presencesOf(*w).empty());
}

TEST_CASE("ExtensionPresence: a client with it receives the rows at attach and on a change",
          "[daemon][presence]")
{
    auto h = ConsentHarness {};
    h.world->pending.clear();
    h.world->rows = { orchestratorRows(vthost::DaemonExtensionState::Running) };
    auto window = h.connect();
    auto* const w = window.get();
    h.loop.blockOn(core::net::testing::allOf(
        w->server->run(),
        runWhen(
            &h.loop,
            [w] { return w->attached() && presencesOf(*w).size() == 1; },
            [&] {
                // A burst of changes in one loop turn is sent once.
                h.world->changePresence({ orchestratorRows(vthost::DaemonExtensionState::Disabled) });
                h.world->changePresence({ orchestratorRows(vthost::DaemonExtensionState::Pending) });
            }),
        play(
            &h.loop,
            w,
            { Step { .when = [] { return true; }, .send = { paletteHello() } } },
            [w] { return presencesOf(*w).size() == 2; },
            QuietTicks)));

    auto const presences = presencesOf(*w);
    REQUIRE(presences.size() == 2);
    CHECK(presences[0]
          == proto::ExtensionPresence {
              .extensions = { proto::WireExtensionPresence {
                  .extension = "orchestrator",
                  .title = "Agents",
                  .state = proto::WirePresenceState::Running,
                  .rows = { proto::WireActionRow { .id = "open", .title = "Open Agents" },
                            proto::WireActionRow { .id = "refresh", .title = "Refresh Agents" } } } } });
    REQUIRE(presences[1].extensions.size() == 1);
    CHECK(presences[1].extensions[0].state == proto::WirePresenceState::AwaitingGrant);
}

TEST_CASE("ExtensionPresence: an invocation for an unknown extension or action is dropped",
          "[daemon][presence]")
{
    auto h = ConsentHarness {};
    h.world->pending.clear();
    h.world->rows = { orchestratorRows(vthost::DaemonExtensionState::Running) };
    auto window = h.connect();
    auto* const w = window.get();
    auto const& journal = *h.journal;
    h.loop.blockOn(
        core::net::testing::allOf(w->server->run(),
                                  play(
                                      &h.loop,
                                      w,
                                      { Step { .when = [] { return true; }, .send = { paletteHello() } },
                                        Step { .when = [w] { return w->attached() && w->proofRead(); },
                                               .send = { invocationOf("unknown", "open"),
                                                         invocationOf("orchestrator", "undeclared"),
                                                         invocationOf("orchestrator", ""),
                                                         invocationOf("orchestrator", "open") } } },
                                      [&journal] { return journaled(journal, "hint orchestrator actions"); },
                                      QuietTicks)));

    // Only the declared action of the known extension reached it: the queue holds it alone.
    CHECK(invocationEffects(journal) == std::vector<std::string> { "hint orchestrator actions" });
    auto const taken = h.host.extensionActions().take("orchestrator");
    REQUIRE(taken.size() == 1);
    CHECK(taken.front().first == "open");
    CHECK(h.host.extensionActions().connectionOf("orchestrator", taken.front().second).has_value());
}

TEST_CASE("ExtensionPresence: an action the extension did not take is raised again once it runs",
          "[daemon][presence]")
{
    auto h = ConsentHarness {};
    h.world->pending.clear();
    h.world->rows = { orchestratorRows(vthost::DaemonExtensionState::Running) };
    auto window = h.connect();
    auto* const w = window.get();
    auto const& journal = *h.journal;
    auto const hints = [&journal] {
        return std::ranges::count(journal, std::string { "hint orchestrator actions" });
    };
    auto restarted = false;
    h.loop.blockOn(core::net::testing::allOf(
        w->server->run(),
        runWhen(
            &h.loop,
            [&hints] { return hints() == 1; },
            [&] {
                // It faulted in update(actions) before it took the action, was disabled, and restarted.
                h.world->changePresence({ orchestratorRows(vthost::DaemonExtensionState::Disabled) });
                h.world->changePresence({ orchestratorRows(vthost::DaemonExtensionState::Running) });
                restarted = true;
            }),
        play(
            &h.loop,
            w,
            { Step { .when = [] { return true; }, .send = { paletteHello() } },
              Step { .when = [w] { return w->attached() && w->proofRead(); },
                     .send = { invocationOf("orchestrator", "open") } },
              // A later invocation is raised too, though the first still waits.
              Step { .when = [&] { return restarted && hints() == 2; },
                     .send = { invocationOf("orchestrator", "refresh") } } },
            [&hints] { return hints() == 3; },
            QuietTicks)));

    CHECK(hints() == 3);
    auto const taken = h.host.extensionActions().take("orchestrator");
    REQUIRE(taken.size() == 2);
    CHECK(taken[0].first == "open");
    CHECK(taken[1].first == "refresh");
}

TEST_CASE("ExtensionPresence: an invocation from a client without the capability is dropped",
          "[daemon][presence]")
{
    auto h = ConsentHarness {};
    h.world->pending.clear();
    h.world->rows = { orchestratorRows(vthost::DaemonExtensionState::Running) };
    auto window = h.connect();
    auto* const w = window.get();
    h.loop.blockOn(core::net::testing::allOf(
        w->server->run(),
        play(
            &h.loop,
            w,
            { Step { .when = [] { return true; }, .send = { windowHello() } },
              Step { .when = [w] { return w->attached() && w->proofRead(); },
                     .send = { invocationOf("orchestrator", "open"),
                               proto::DecodedPdu { proto::Heartbeat { .nonce = 9 } } } } },
            [w] { return w->echoes() == 2; },
            QuietTicks)));

    CHECK(w->echoes() == 2);
    CHECK(invocationEffects(*h.journal).empty());
}

TEST_CASE("ExtensionPresence: what an invocation does follows the extension's state", "[daemon][presence]")
{
    struct Row
    {
        vthost::DaemonExtensionState state;
        std::vector<std::string> effects;
    };
    for (auto const& row:
         { Row { .state = vthost::DaemonExtensionState::Stopped,
                 .effects = { "start orchestrator", "hint orchestrator actions" } },
           Row { .state = vthost::DaemonExtensionState::Disabled, .effects = { "restart orchestrator" } },
           Row { .state = vthost::DaemonExtensionState::Denied, .effects = {} },
           Row { .state = vthost::DaemonExtensionState::Failed, .effects = {} } })
    {
        INFO(std::to_underlying(row.state));
        auto h = ConsentHarness {};
        h.world->pending.clear();
        h.world->rows = { orchestratorRows(row.state) };
        auto window = h.connect();
        auto* const w = window.get();
        auto const& journal = *h.journal;
        auto const expected = row.effects.size();
        h.loop.blockOn(core::net::testing::allOf(
            w->server->run(),
            play(
                &h.loop,
                w,
                { Step { .when = [] { return true; }, .send = { paletteHello() } },
                  Step { .when = [w] { return w->attached() && w->proofRead(); },
                         .send = { invocationOf("orchestrator", "open"),
                                   proto::DecodedPdu { proto::Heartbeat { .nonce = 9 } } } } },
                [&journal, expected, w] {
                    return w->echoes() == 2 && invocationEffects(journal).size() == expected;
                },
                QuietTicks)));

        CHECK(invocationEffects(journal) == row.effects);
    }
}

TEST_CASE("ExtensionPresence: invoking a pending extension asks that window again",
          "[daemon][presence][consent]")
{
    auto h = ConsentHarness {};
    h.world->rows = { orchestratorRows(vthost::DaemonExtensionState::Pending) };
    auto proven = h.connect();
    auto unproven = h.connect(Proving::Nothing);
    auto bystander = h.connect();
    auto* const p = proven.get();
    auto* const u = unproven.get();
    auto* const b = bystander.get();
    h.loop.blockOn(core::net::testing::allOf(
        p->server->run(),
        u->server->run(),
        b->server->run(),
        play(
            &h.loop,
            p,
            { Step { .when = [] { return true; }, .send = { paletteHello() } },
              Step { .when = [p] { return p->count<proto::ExtensionPermissionRequest>() == 1; },
                     .send = { invocationOf("orchestrator", "") } } },
            [p] { return p->count<proto::ExtensionPermissionRequest>() == 2; },
            QuietTicks),
        play(
            &h.loop,
            u,
            { Step { .when = [] { return true; }, .send = { paletteHello() } },
              Step { .when = [u] { return u->attached(); },
                     .send = { invocationOf("orchestrator", ""),
                               proto::DecodedPdu { proto::Heartbeat { .nonce = 9 } } } } },
            [u] { return u->echoes() == 1; },
            QuietTicks),
        play(
            &h.loop,
            b,
            { Step { .when = [] { return true; }, .send = { paletteHello() } } },
            [b] { return b->count<proto::ExtensionPermissionRequest>() == 1; },
            QuietTicks)));

    // The invoking window is asked again; one that proved no key is asked nothing, and another window is not
    // asked twice.
    CHECK(p->count<proto::ExtensionPermissionRequest>() == 2);
    CHECK(u->count<proto::ExtensionPermissionRequest>() == 0);
    CHECK(b->count<proto::ExtensionPermissionRequest>() == 1);
    CHECK(invocationEffects(*h.journal).empty());
}

// }}}
