// SPDX-License-Identifier: Apache-2.0
#include <contour/window/ProgramStatusBadge.hpp>

#include <vtbackend/screen/Terminal.hpp>
#include <vtbackend/screen/TerminalClocks.hpp>

#include <vtpty/MockPty.hpp>

#include <core/async/WhenAll.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/testing/InMemoryTransport.hpp>
#include <core/platform/NativeFileSystem.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include <vthost/NativeSession.hpp>
#include <vthost/SessionHost.hpp>
#include <vthost/client/NativeClient.hpp>
#include <vthost/client/ScreenMirror.hpp>
#include <vthost/testing/TestHelpers.hpp>

using contour::window::badgeFor;
using contour::window::BadgeKind;
using contour::window::disarmInvisibleFormatting;
using contour::window::ProgramStatusBadge;
using contour::window::TabFocus;
using vtbackend::ProgramState;
using vtbackend::ProgramStatusRecord;

namespace
{

/// A record of @p state, with the texts given.
ProgramStatusRecord recordOf(ProgramState state, std::string message = {}, std::string title = {})
{
    return ProgramStatusRecord { .id = {},
                                 .state = state,
                                 .kind = vtbackend::BlockedKind::None,
                                 .progress = std::nullopt,
                                 .app = "app",
                                 .title = std::move(title),
                                 .message = std::move(message) };
}

/// @p codepoints as UTF-8.
std::string utf8(std::initializer_list<char32_t> codepoints)
{
    return unicode::convert_to<char>(std::u32string_view { codepoints.begin(), codepoints.size() });
}

/// The badge @p records earn in a background tab.
ProgramStatusBadge background(std::vector<ProgramStatusRecord> const& records)
{
    return badgeFor(records, TabFocus::Background);
}

} // namespace

TEST_CASE("badgeFor picks the most urgent record", "[contour][window][programstatus]")
{
    CHECK(background({ recordOf(ProgramState::Working) }).kind == BadgeKind::Working);

    auto const blocked =
        background({ recordOf(ProgramState::Done), recordOf(ProgramState::Blocked, "Approve?") });
    CHECK(blocked.kind == BadgeKind::Blocked);
    CHECK(blocked.text == "Approve?");

    CHECK(background({ recordOf(ProgramState::Idle) }).kind == BadgeKind::None);
    CHECK(background({}).kind == BadgeKind::None);
    CHECK(badgeFor(std::vector { recordOf(ProgramState::Done) }, TabFocus::Foreground).kind
          == BadgeKind::None);
    CHECK(background({ recordOf(ProgramState::Done) }).kind == BadgeKind::Done);
    CHECK(background({ recordOf(ProgramState::Error), recordOf(ProgramState::Working) }).kind
          == BadgeKind::Error);

    // In front, a failure is already on screen; what still runs shows.
    CHECK(badgeFor(std::vector { recordOf(ProgramState::Error), recordOf(ProgramState::Working) },
                   TabFocus::Foreground)
              .kind
          == BadgeKind::Working);
    CHECK(badgeFor(std::vector { recordOf(ProgramState::Blocked) }, TabFocus::Foreground).kind
          == BadgeKind::Blocked);
}

TEST_CASE("badgeFor shows the newest record's progress and text", "[contour][window][programstatus]")
{
    auto older = recordOf(ProgramState::Working, "older");
    older.progress = 10;
    auto newer = recordOf(ProgramState::Working, "newer");
    newer.progress = 70;
    auto const badge = background({ older, newer });
    CHECK(badge.kind == BadgeKind::Working);
    CHECK(badge.progress == uint8_t { 70 });
    CHECK(badge.text == "newer");

    // The title before the message, the message before the app.
    CHECK(background({ recordOf(ProgramState::Done, "message", "Title") }).text == "Title");
    CHECK(background({ recordOf(ProgramState::Done) }).text == "app");

    // A progress belongs to a spinner only.
    auto blocked = recordOf(ProgramState::Blocked);
    blocked.progress = 50;
    CHECK(!background({ blocked }).progress.has_value());
}

