// SPDX-License-Identifier: Apache-2.0
#include <vtpty/MockPty.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <string>

#include <coro/Cancellation.hpp>
#include <coro/Task.hpp>
#include <net/EventLoop.hpp>
#include <net/PollEventSource.hpp>
#include <net/Sockets.hpp>
#include <net/testing/CoroTestSupport.hpp>
#include <net/testing/ScriptedEventSource.hpp>
#include <net/testing/TempDir.hpp>
#include <vthost/ConnectionAcceptor.hpp>
#include <vthost/LastSessionWatcher.hpp>
#include <vtworkspace/Pane.hpp>
#include <vtworkspace/Tab.hpp>

using namespace std::chrono_literals;

using vthost::LastSessionWatcher;
using vthost::SessionHost;

namespace
{

/// A connection handler that completes immediately. Kept a separate coroutine so the handler lambda
/// itself is not one: a coroutine may not take reference parameters, while an unused by-value
/// ConnectionId parameter is a needless copy — only this split satisfies both.
coro::Task<void> noopConnection()
{
    co_return;
}

/// A host over MockPty sessions with pump threads disabled, plus the watcher under test and a
/// counter standing in for the daemon's shutdown action.
struct WatcherHarness
{
    net::testing::ScriptedEventSource source;
    net::EventLoop loop { source };
    SessionHost host { loop,
                       [](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const&) {
                           return std::make_unique<vtpty::MockPty>(size);
                       },
                       vtbackend::Settings {},
                       crispy::defaultEnvironment(),
                       /*startPumps=*/false };
    int shutdowns = 0;
    LastSessionWatcher watcher { host, loop, [this] { ++shutdowns; } };

    /// Runs the loop's posted callbacks once. `runPostedCallbacks` is the first thing every pump
    /// does, and a zero delay never parks, so this is a deterministic single drain with no real fds.
    void pump() { loop.blockOn(net::testing::sleepFor(&loop, 0ms)); }

    /// Creates a tab and returns its only pane's session.
    [[nodiscard]] vtworkspace::SessionId createSession()
    {
        auto* tab = host.createTab();
        REQUIRE(tab != nullptr);
        return tab->rootPane()->session();
    }
};

} // namespace

TEST_CASE("the last session's exit shuts an exit-when-empty daemon down", "[vthost][lifecycle]")
{
    auto h = WatcherHarness {};
    auto const session = h.createSession();

    h.host.handleSessionExit(session);

    // Not yet: the decision must not be taken inside SessionHost's observer fan-out.
    CHECK(h.shutdowns == 0);

    h.pump();
    CHECK(h.shutdowns == 1);
}

TEST_CASE("a session exit with a sibling left keeps the daemon serving", "[vthost][lifecycle]")
{
    auto h = WatcherHarness {};
    auto* tab = h.host.createTab();
    REQUIRE(tab != nullptr);
    h.host.splitActivePane(tab->id(), vtworkspace::SplitState::Horizontal, 0.5);
    REQUIRE(h.host.sessionCount() == 2);

    h.host.handleSessionExit(tab->rootPane()->first()->session());
    h.pump();

    REQUIRE(h.host.sessionCount() == 1);
    CHECK(h.shutdowns == 0);
}

TEST_CASE("a daemon that never hosted a session is not shut down", "[vthost][lifecycle]")
{
    // The regression test for the edge trigger: runDaemon binds its sockets with zero sessions and
    // the first client's attach spawns one, so a level check here would end the daemon before
    // anybody could attach.
    auto h = WatcherHarness {};
    REQUIRE(h.host.sessionCount() == 0);

    h.pump();

    CHECK(h.shutdowns == 0);
}

TEST_CASE("a session created before the deferred check ran keeps the daemon alive", "[vthost][lifecycle]")
{
    // The reason the decision is deferred AND re-checked. Buffered input is dispatched without
    // touching the reactor, so a `ClosePane` followed by a `CreateTab` — or tmux's `kill-pane` then
    // `new-window` — arriving in ONE read both land before the loop regains control.
    auto h = WatcherHarness {};
    auto const first = h.createSession();

    h.host.handleSessionExit(first);
    std::ignore = h.createSession(); // the rest of the same input batch
    h.pump();

    REQUIRE(h.host.sessionCount() == 1);
    CHECK(h.shutdowns == 0);
}

