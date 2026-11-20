// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/screen/Terminal.hpp>
#include <vtbackend/screen/TerminalClocks.hpp>

#include <vtpty/MockPty.hpp>

#include <core/async/WhenAll.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/testing/CoroTestSupport.hpp>
#include <core/net/testing/InMemoryTransport.hpp>
#include <core/platform/NativeFileSystem.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include <vthost/NativeSession.hpp>
#include <vthost/SessionHost.hpp>
#include <vthost/client/NativeClient.hpp>
#include <vthost/client/ScreenMirror.hpp>
#include <vthost/testing/GridParity.hpp>
#include <vthost/testing/ScenarioGuard.hpp>
#include <vthost/testing/TestHelpers.hpp>

using core::async::Task;
using vthost::NativeSession;
using vthost::SessionHost;
using vthost::client::NativeClient;
using vthost::client::ScreenMirror;
using vthost::testing::newSession;
namespace proto = vthost::proto;
using namespace std::chrono_literals;

namespace
{

/// @return What an application writes to register a square at @p codepoint, with @p parameters.
std::string registerSquare(std::string_view codepoint, std::string_view parameters = "reply=0")
{
    return std::format("\033_25a1;r;cp={};fmt=vg;vb=0,0,24,24;{};fill=nonzero,fg:M4 4h16v16H4Z\033\\",
                       codepoint,
                       parameters);
}

/// Whether the offered capabilities of the client include Capability::RegisteredGlyphs.
enum class ClientOffers : uint8_t
{
    NoRegisteredGlyphs,
    RegisteredGlyphs,
};

/// The real server serving a real client whose ScreenMirror populates a terminal of its own.
struct GlyphHarness
{
    explicit GlyphHarness(ClientOffers offers = ClientOffers::RegisteredGlyphs): mirrorOffers { offers }
    {
        auto settings = vtbackend::Settings {};
        settings.historyLimits = vtbackend::HistoryLimits::plain(vtbackend::LineCount(1000));
        mirror = std::make_unique<vtbackend::Terminal>(mirrorEvents,
                                                       core::defaultEnvironment(),
                                                       core::platform::NativeFileSystem::instance(),
                                                       std::make_unique<vtpty::MockPty>(settings.pageSize),
                                                       std::move(settings),
                                                       std::nullopt,
                                                       vtbackend::TerminalClocks::system(),
                                                       vtbackend::randomSemanticBlockTokens());
        populator = std::make_unique<ScreenMirror>(*mirror, vthost::DaemonMachine::Same);
    }

    /// Builds the connection once the server terminal holds what the case wants it to hold at attach.
    void connect()
    {
        server = std::make_unique<NativeSession>(loop,
                                                 host,
                                                 vthost::ConnectionId { .endpoint = "test", .index = 1 },
                                                 std::move(pair.first),
                                                 NativeSession::DefaultWriteQueueBytes);
        auto capabilities = proto::CapabilitySet {};
        if (mirrorOffers == ClientOffers::RegisteredGlyphs)
            capabilities = proto::CapabilitySet::of({ proto::Capability::RegisteredGlyphs });
        client = std::make_unique<NativeClient>(
            loop,
            std::move(pair.second),
            NativeClient::HandshakeOptions { .capabilities = std::move(capabilities) },
            vtbackend::CommandBlockStoreLimits {},
            NativeClient::UpdateHandler {
                [this](vthost::client::RemoteScreen const& screen, proto::Delta const& delta) {
                    populator->apply(screen, delta);
                } },
            NativeClient::ImageHandler {},
            NativeClient::SessionEventHandler {
                [this](vthost::client::RemoteScreen const&, proto::SessionEventPdu const& event) {
                    if (std::holds_alternative<proto::RegisteredGlyphState>(event))
                        ++glyphPushes;
                    populator->applyEvent(event);
                } },
            NativeClient::LayoutHandler {});
    }

    [[nodiscard]] vtbackend::Terminal* serverTerminal(vtworkspace::SessionId session)
    {
        return host.terminal(session);
    }

    /// Writes @p bytes on the server terminal and pushes the resulting delta.
    void serverWrites(vtworkspace::SessionId session, std::string_view bytes)
    {
        serverTerminal(session)->writeToScreen(bytes);
        server->sessionScreenUpdated(session);
    }

    /// @return The glyph the mirror holds at @p codepoint, or null.
    [[nodiscard]] std::shared_ptr<vtbackend::RegisteredGlyph const> mirrored(char32_t codepoint) const
    {
        return std::as_const(*mirror).glyphGlossary().find(codepoint);
    }

