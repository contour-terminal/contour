// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/Terminal.h>

#include <vtpty/MockPty.h>

#include <crispy/base64.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <coro/WhenAll.hpp>
#include <muxserver/NativeSession.h>
#include <muxserver/SessionHost.h>
#include <muxserver/client/AttachClient.h>
#include <muxserver/client/ScreenMirror.h>
#include <net/EventLoop.h>
#include <net/PollEventSource.h>
#include <net/testing/InMemoryTransport.h>
#include <vtmux/Pane.h>
#include <vtmux/Tab.h>

using coro::Task;
using muxserver::NativeSession;
using muxserver::SessionHost;
using muxserver::client::AttachClient;
using muxserver::client::ScreenMirror;
namespace proto = muxserver::proto;
using namespace std::chrono_literals;

namespace
{

/// Settings with the Good Image Protocol enabled on both ends, so an inline
/// image drawn on the server round-trips through the mirror's re-emit.
vtbackend::Settings gipSettings()
{
    auto settings = vtbackend::Settings {};
    settings.goodImageProtocol = true;
    return settings;
}

/// A mirror-terminal event sink recording the transient events (bell,
/// notification, clipboard write) the re-emit is expected to reproduce.
struct RecordingEvents final: vtbackend::Terminal::NullEvents
{
    int bells = 0;
    std::string notifyTitle;
    std::string notifyBody;
    std::string clipboard;

    void bell() override { ++bells; }
    void notify(std::string_view title, std::string_view body) override
    {
        notifyTitle = title;
        notifyBody = body;
    }
    void copyToClipboard(std::string_view data) override { clipboard = data; }
};

/// The full closed loop: the REAL server serves deltas of a REAL terminal,
/// the REAL client mirrors them into RemoteScreen, and the ScreenMirror
/// re-serializes every update into a LOCAL mirror terminal — whose content
/// must equal the server terminal's.
struct MirrorHarness
{
    net::PollEventSource source;
    net::EventLoop loop { source };
    SessionHost host { loop,
                       [](vtbackend::PageSize size) { return std::make_unique<vtpty::MockPty>(size); },
                       gipSettings(),
                       /*startPumps=*/false };
    net::testing::SocketPair pair = *net::testing::makeSocketPair(loop);
    std::unique_ptr<NativeSession> server =
        std::make_unique<NativeSession>(loop, host, std::move(pair.first));
    std::unique_ptr<AttachClient> client = std::make_unique<AttachClient>(loop, std::move(pair.second));

    RecordingEvents mirrorEvents;
    std::unique_ptr<vtbackend::Terminal> mirror;
    ScreenMirror reserializer;

    MirrorHarness()
    {
        auto settings = gipSettings();
        mirror = std::make_unique<vtbackend::Terminal>(mirrorEvents,
                                                       std::make_unique<vtpty::MockPty>(settings.pageSize),
                                                       std::move(settings),
                                                       std::chrono::steady_clock::now());
        client->setUpdateHandler(
            [this](muxserver::client::RemoteScreen const& screen, proto::Delta const& delta) {
                mirror->writeToScreen(reserializer.apply(screen, delta));
            });
        client->setImageHandler([this](muxserver::client::RemoteScreen const& screen, uint32_t imageId) {
            mirror->writeToScreen(reserializer.applyImage(screen, imageId));
        });
        client->setSessionEventHandler(
            [this](muxserver::client::RemoteScreen const& screen, proto::SessionEvent const& event) {
                std::ignore = screen;
                mirror->writeToScreen(ScreenMirror::applyEvent(event));
            });
        // Deliver the host's stream fan-out (bell / notify / clipboard, and screen
        // updates) to the session, exactly as the daemon's serveNativeClient does —
        // so the transient-event path (Terminal::Events -> host -> NativeSession) is
        // exercised, not just the manually-poked sessionScreenUpdated.
        host.subscribeStream(server.get());
    }

    ~MirrorHarness() { host.unsubscribeStream(server.get()); }