TEST_CASE("the shutdown is requested once across several exits", "[vthost][lifecycle]")
{
    auto h = WatcherHarness {};
    auto* tab = h.host.createTab();
    REQUIRE(tab != nullptr);
    h.host.splitActivePane(tab->id(), vtworkspace::SplitState::Vertical, 0.5);
    REQUIRE(h.host.sessionCount() == 2);

    auto const first = tab->rootPane()->first()->session();
    auto const second = tab->rootPane()->second()->session();
    h.host.handleSessionExit(first);
    h.host.handleSessionExit(second);
    h.pump();

    REQUIRE(h.host.sessionCount() == 0);
    CHECK(h.shutdowns == 1); // only the zero-transition arms the decision
}

#ifndef _WIN32
TEST_CASE("the last session's exit unwinds the daemon's accept loop", "[vthost][lifecycle]")
{
    // The composition the unit cases above cannot reach: a REAL bound listener served by a REAL
    // accept coroutine, shut down the way runDaemon does it (close the listener, then requestStop).
    // Proves the watcher's decision actually unwinds the serve loop and returns control — the step
    // between "we decided to exit" and "the process exits".
    auto const dir = net::testing::TempDir { "contour-lifecycle" };
    auto const socketPath = (dir / "sock").string();

    auto source = net::PollEventSource {};
    auto loop = net::EventLoop { source };
    auto host = SessionHost { loop,
                              [](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const&) {
                                  return std::make_unique<vtpty::MockPty>(size);
                              },
                              vtbackend::Settings {},
                              crispy::defaultEnvironment(),
                              /*startPumps=*/false };

    auto listener = net::listenUnix(loop, socketPath);
    REQUIRE(listener.has_value());
    auto acceptor =
        vthost::ConnectionAcceptor { loop,
                                     "test",
                                     std::move(*listener),
                                     [](vthost::ConnectionId const&, std::unique_ptr<net::ISocket>) {
                                         return noopConnection();
                                     } };

    auto const watcher = LastSessionWatcher { host, loop, [&] {
                                                 acceptor.close();
                                                 loop.requestStop();
                                             } };
    auto* tab = host.createTab();
    REQUIRE(tab != nullptr);
    auto const session = tab->rootPane()->session();

    // Marshal the exit onto the loop so it lands once the accept loop is already parked, exactly as
    // a shell exit's posted completion does in production.
    loop.post([&] { host.handleSessionExit(session); });

    // Reaching past this call IS the assertion: a watcher that never fired would leave the accept
    // parked and block here forever. serve() co_returns cleanly when accept reports Cancelled
    // rather than throwing, so both outcomes count as "the serve loop ended".
    auto unwound = false;
    try
    {
        loop.blockOn(acceptor.serve());
        unwound = true;
    }
    catch (coro::OperationCancelled const&)
    {
        unwound = true;
    }

    CHECK(unwound);
    CHECK(host.sessionCount() == 0);
    // Closing the unix listener unlinks its socket file, which is what lets the next ensureDaemon
    // probe fail and spawn a fresh daemon instead of finding a dead path.
    CHECK_FALSE(std::filesystem::exists(socketPath));
}
#endif

TEST_CASE("the watcher unsubscribes itself before the host outlives it", "[vthost][lifecycle]")
{
    // Constructing the watcher arms it and destroying it disarms it, so a host that outlives one
    // fans out to nothing — no dangling observer in _streamSubscribers.
    net::testing::ScriptedEventSource source;
    auto loop = net::EventLoop { source };
    auto host = SessionHost { loop,
                              [](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const&) {
                                  return std::make_unique<vtpty::MockPty>(size);
                              },
                              vtbackend::Settings {},
                              crispy::defaultEnvironment(),
                              /*startPumps=*/false };
    auto shutdowns = 0;
    {
        auto const watcher = LastSessionWatcher { host, loop, [&] { ++shutdowns; } };
        (void) watcher;
    }

    auto* tab = host.createTab();
    REQUIRE(tab != nullptr);
    host.handleSessionExit(tab->rootPane()->session());
    loop.blockOn(net::testing::sleepFor(&loop, 0ms));

    CHECK(shutdowns == 0);
}
