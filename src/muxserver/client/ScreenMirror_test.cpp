// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/Terminal.h>

#include <vtpty/MockPty.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <format>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

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
                       vtbackend::Settings {},
                       /*startPumps=*/false };
    net::testing::SocketPair pair = *net::testing::makeSocketPair(loop);
    std::unique_ptr<NativeSession> server =
        std::make_unique<NativeSession>(loop, host, std::move(pair.first));
    std::unique_ptr<AttachClient> client = std::make_unique<AttachClient>(loop, std::move(pair.second));

    vtbackend::Terminal::NullEvents mirrorEvents;
    std::unique_ptr<vtbackend::Terminal> mirror;
    ScreenMirror reserializer;

    MirrorHarness()
    {
        auto settings = vtbackend::Settings {};
        mirror = std::make_unique<vtbackend::Terminal>(mirrorEvents,
                                                       std::make_unique<vtpty::MockPty>(settings.pageSize),
                                                       std::move(settings),
                                                       std::chrono::steady_clock::now());
        client->setUpdateHandler(
            [this](muxserver::client::RemoteScreen const& screen, proto::Delta const& delta) {
                mirror->writeToScreen(reserializer.apply(screen, delta));
            });
    }

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

} // namespace

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