    ClientOffers mirrorOffers;
    int glyphPushes = 0; ///< How many RegisteredGlyphState pushes the client received.
    std::unique_ptr<core::net::IoBackend> source = core::net::makeDefaultBackend();
    core::net::EventLoop loop { *source };
    SessionHost host { loop,
                       [](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const&) {
                           return std::make_unique<vtpty::MockPty>(size);
                       },
                       [] {
                           auto settings = vtbackend::Settings {};
                           settings.historyLimits =
                               vtbackend::HistoryLimits::plain(vtbackend::LineCount(1000));
                           return settings;
                       }(),
                       core::defaultEnvironment(),
                       vthost::SessionHostConfig { .pumping = vthost::SessionPumping::Manual } };
    core::net::testing::SocketPair pair = *core::net::testing::makeSocketPair(loop);
    vtbackend::Terminal::NullEvents mirrorEvents;
    std::unique_ptr<vtbackend::Terminal> mirror;
    std::unique_ptr<ScreenMirror> populator;
    std::unique_ptr<NativeSession> server;
    std::unique_ptr<NativeClient> client;
};

/// Polls until @p ready holds, failing the scenario when it never does.
Task<void> requireUntil(core::net::EventLoop* loop, std::function<bool()> ready)
{
    auto const reached = co_await core::net::testing::waitUntil(loop, std::move(ready), 2000);
    REQUIRE(reached);
}

Task<void> drive(GlyphHarness* h, Task<void> scenario)
{
    co_await core::async::whenAll(
        h->server->run(),
        h->client->run(),
        vthost::testing::guardScenario(&h->loop, [h] { h->client->detach(); }, std::move(scenario)));
}

/// @return Whether the mirror's row 0 has caught up with @p text.
bool mirrorShows(GlyphHarness& h, std::string_view text)
{
    return h.mirror->primaryScreen().grid().lineText(vtbackend::LineOffset(0)).starts_with(text);
}

} // namespace

TEST_CASE("a glyph registered before attach reaches the client", "[vthost][glyph][daemon]")
{
    auto h = GlyphHarness {};
    auto const session = newSession(h.host);
    h.serverTerminal(session)->writeToScreen(registerSquare("100000") + "i:\U00100000");
    h.connect();

    auto scenario = [](GlyphHarness* h, vtworkspace::SessionId session) -> Task<void> {
        co_await requireUntil(&h->loop, [&] { return h->mirrored(0x100000) != nullptr; });
        co_await requireUntil(&h->loop, [&] { return mirrorShows(*h, "i:\U00100000"); });

        auto const serverGlyph = std::as_const(*h->serverTerminal(session)).glyphGlossary().find(0x100000);
        auto const mirrorGlyph = h->mirrored(0x100000);
        REQUIRE(serverGlyph != nullptr);
        REQUIRE(mirrorGlyph != nullptr);
        // The same content on both ends: the same payload, read the same way, and so the same atlas id.
        CHECK(mirrorGlyph->id == serverGlyph->id);
        CHECK(mirrorGlyph->definition == serverGlyph->definition);

        // And the mirror's frame carries it, which is what its window draws from.
        h->mirror->markScreenDirty();
        (void) h->mirror->refreshRenderBuffer();
        auto const frame = h->mirror->renderBuffer();
        REQUIRE(frame.get().registeredGlyphs != nullptr);
        CHECK(vtbackend::registeredGlyphOf(frame.get().registeredGlyphs.get(), U"\U00100000") != nullptr);
        h->client->detach();
    }(&h, session);

    h.loop.blockOn(drive(&h, std::move(scenario)));
}

TEST_CASE("registering, replacing and clearing after attach follow on the client", "[vthost][glyph][daemon]")
{
    auto h = GlyphHarness {};
    auto const session = newSession(h.host);
    h.connect();

    auto scenario = [](GlyphHarness* h, vtworkspace::SessionId session) -> Task<void> {
        h->serverWrites(session, registerSquare("100000") + registerSquare("100001"));
        co_await requireUntil(&h->loop, [&] { return h->mirrored(0x100000) && h->mirrored(0x100001); });
        auto const first = h->mirrored(0x100000);

        // Registered again with another layout: the client's glyph is the new one.
        h->serverWrites(session, registerSquare("100000", "reply=0;fit=stretch"));
        co_await requireUntil(&h->loop, [&] {
            auto const now = h->mirrored(0x100000);
            return now != nullptr && now->id != first->id;
        });
        CHECK(h->mirrored(0x100000)->definition.layout.fit == vtbackend::GlyphFit::Stretch);
        // The slot that did not change was not sent again, and is still there.
        CHECK(h->mirrored(0x100001) != nullptr);

        // One slot cleared.
        h->serverWrites(session, "\033_25a1;c;cp=100000\033\\");
        co_await requireUntil(&h->loop, [&] { return h->mirrored(0x100000) == nullptr; });
        CHECK(h->mirrored(0x100001) != nullptr);

        // Every slot cleared, by RIS.
        h->serverWrites(session, "\033c");
        co_await requireUntil(&h->loop, [&] { return std::as_const(*h->mirror).glyphGlossary().empty(); });
        h->client->detach();
    }(&h, session);

    h.loop.blockOn(drive(&h, std::move(scenario)));
}