TEST_CASE("badge text is disarmed", "[contour][window][programstatus]")
{
    // Spelled by code point: the characters under test must not hide in this file's own literals.
    CHECK(disarmInvisibleFormatting(utf8({ U'a', 0x202E, U'b' })) == "ab");
    CHECK(disarmInvisibleFormatting(utf8({ 0x200B, U'z', 0x200F, U'e', 0x2066, U'r', 0x2069, U'o', 0xFEFF }))
          == "zero");
    auto const kept = utf8({ U'c', U'a', U'f', 0x00E9, U' ', 0x2192, U' ', U'o', U'k' });
    CHECK(disarmInvisibleFormatting(kept) == kept);
    CHECK(disarmInvisibleFormatting("a\x1b[31mb") == "a[31mb");
    CHECK(background({ recordOf(ProgramState::Blocked, "rm -rf " + utf8({ 0x202E }) + "/ ?") }).text
          == "rm -rf / ?");

    // Cut at 120 characters, the last an ellipsis; counted in characters, not bytes.
    auto const longText = std::string(200, 'x');
    auto const cut = background({ recordOf(ProgramState::Done, longText) }).text;
    CHECK(cut == std::string(119, 'x') + utf8({ 0x2026 }));
    auto const wide = utf8({ 0x00E9 }) + std::string(119, 'x');
    CHECK(background({ recordOf(ProgramState::Done, wide) }).text == wide);
}

TEST_CASE("a tab's badge is computed once until it is dropped", "[contour][window][programstatus]")
{
    // Each computation takes every pane's terminal lock; the strip's three roles must share one.
    auto cache = contour::window::TabBadgeCache {};
    auto computed = 0;
    auto badge = ProgramStatusBadge { .kind = BadgeKind::Working, .progress = 40, .text = "build" };
    auto const compute = [&] {
        ++computed;
        return badge;
    };
    auto const tab = vtworkspace::TabId { 7 };
    auto const other = vtworkspace::TabId { 8 };

    CHECK(cache.get(tab, compute) == badge);
    CHECK(cache.get(tab, compute).text == "build");
    CHECK(cache.get(tab, compute).progress == 40);
    CHECK(computed == 1);

    // Another tab has its own.
    std::ignore = cache.get(other, compute);
    CHECK(computed == 2);

    // Dropped when its row changes: the next ask sees the new records.
    badge = ProgramStatusBadge { .kind = BadgeKind::Done, .progress = std::nullopt, .text = "built" };
    cache.drop(tab);
    CHECK(cache.get(tab, compute).kind == BadgeKind::Done);
    CHECK(computed == 3);
    CHECK(cache.get(other, compute).kind == BadgeKind::Working);
    CHECK(computed == 3);

    // All dropped when the tab in front changes.
    cache.clear();
    CHECK(cache.get(other, compute).kind == BadgeKind::Done);
    CHECK(computed == 4);
}

TEST_CASE("a tab's badge pools its panes' records, each with its own pane's app",
          "[contour][window][programstatus]")
{
    auto first = vtbackend::ProgramStatusSet {};
    std::ignore = first.apply(ProgramStatusRecord { .id = "",
                                                    .state = ProgramState::Idle,
                                                    .kind = {},
                                                    .progress = {},
                                                    .app = "deploy",
                                                    .title = {},
                                                    .message = {} });
    std::ignore = first.apply(ProgramStatusRecord { .id = "eu",
                                                    .state = ProgramState::Working,
                                                    .kind = {},
                                                    .progress = {},
                                                    .app = {},
                                                    .title = {},
                                                    .message = {} });
    auto const resolved = contour::window::resolvedRecords(first);
    REQUIRE(resolved.size() == 2);
    CHECK(resolved.back().app == "deploy");
    CHECK(background(resolved).text == "deploy");
}