    MirrorHarness(MirrorHarness const&) = delete;
    MirrorHarness& operator=(MirrorHarness const&) = delete;
    MirrorHarness(MirrorHarness&&) = delete;
    MirrorHarness& operator=(MirrorHarness&&) = delete;

    [[nodiscard]] vtbackend::Terminal* serverTerminal(vtmux::SessionId session)
    {
        return host.terminal(session);
    }
};

Task<void> waitUntil(net::EventLoop* loop, std::function<bool()> ready)
{
    for (auto i = 0; i < 1000 && !ready(); ++i)
        co_await loop->delay(1ms);
}

Task<void> drive(MirrorHarness* h, Task<void> scenario)
{
    co_await coro::whenAll(h->server->run(), h->client->run(), std::move(scenario));
}

/// Writes @p bytes on the server terminal and pushes the resulting delta.
void serverWrites(MirrorHarness* h, vtmux::SessionId session, std::string_view bytes)
{
    h->serverTerminal(session)->writeToScreen(bytes);
    h->server->sessionScreenUpdated(session);
}

/// One 5x1 wire row at @p stableId.
proto::WireLine rowAt(int64_t stableId)
{
    auto line = proto::WireLine {};
    line.stableId = stableId;
    line.columns = 5;
    return line;
}

} // namespace

TEST_CASE("a server-side clear drops the mirror's local scrollback", "[muxserver][mirror]")
{
    // Unit-level: drive ScreenMirror directly. The mirror is primed with a screen
    // that has scrollback (floor below the viewport).
    auto mirror = ScreenMirror {};
    auto screen = muxserver::client::RemoteScreen {};
    screen.columns = 5;
    screen.lines = 1;

    auto seed = proto::Delta {};
    seed.snapshot = 1;
    seed.stableViewportBase = 10;
    seed.stableFloor = 7;
    seed.lines.push_back(rowAt(10));
    screen.apply(seed);
    std::ignore = mirror.apply(screen, seed); // primes (full replay), remembers floor 7

    // An ordinary incremental delta (floor unchanged) stays incremental: it must
    // NOT clear scrollback.
    auto tick = proto::Delta {};
    tick.stableViewportBase = 10;
    tick.stableFloor = 7;
    tick.lines.push_back(rowAt(10));
    screen.apply(tick);
    auto const incremental = mirror.apply(screen, tick);
    CHECK_FALSE(incremental.contains("\033[3J"));

    // A `clear`/CSI 3 J jumps the floor to the viewport base with NO line change
    // and NO generation bump. The incremental path would leave ghost scrollback,
    // so the mirror must fall back to a full replay that re-emits ESC[3J.
    auto cleared = proto::Delta {};
    cleared.stableViewportBase = 10;
    cleared.stableFloor = 10;
    screen.apply(cleared);
    auto const out = mirror.apply(screen, cleared);
    CHECK(out.contains("\033[3J"));
}

TEST_CASE("the mirror terminal reproduces text, SGR and cursor", "[muxserver][mirror]")
{
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    h.serverTerminal(session)->writeToScreen("plain \033[1;31mbold-red\033[0m \033[4;58;5;33munder\033[0m");

    auto scenario = [](MirrorHarness* h, vtmux::SessionId session) -> Task<void> {
        co_await waitUntil(&h->loop, [&] {
            return h->mirror->primaryScreen().grid().renderMainPageText().contains("bold-red");
        });

        auto const& serverGrid = h->serverTerminal(session)->primaryScreen().grid();
        auto const& mirrorGrid = h->mirror->primaryScreen().grid();
        CHECK(mirrorGrid.renderMainPageText() == serverGrid.renderMainPageText());

        // The SGR state made it: bold flag and colors of "bold-red"'s first cell.
        auto const& serverRow = serverGrid.lineAt(vtbackend::LineOffset(0)).storage();
        auto const& mirrorRow = mirrorGrid.lineAt(vtbackend::LineOffset(0)).storage();
        auto const column = 6; // first cell of "bold-red"
        CHECK(mirrorRow.sgr[column].flags == serverRow.sgr[column].flags);
        CHECK(mirrorRow.sgr[column].foregroundColor == serverRow.sgr[column].foregroundColor);
        auto const underColumn = 15; // first cell of "under"
        CHECK(mirrorRow.sgr[underColumn].underlineColor == serverRow.sgr[underColumn].underlineColor);

        // The cursor landed where the server's cursor is.
        CHECK(h->mirror->primaryScreen().cursor().position
              == h->serverTerminal(session)->primaryScreen().cursor().position);

        h->client->detach();
    }(&h, session);

    h.loop.blockOn(drive(&h, std::move(scenario)));
}

