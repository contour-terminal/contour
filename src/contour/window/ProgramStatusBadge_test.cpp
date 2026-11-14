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

#include <algorithm>
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

using contour::window::AllRecordsSeen;
using contour::window::badgeFor;
using contour::window::BadgeKind;
using contour::window::disarmInvisibleFormatting;
using contour::window::elided;
using contour::window::ProgramStatusBadge;
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
                                 .message = std::move(message),
                                 .generation = 1 };
}

/// @p codepoints as UTF-8.
std::string utf8(std::initializer_list<char32_t> codepoints)
{
    return unicode::convert_to<char>(std::u32string_view { codepoints.begin(), codepoints.size() });
}

/// The badge @p records earn in a background tab.
ProgramStatusBadge background(std::vector<ProgramStatusRecord> const& records)
{
    return badgeFor(records, 0);
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
    CHECK(badgeFor(std::vector { recordOf(ProgramState::Done) }, AllRecordsSeen).kind == BadgeKind::None);
    CHECK(background({ recordOf(ProgramState::Done) }).kind == BadgeKind::Done);
    CHECK(background({ recordOf(ProgramState::Error), recordOf(ProgramState::Working) }).kind
          == BadgeKind::Error);

    // In front, a failure is already on screen; what still runs shows.
    CHECK(badgeFor(std::vector { recordOf(ProgramState::Error), recordOf(ProgramState::Working) },
                   AllRecordsSeen)
              .kind
          == BadgeKind::Working);
    CHECK(badgeFor(std::vector { recordOf(ProgramState::Blocked) }, AllRecordsSeen).kind
          == BadgeKind::Blocked);
}

TEST_CASE("badgeFor.doneOnlyWhileUnseen", "[contour][window][programstatus]")
{
    auto done = recordOf(ProgramState::Done);
    done.generation = 5;
    auto error = recordOf(ProgramState::Error);
    error.generation = 5;
    CHECK(badgeFor(std::vector { done }, 4).kind == BadgeKind::Done);
    CHECK(badgeFor(std::vector { done }, 5).kind == BadgeKind::None);
    CHECK(badgeFor(std::vector { error }, 4).kind == BadgeKind::Error);
    CHECK(badgeFor(std::vector { error }, 5).kind == BadgeKind::None);
    // A seen failure no longer hides what still runs, nor shadows a newer one.
    auto working = recordOf(ProgramState::Working);
    CHECK(badgeFor(std::vector { error, working }, 5).kind == BadgeKind::Working);
}