TEST_CASE("a replay that changes no glyph leaves the client's glossary alone", "[vthost][glyph][daemon]")
{
    // Switching to the alternate screen and back replays the whole screen each time. The glyphs did not
    // change, so the mirror must not rebuild them -- which would hash every outline again.
    auto h = GlyphHarness {};
    auto const session = newSession(h.host);
    h.serverTerminal(session)->writeToScreen(registerSquare("100000") + "i:\U00100000");
    h.connect();

    auto scenario = [](GlyphHarness* h, vtworkspace::SessionId session) -> Task<void> {
        co_await requireUntil(&h->loop, [&] { return mirrorShows(*h, "i:\U00100000"); });
        REQUIRE(h->mirrored(0x100000) != nullptr);
        auto const generation = std::as_const(*h->mirror).glyphGlossary().generation();
        auto const glyph = h->mirrored(0x100000);

        h->serverWrites(session, "\033[?1049h");
        co_await requireUntil(&h->loop, [&] { return h->mirror->isAlternateScreen(); });
        h->serverWrites(session, "\033[?1049l");
        co_await requireUntil(&h->loop, [&] { return !h->mirror->isAlternateScreen(); });

        CHECK(std::as_const(*h->mirror).glyphGlossary().generation() == generation);
        CHECK(h->mirrored(0x100000) == glyph); // the very same glyph, not an equal one rebuilt

        // A change after that still arrives.
        h->serverWrites(session, registerSquare("100001"));
        co_await requireUntil(&h->loop, [&] { return h->mirrored(0x100001) != nullptr; });
        h->client->detach();
    }(&h, session);

    h.loop.blockOn(drive(&h, std::move(scenario)));
}

TEST_CASE("a client that does not mirror glyphs is sent none", "[vthost][glyph][daemon]")
{
    auto h = GlyphHarness { ClientOffers::NoRegisteredGlyphs };
    auto const session = newSession(h.host);
    h.serverTerminal(session)->writeToScreen(registerSquare("100000") + "i:\U00100000");
    h.connect();

    auto scenario = [](GlyphHarness* h, vtworkspace::SessionId session) -> Task<void> {
        // The rows arrive all the same, with the codepoint the application wrote.
        co_await requireUntil(&h->loop, [&] { return mirrorShows(*h, "i:\U00100000"); });
        h->serverWrites(session, registerSquare("100001") + "x");
        co_await requireUntil(&h->loop, [&] { return mirrorShows(*h, "i:\U00100000x"); });
        CHECK(h->glyphPushes == 0);
        CHECK(std::as_const(*h->mirror).glyphGlossary().empty());
        h->client->detach();
    }(&h, session);

    h.loop.blockOn(drive(&h, std::move(scenario)));
}

TEST_CASE("a glossary too large for one push arrives in several, whole", "[vthost][glyph][daemon]")
{
    // Every slot of the block, each with a payload of some 2 KiB: two megabytes, more than one push
    // carries.
    auto h = GlyphHarness {};
    auto const session = newSession(h.host);
    auto payload = std::string { "fill=nonzero,fg:M1 1" };
    for ([[maybe_unused]] auto const _: std::views::iota(0, 250))
        payload += "h0.01v0.01";
    {
        auto const definition = vtbackend::parseGlyphDefinition(
                                    vtbackend::GlyphStatement { .viewBox = "0,0,24,24", .payload = payload })
                                    .value();
        auto* const terminal = h.serverTerminal(session);
        auto const guard = std::lock_guard { *terminal };
        for (auto const slot: std::views::iota(size_t { 0 }, vtbackend::GlyphRegistrationSlots))
            terminal->glyphGlossary().registerGlyph(
                static_cast<char32_t>(vtbackend::FirstGlyphRegistrationCodepoint + slot), definition);
    }
    h.connect();

    auto scenario = [](GlyphHarness* h, std::string const* payload) -> Task<void> {
        co_await requireUntil(&h->loop, [&] { return h->mirrored(0x1003FF) != nullptr; });
        auto const frame = std::as_const(*h->mirror).glyphGlossary().snapshot();
        REQUIRE(frame != nullptr);
        CHECK(std::ranges::none_of(*frame, [](auto const& glyph) { return glyph == nullptr; }));
        CHECK(h->mirrored(0x100200)->definition.payload == *payload);
        CHECK(h->glyphPushes > 1);
        h->client->detach();
    }(&h, &payload);

    h.loop.blockOn(drive(&h, std::move(scenario)));
}