TEST_CASE("DEC pages beyond primary/alternate mirror faithfully", "[muxserver][mirror]")
{
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    h.serverTerminal(session)->writeToScreen("page-zero-here");

    auto scenario = [](MirrorHarness* h, vtmux::SessionId session) -> Task<void> {
        // Page 0 (primary) mirrors onto the mirror's primary buffer.
        co_await waitUntil(&h->loop, [&] {
            return h->mirror->primaryScreen().grid().renderMainPageText().contains("page-zero-here");
        });
        CHECK(!h->mirror->isAlternateScreen());

        // Live-switch to DEC page 1 (NP) and write there. DECPCCM couples the
        // display by default, so the user now sees page 1 — which shares the
        // "Alternate" wire screen-type with pages 2..14 and the xterm alt page, yet
        // is a distinct grid. The daemon must force a resync onto it (the page-index
        // gate, not the primary-vs-alt one, is what catches this).
        serverWrites(h, session, "\033[1Upage-one-here"); // NP -> page 1, then write
        co_await waitUntil(&h->loop, [&] {
            return h->mirror->isAlternateScreen()
                   && h->mirror->alternateScreen().grid().renderMainPageText().contains("page-one-here");
        });
        // The mirror shows page 1's content, not page 0's bleeding through.
        CHECK(!h->mirror->alternateScreen().grid().renderMainPageText().contains("page-zero-here"));
        CHECK(h->mirror->alternateScreen().grid().renderMainPageText()
              == h->serverTerminal(session)->pageAt(vtbackend::PageIndex(1)).grid().renderMainPageText());

        // Switch back to page 0 (PP): the mirror leaves the alternate buffer and
        // page 0's preserved content returns from the daemon's fresh snapshot.
        serverWrites(h, session, "\033[1V"); // PP -> page 0
        co_await waitUntil(&h->loop, [&] {
            return !h->mirror->isAlternateScreen()
                   && h->mirror->primaryScreen().grid().renderMainPageText().contains("page-zero-here");
        });
        CHECK(!h->mirror->primaryScreen().grid().renderMainPageText().contains("page-one-here"));

        h->client->detach();
    }(&h, session);

    h.loop.blockOn(drive(&h, std::move(scenario)));
}

TEST_CASE("a decoupled cursor page hides the mirror's cursor", "[muxserver][mirror]")
{
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    h.serverTerminal(session)->writeToScreen("visible-page-zero");

    auto scenario = [](MirrorHarness* h, vtmux::SessionId session) -> Task<void> {
        // Coupled (the default): the mirror shows page 0 with a visible cursor.
        co_await waitUntil(&h->loop, [&] {
            return h->mirror->primaryScreen().grid().renderMainPageText().contains("visible-page-zero");
        });
        CHECK(h->mirror->isModeEnabled(vtbackend::DECMode::VisibleCursor));

        // Decouple the display from the cursor (DECPCCM reset), then move VT output
        // to page 1. The user keeps looking at page 0, so the fat GUI hides the
        // cursor (it now belongs to an off-screen page) — the daemon must mirror that
        // by withholding DECTCEM even though the app never hid the cursor itself.
        serverWrites(h, session, "\033[?64l\033[1Uhidden-on-page-one"); // DECPCCM off, NP, write
        co_await waitUntil(&h->loop,
                           [&] { return !h->mirror->isModeEnabled(vtbackend::DECMode::VisibleCursor); });
        // The displayed page is still 0: its content stands and page 1 does not bleed
        // through — VT output landed on a page the user is not looking at.
        CHECK(h->mirror->primaryScreen().grid().renderMainPageText().contains("visible-page-zero"));
        CHECK(!h->mirror->primaryScreen().grid().renderMainPageText().contains("hidden-on-page-one"));
        CHECK(!h->mirror->isAlternateScreen());

        h->client->detach();
    }(&h, session);

    h.loop.blockOn(drive(&h, std::move(scenario)));
}