TEST_CASE("badgeFor.blockedIgnoresSeen", "[contour][window][programstatus]")
{
    auto blocked = recordOf(ProgramState::Blocked);
    blocked.generation = 3;
    auto working = recordOf(ProgramState::Working);
    working.generation = 3;
    CHECK(badgeFor(std::vector { blocked }, 3).kind == BadgeKind::Blocked);
    CHECK(badgeFor(std::vector { blocked }, AllRecordsSeen).kind == BadgeKind::Blocked);
    CHECK(badgeFor(std::vector { working }, AllRecordsSeen).kind == BadgeKind::Working);
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
    // The same table as the daemon's: what it lacked before -- the Arabic letter mark, line separators, tag
    // characters, prepended concatenation marks, Hangul fillers -- and line breaks shown as one space.
    CHECK(disarmInvisibleFormatting(utf8({ U'a', 0x061C, 0x0600, 0x3164, 0xFFA0, 0xE0041, U'b' })) == "ab");
    CHECK(disarmInvisibleFormatting(utf8({ U'a', U'\r', U'\n', 0x2028, U'b' })) == "a b");
    CHECK(disarmInvisibleFormatting("a\nb") == "a b");
    CHECK(disarmInvisibleFormatting("\r\ndone it\n") == "done it"); // no space at either end
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
                                                    .message = {} },
                              {});
    std::ignore = first.apply(ProgramStatusRecord { .id = "eu",
                                                    .state = ProgramState::Working,
                                                    .kind = {},
                                                    .progress = {},
                                                    .app = {},
                                                    .title = {},
                                                    .message = {} },
                              {});
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
    /// @param offered What the client offers; by default program status without its timing, as a daemon
    ///        that predates it shares.
    explicit DaemonMirror(vthost::proto::CapabilitySet offered =
                              vthost::proto::CapabilitySet::of({ vthost::proto::Capability::ProgramStatus })):
        offered { std::move(offered) }
    {
    }

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
    vthost::proto::CapabilitySet offered;
    vthost::client::NativeClient client {
        loop,
        std::move(pair.second),
        vthost::client::NativeClient::HandshakeOptions { .capabilities = offered },
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
    /// @param seen The newest generation the user has seen.
    ProgramStatusBadge mirroredBadge(uint64_t seen = 0)
    {
        auto const _ = std::lock_guard { mirror };
        return badgeFor(contour::window::resolvedRecords(mirror.programStatus()), seen);
    }

    /// @return The newest generation among the mirror's records: what a look at the tab sees.
    uint64_t newestGeneration()
    {
        auto const _ = std::lock_guard { mirror };
        auto newest = uint64_t { 0 };
        for (auto const& record: mirror.programStatus().records())
            newest = std::max(newest, record.generation);
        return newest;
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

TEST_CASE("a done the user has seen stays seen when the daemon sends it again",
          "[contour][window][programstatus]")
{
    using vthost::proto::Capability;
    using vthost::proto::CapabilitySet;
    // Against a daemon that carries the generations and one that predates them: either way a Done keeps the
    // generation it had, so the set coming again -- as it does whenever any record changes -- does not show
    // it anew.
    for (auto const& offered:
         { CapabilitySet::of({ Capability::ProgramStatus }),
           CapabilitySet::of({ Capability::ProgramStatus, Capability::ProgramStatusTiming }) })
    {
        auto h = DaemonMirror { offered };
        auto const session = vthost::testing::newSession(h.host);

        auto scenario = [](DaemonMirror* h, vtworkspace::SessionId session) -> core::async::Task<void> {
            h->host.terminal(session)->writeToScreen("\033]7501;state=done:app=cargo\033\\");
            h->server->sessionScreenUpdated(session);
            co_await waitUntil(&h->loop, [h] { return h->mirroredBadge().kind == BadgeKind::Done; });
            CHECK(h->mirroredBadge().kind == BadgeKind::Done); // unseen: shown, also from an older daemon
            auto const seen = h->newestGeneration();
            CHECK(h->mirroredBadge(seen).kind == BadgeKind::None);

            h->host.terminal(session)->writeToScreen("\033]7501;state=working:id=other\033\\");
            h->server->sessionScreenUpdated(session);
            co_await waitUntil(&h->loop,
                               [h, seen] { return h->mirroredBadge(seen).kind != BadgeKind::None; });
            // Done outranks Working: had the repeated Done read as news, it would be the badge.
            CHECK(h->mirroredBadge(seen).kind == BadgeKind::Working);

            h->client.detach();
        }(&h, session);

        h.loop.blockOn(drive(&h, std::move(scenario)));
    }
}

TEST_CASE("elided cuts at a character, never inside one", "[contour][window][programstatus]")
{
    CHECK(elided("short", 10) == "short");
    CHECK(elided("exactly-ten", 11) == "exactly-ten");
    CHECK(elided("a long enough text", 8) == "a long …");
    // An emoji ZWJ sequence is five codepoints and one character: it goes whole or stays whole.
    auto const family = std::string { "👨‍👩‍👧" };
    CHECK(elided("a" + family + "bcd", 3) == "a" + family + "…");
    CHECK(elided("a" + family + "bcd", 2) == "a…");
    // A CJK character is one character, whatever its width.
    CHECK(elided("日本語のテキスト", 4) == "日本語…");
}