namespace
{

/// A daemon serving one session over a real socket, and a client mirroring it into a local terminal, as the
/// GUI's default (daemon) mode does: the badge must reach the tab strip that way too.
struct DaemonMirror
{
    std::unique_ptr<core::net::IoBackend> source = core::net::makeDefaultBackend();
    core::net::EventLoop loop { *source };
    vthost::SessionHost host { loop,
                               [](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const&) {
                                   return std::make_unique<vtpty::MockPty>(size);
                               },
                               vtbackend::Settings {},
                               core::defaultEnvironment(),
                               vthost::SessionHostConfig { .pumping = vthost::SessionPumping::Manual } };
    core::net::testing::SocketPair pair = *core::net::testing::makeSocketPair(loop);
    std::unique_ptr<vthost::NativeSession> server = std::make_unique<vthost::NativeSession>(
        loop, host, vthost::ConnectionId { .endpoint = "test", .index = 1 }, std::move(pair.first));
    vtbackend::Terminal::NullEvents mirrorEvents;
    vtbackend::Terminal mirror { mirrorEvents,
                                 core::defaultEnvironment(),
                                 core::platform::NativeFileSystem::instance(),
                                 std::make_unique<vtpty::MockPty>(vtbackend::Settings {}.pageSize),
                                 vtbackend::Settings {},
                                 std::nullopt,
                                 vtbackend::TerminalClocks::system(),
                                 vtbackend::randomSemanticBlockTokens() };
    vthost::client::ScreenMirror populator { mirror, vthost::DaemonMachine::Same };
    vthost::client::NativeClient client {
        loop,
        std::move(pair.second),
        // What the GUI offers for program status (remote/NativeController.cpp).
        vthost::client::NativeClient::HandshakeOptions {
            .capabilities = vthost::proto::CapabilitySet::of({ vthost::proto::Capability::ProgramStatus }) },
        vtbackend::CommandBlockStoreLimits {},
        [this](vthost::client::RemoteScreen const& screen, vthost::proto::Delta const& delta) {
            populator.apply(screen, delta);
        },
        [this](vthost::client::RemoteScreen const& screen, uint32_t imageId) {
            populator.applyImage(screen, imageId);
        },
        [this](vthost::client::RemoteScreen const&, vthost::proto::SessionEventPdu const& event) {
            populator.applyEvent(event);
        },
        {}
    };

    /// The badge the mirror's records earn in a background tab.
    ProgramStatusBadge mirroredBadge()
    {
        auto const _ = std::lock_guard { mirror };
        return badgeFor(contour::window::resolvedRecords(mirror.programStatus()), TabFocus::Background);
    }
};

/// Waits on @p loop until @p ready holds, a second at most.
core::async::Task<void> waitUntil(core::net::EventLoop* loop, std::function<bool()> ready)
{
    for (auto tick = 0; tick < 1000 && !ready(); ++tick)
        co_await loop->delay(std::chrono::milliseconds { 1 });
}

/// Runs the daemon, the client and @p scenario until all three are done.
core::async::Task<void> drive(DaemonMirror* h, core::async::Task<void> scenario)
{
    co_await core::async::whenAll(h->server->run(), h->client.run(), std::move(scenario));
}

} // namespace

TEST_CASE("a daemon-backed mirror shows the hosted pane's badge", "[contour][window][programstatus]")
{
    auto h = DaemonMirror {};
    auto const session = vthost::testing::newSession(h.host);

    auto scenario = [](DaemonMirror* h, vtworkspace::SessionId session) -> core::async::Task<void> {
        h->host.terminal(session)->writeToScreen(
            "\033]7501;state=working:app=cargo:progress=40\033\\"
            "\033]7501;state=blocked:id=build:kind=permission:msg=QXBwcm92ZT8\033\\");
        h->server->sessionScreenUpdated(session);
        co_await waitUntil(&h->loop, [h] { return h->mirroredBadge().kind == BadgeKind::Blocked; });
        auto const badge = h->mirroredBadge();
        CHECK(badge.kind == BadgeKind::Blocked);
        CHECK(badge.text == "Approve?");

        h->host.terminal(session)->writeToScreen("\033]7501;state=clear:id=build\033\\");
        co_await waitUntil(&h->loop, [h] { return h->mirroredBadge().kind == BadgeKind::Working; });
        CHECK(h->mirroredBadge()
              == ProgramStatusBadge { .kind = BadgeKind::Working, .progress = 40, .text = "cargo" });

        h->client.detach();
    }(&h, session);

    h.loop.blockOn(drive(&h, std::move(scenario)));
}