TEST_CASE("scrolled-out rows land in the mirror's local scrollback", "[muxserver][mirror]")
{
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    h.serverTerminal(session)->writeToScreen("first-line");

    auto scenario = [](MirrorHarness* h, vtmux::SessionId session) -> Task<void> {
        co_await waitUntil(&h->loop, [&] {
            return h->mirror->primaryScreen().grid().renderMainPageText().contains("first-line");
        });

        // Scroll far enough that "first-line" leaves the viewport (25 lines).
        auto scrolled = std::string {};
        for (auto i = 0; i < 30; ++i)
            scrolled += std::format("\r\nline-{}", i);
        serverWrites(h, session, scrolled);
        co_await waitUntil(&h->loop, [&] {
            return h->mirror->primaryScreen().grid().renderMainPageText().contains("line-29");
        });

        auto const& serverGrid = h->serverTerminal(session)->primaryScreen().grid();
        auto const& mirrorGrid = h->mirror->primaryScreen().grid();
        CHECK(mirrorGrid.renderMainPageText() == serverGrid.renderMainPageText());
        REQUIRE(mirrorGrid.historyLineCount() == serverGrid.historyLineCount());
        for (auto offset = 1; offset <= unbox<int>(serverGrid.historyLineCount()); ++offset)
            CHECK(mirrorGrid.lineText(vtbackend::LineOffset(-offset))
                  == serverGrid.lineText(vtbackend::LineOffset(-offset)));

        h->client->detach();
    }(&h, session);

    h.loop.blockOn(drive(&h, std::move(scenario)));
}

TEST_CASE("hyperlinks survive the mirror round trip", "[muxserver][mirror]")
{
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    h.serverTerminal(session)->writeToScreen(
        "\033]8;;https://example.com/doc\033\\linked\033]8;;\033\\ plain");

    auto scenario = [](MirrorHarness* h) -> Task<void> {
        co_await waitUntil(&h->loop, [&] {
            return h->mirror->primaryScreen().grid().renderMainPageText().contains("linked");
        });

        auto const& mirrorRow = h->mirror->primaryScreen().grid().lineAt(vtbackend::LineOffset(0)).storage();
        auto const linkId = mirrorRow.hyperlinks[0];
        REQUIRE(linkId != vtbackend::HyperlinkId(0));
        auto const info = h->mirror->hyperlinks().hyperlinkById(linkId);
        REQUIRE(info);
        CHECK(info->uri == "https://example.com/doc");
        // "plain" carries no link.
        CHECK(mirrorRow.hyperlinks[8] == vtbackend::HyperlinkId(0));

        h->client->detach();
    }(&h);

    h.loop.blockOn(drive(&h, std::move(scenario)));
}

TEST_CASE("wide characters and scaled text reproduce in the mirror", "[muxserver][mirror]")
{
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    h.serverTerminal(session)->writeToScreen("wide: 你好 end\r\n\033]66;s=2;Big\033\\ tail");

    auto scenario = [](MirrorHarness* h, vtmux::SessionId session) -> Task<void> {
        co_await waitUntil(&h->loop, [&] {
            return h->mirror->primaryScreen().grid().renderMainPageText().contains("tail");
        });

        auto const& serverGrid = h->serverTerminal(session)->primaryScreen().grid();
        auto const& mirrorGrid = h->mirror->primaryScreen().grid();
        CHECK(mirrorGrid.renderMainPageText() == serverGrid.renderMainPageText());

        // The wide glyph occupies two columns in both.
        auto const& serverWide = serverGrid.lineAt(vtbackend::LineOffset(0)).storage();
        auto const& mirrorWide = mirrorGrid.lineAt(vtbackend::LineOffset(0)).storage();
        CHECK(mirrorWide.widths[6] == serverWide.widths[6]);

        // The OSC 66 block kept its scale, on the head row and the band below.
        auto const& serverScaled = serverGrid.lineAt(vtbackend::LineOffset(1)).storage();
        auto const& mirrorScaled = mirrorGrid.lineAt(vtbackend::LineOffset(1)).storage();
        CHECK(mirrorScaled.scales[0] == 2);
        CHECK(mirrorScaled.scales[0] == serverScaled.scales[0]);
        auto const& serverBand = serverGrid.lineAt(vtbackend::LineOffset(2)).storage();
        auto const& mirrorBand = mirrorGrid.lineAt(vtbackend::LineOffset(2)).storage();
        CHECK(mirrorBand.scales[0] == serverBand.scales[0]);

        h->client->detach();
    }(&h, session);

    h.loop.blockOn(drive(&h, std::move(scenario)));
}

TEST_CASE("input-relevant DEC modes mirror into the local terminal", "[muxserver][mirror]")
{
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    h.serverTerminal(session)->writeToScreen("prompt");

    auto scenario = [](MirrorHarness* h, vtmux::SessionId session) -> Task<void> {
        co_await waitUntil(&h->loop, [&] {
            return h->mirror->primaryScreen().grid().renderMainPageText().contains("prompt");
        });
        CHECK(h->mirror->isModeEnabled(vtbackend::DECMode::VisibleCursor));
        CHECK(!h->mirror->isModeEnabled(vtbackend::DECMode::MouseSGR));

        // The app (vim, say) flips input modes WITHOUT touching any cell —
        // the pure-mode delta must still reach the mirror.
        serverWrites(h, session, "\033[?1h\033[?1000h\033[?1006h\033[?2004h\033[?25l");
        co_await waitUntil(&h->loop, [&] { return h->mirror->isModeEnabled(vtbackend::DECMode::MouseSGR); });
        CHECK(h->mirror->isModeEnabled(vtbackend::DECMode::UseApplicationCursorKeys));
        CHECK(h->mirror->isModeEnabled(vtbackend::DECMode::MouseProtocolNormalTracking));
        CHECK(h->mirror->isModeEnabled(vtbackend::DECMode::BracketedPaste));
        CHECK(!h->mirror->isModeEnabled(vtbackend::DECMode::VisibleCursor));

        // And back off again.
        serverWrites(h, session, "\033[?1l\033[?1000l\033[?1006l\033[?2004l\033[?25h");
        co_await waitUntil(&h->loop, [&] { return !h->mirror->isModeEnabled(vtbackend::DECMode::MouseSGR); });
        CHECK(!h->mirror->isModeEnabled(vtbackend::DECMode::UseApplicationCursorKeys));
        CHECK(!h->mirror->isModeEnabled(vtbackend::DECMode::BracketedPaste));
        CHECK(h->mirror->isModeEnabled(vtbackend::DECMode::VisibleCursor));

        h->client->detach();
    }(&h, session);

    h.loop.blockOn(drive(&h, std::move(scenario)));
}

TEST_CASE("a resize resyncs the mirror through a full replay", "[muxserver][mirror]")
{
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    h.serverTerminal(session)->writeToScreen("before-resize");

    auto scenario = [](MirrorHarness* h, vtmux::SessionId session) -> Task<void> {
        co_await waitUntil(&h->loop, [&] {
            return h->mirror->primaryScreen().grid().renderMainPageText().contains("before-resize");
        });

        // The controller's job on a size change: resize the local terminal,
        // then let the snapshot delta repaint it. The test plays controller.
        h->client->setUpdateHandler(
            [h](muxserver::client::RemoteScreen const& screen, proto::Delta const& delta) {
                auto const size =
                    vtbackend::PageSize { vtbackend::LineCount(static_cast<int>(screen.lines)),
                                          vtbackend::ColumnCount(static_cast<int>(screen.columns)) };
                if (h->mirror->pageSize() != size)
                    h->mirror->resizeScreen(size);
                h->mirror->writeToScreen(h->reserializer.apply(screen, delta));
            });
        h->client->requestResize(100, 40);
        serverWrites(h, session, "\r\nafter-resize");
        co_await waitUntil(&h->loop, [&] {
            return h->mirror->pageSize().columns == vtbackend::ColumnCount(100)
                   && h->mirror->primaryScreen().grid().renderMainPageText().contains("after-resize");
        });

        auto const& serverGrid = h->serverTerminal(session)->primaryScreen().grid();
        auto const& mirrorGrid = h->mirror->primaryScreen().grid();
        CHECK(mirrorGrid.renderMainPageText() == serverGrid.renderMainPageText());

        h->client->detach();
    }(&h, session);

    h.loop.blockOn(drive(&h, std::move(scenario)));
}

TEST_CASE("inline images round-trip into the mirror via GIP", "[muxserver][mirror]")
{
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();

    // Draw a 2-cell-wide, 1-cell-tall image at the home position on the server via a
    // GIP oneshot (StretchToFill, so both cells are covered). The client fetches the
    // pixels and the mirror re-emits them as GIP, materialising image fragments.
    auto const pixels = std::vector<uint8_t>(static_cast<std::size_t>(8 * 8 * 4), 0xC0);
    auto const body = crispy::base64::encode(
        std::string_view { reinterpret_cast<char const*>(pixels.data()), pixels.size() });
    h.serverTerminal(session)->writeToScreen(
        std::format("\033P!go=s,f=3,w=8,h=8,c=2,r=1,z=3;!{}\033\\", body));

    auto scenario = [](MirrorHarness* h, vtmux::SessionId session) -> Task<void> {
        co_await waitUntil(&h->loop, [&] {
            auto const& row = h->mirror->primaryScreen().grid().lineAt(vtbackend::LineOffset(0)).storage();
            return row.imageFragments.has_value() && row.imageFragments->contains(0)
                   && row.imageFragments->contains(1);
        });

        auto const& serverRow =
            h->serverTerminal(session)->primaryScreen().grid().lineAt(vtbackend::LineOffset(0)).storage();
        auto const& mirrorRow = h->mirror->primaryScreen().grid().lineAt(vtbackend::LineOffset(0)).storage();
        REQUIRE(serverRow.imageFragments.has_value());
        REQUIRE(mirrorRow.imageFragments.has_value());
        // Every cell the server covered with the image is covered in the mirror.
        for (auto const& [column, fragment]: *serverRow.imageFragments)
        {
            std::ignore = fragment;
            CHECK(mirrorRow.imageFragments->contains(column));
        }

        h->client->detach();
    }(&h, session);

    h.loop.blockOn(drive(&h, std::move(scenario)));
}

TEST_CASE("a live window-title change reaches the mirror", "[muxserver][mirror]")
{
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    h.serverTerminal(session)->writeToScreen("work");

    auto scenario = [](MirrorHarness* h, vtmux::SessionId session) -> Task<void> {
        co_await waitUntil(&h->loop, [&] {
            return h->mirror->primaryScreen().grid().renderMainPageText().contains("work");
        });

        // Change ONLY the title (OSC 2, no cell change) — the title-only delta must
        // still push and the mirror must re-title.
        serverWrites(h, session, "\033]2;my-title\033\\");
        co_await waitUntil(&h->loop, [&] { return h->mirror->windowTitle() == "my-title"; });
        CHECK(h->mirror->windowTitle() == "my-title");
        CHECK(h->serverTerminal(session)->windowTitle() == "my-title");

        h->client->detach();
    }(&h, session);

    h.loop.blockOn(drive(&h, std::move(scenario)));
}

TEST_CASE("bell, notification and clipboard events reach the mirror", "[muxserver][mirror]")
{
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    h.serverTerminal(session)->writeToScreen("ready");

    auto scenario = [](MirrorHarness* h, vtmux::SessionId session) -> Task<void> {
        co_await waitUntil(&h->loop, [&] {
            return h->mirror->primaryScreen().grid().renderMainPageText().contains("ready");
        });

        // Bell (BEL) — a transient event, re-emitted as BEL into the mirror.
        serverWrites(h, session, "\a");
        co_await waitUntil(&h->loop, [&] { return h->mirrorEvents.bells > 0; });
        CHECK(h->mirrorEvents.bells > 0);

        // Desktop notification (OSC 777 notify;title;body) → mirror's notify().
        serverWrites(h, session, "\033]777;notify;Build;done ok\033\\");
        co_await waitUntil(&h->loop, [&] { return h->mirrorEvents.notifyTitle == "Build"; });
        CHECK(h->mirrorEvents.notifyBody == "done ok");

        // Clipboard write (OSC 52) → mirror's copyToClipboard() with decoded text.
        auto const encoded = crispy::base64::encode(std::string_view { "clip-text" });
        serverWrites(h, session, std::format("\033]52;c;{}\033\\", encoded));
        co_await waitUntil(&h->loop, [&] { return h->mirrorEvents.clipboard == "clip-text"; });
        CHECK(h->mirrorEvents.clipboard == "clip-text");

        h->client->detach();
    }(&h, session);

    h.loop.blockOn(drive(&h, std::move(scenario)));
}

TEST_CASE("a live cursor-shape change reaches the mirror", "[muxserver][mirror]")
{
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    h.serverTerminal(session)->writeToScreen("x");

    auto scenario = [](MirrorHarness* h, vtmux::SessionId session) -> Task<void> {
        co_await waitUntil(
            &h->loop, [&] { return h->mirror->primaryScreen().grid().renderMainPageText().contains("x"); });

        // DECSCUSR steady bar (Ps=6): a cursor-shape-only change must push and
        // re-shape the mirror's cursor.
        serverWrites(h, session, "\033[6 q");
        co_await waitUntil(&h->loop, [&] { return h->mirror->cursorShape() == vtbackend::CursorShape::Bar; });
        CHECK(h->mirror->cursorShape() == vtbackend::CursorShape::Bar);
        CHECK(h->serverTerminal(session)->cursorShape() == vtbackend::CursorShape::Bar);

        h->client->detach();
    }(&h, session);

    h.loop.blockOn(drive(&h, std::move(scenario)));
}

TEST_CASE("a live working-directory change reaches the mirror", "[muxserver][mirror]")
{
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    h.serverTerminal(session)->writeToScreen("y");

    auto scenario = [](MirrorHarness* h, vtmux::SessionId session) -> Task<void> {
        co_await waitUntil(
            &h->loop, [&] { return h->mirror->primaryScreen().grid().renderMainPageText().contains("y"); });

        // OSC 7: a cwd-only change must push and reach the mirror terminal's
        // currentWorkingDirectory (which the GUI queries for split-in-same-dir).
        serverWrites(h, session, "\033]7;file:///home/user/project\033\\");
        co_await waitUntil(
            &h->loop, [&] { return h->mirror->currentWorkingDirectory() == "file:///home/user/project"; });
        CHECK(h->mirror->currentWorkingDirectory() == "file:///home/user/project");
        CHECK(h->serverTerminal(session)->currentWorkingDirectory() == "file:///home/user/project");

        h->client->detach();
    }(&h, session);

    h.loop.blockOn(drive(&h, std::move(scenario)));
}

TEST_CASE("a live default-color change reaches the mirror", "[muxserver][mirror]")
{
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    h.serverTerminal(session)->writeToScreen("z");

    auto scenario = [](MirrorHarness* h, vtmux::SessionId session) -> Task<void> {
        co_await waitUntil(
            &h->loop, [&] { return h->mirror->primaryScreen().grid().renderMainPageText().contains("z"); });

        // OSC 10/11: change the default foreground/background (no cell change).
        serverWrites(h, session, "\033]10;rgb:12/34/56\033\\\033]11;rgb:ab/cd/ef\033\\");
        co_await waitUntil(&h->loop,
                           [&] { return h->mirror->colorPalette().defaultForeground.value() == 0x123456; });
        CHECK(h->mirror->colorPalette().defaultForeground.value() == 0x123456);
        CHECK(h->mirror->colorPalette().defaultBackground.value() == 0xABCDEF);
        // The server saw the same change.
        CHECK(h->serverTerminal(session)->colorPalette().defaultForeground.value() == 0x123456);

        h->client->detach();
    }(&h, session);

    h.loop.blockOn(drive(&h, std::move(scenario)));
}

TEST_CASE("the status-display state reaches the mirror", "[muxserver][mirror]")
{
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    h.serverTerminal(session)->writeToScreen("s");

    auto scenario = [](MirrorHarness* h, vtmux::SessionId session) -> Task<void> {
        co_await waitUntil(
            &h->loop, [&] { return h->mirror->primaryScreen().grid().renderMainPageText().contains("s"); });

        // DECSSDT 1: show the indicator status line (no change to the main grid) —
        // the first slice of multi-page support beyond primary/alternate.
        serverWrites(h, session, "\033[1$~");
        co_await waitUntil(&h->loop, [&] {
            return h->mirror->statusDisplayType() == vtbackend::StatusDisplayType::Indicator;
        });
        CHECK(h->mirror->statusDisplayType() == vtbackend::StatusDisplayType::Indicator);
        CHECK(h->serverTerminal(session)->statusDisplayType() == vtbackend::StatusDisplayType::Indicator);

        h->client->detach();
    }(&h, session);

    h.loop.blockOn(drive(&h, std::move(scenario)));
}

TEST_CASE("host-writable status-line content reaches the mirror", "[muxserver][mirror]")
{
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    h.serverTerminal(session)->writeToScreen("body");

    auto scenario = [](MirrorHarness* h, vtmux::SessionId session) -> Task<void> {
        co_await waitUntil(&h->loop, [&] {
            return h->mirror->primaryScreen().grid().renderMainPageText().contains("body");
        });

        // Show the host-writable status line (DECSSDT 2), switch writes to it
        // (DECSASD 1), write custom content, switch back to the main display
        // (DECSASD 0). The app's status-line text must reach the mirror's status page.
        serverWrites(h, session, "\033[2$~\033[1$}STATUSBAR\033[0$}");
        // Sanity: the server itself put the content on its host-writable status line.
        REQUIRE(h->serverTerminal(session)->statusDisplayType()
                == vtbackend::StatusDisplayType::HostWritable);
        REQUIRE(h->serverTerminal(session)
                    ->hostWritableStatusLineDisplay()
                    .grid()
                    .lineText(vtbackend::LineOffset(0))
                    .contains("STATUSBAR"));
        co_await waitUntil(&h->loop, [&] {
            return h->mirror->hostWritableStatusLineDisplay()
                .grid()
                .lineText(vtbackend::LineOffset(0))
                .contains("STATUSBAR");
        });
        CHECK(h->mirror->hostWritableStatusLineDisplay()
                  .grid()
                  .lineText(vtbackend::LineOffset(0))
                  .contains("STATUSBAR"));
        // The main grid is untouched.
        CHECK(h->mirror->primaryScreen().grid().renderMainPageText().contains("body"));

        h->client->detach();
    }(&h, session);

    h.loop.blockOn(drive(&h, std::move(scenario)));
}
