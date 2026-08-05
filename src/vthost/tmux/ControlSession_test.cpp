// SPDX-License-Identifier: Apache-2.0
#include <vtpty/MockPty.h>

#include <crispy/logsink.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <coro/WhenAll.hpp>
#include <net/AsyncBufferedReader.h>
#include <net/EventLoop.h>
#include <net/ISocket.h>
#include <net/IoResult.h>
#include <net/PollEventSource.h>
#include <net/testing/InMemoryTransport.h>
#include <vthost/SessionHost.h>
#include <vthost/TappingPty.h>
#include <vthost/tmux/ControlSession.h>
#include <vtworkspace/Pane.h>
#include <vtworkspace/Tab.h>

using coro::Task;
using vthost::SessionHost;
using vthost::tmux::ControlSession;
using vthost::tmux::splitCommandLine;

namespace
{

// Free coroutines with pointer parameters (a capturing lambda coroutine would
// dangle its closure — cppcoreguidelines-avoid-capturing-lambda-coroutines).

/// Writes each command (newline-terminated) then a bare empty line, which the
/// protocol treats as detach: the SESSION ends and closes ITS end, so the
/// collector (reading the same bidirectional socket) sees EOF only after every
/// reply. Closing the client end here would instead tear down both directions
/// and starve the collector.
/// Views @p text as a raw byte span for ISocket::write.
[[nodiscard]] std::span<std::byte const> asBytes(std::string_view text) noexcept
{
    return { reinterpret_cast<std::byte const*>(text.data()), text.size() };
}

Task<void> feedCommands(net::ISocket* client, std::vector<std::string> const* commands)
{
    for (auto const& command: *commands)
    {
        auto const wire = command + "\n";
        std::ignore = co_await client->write(asBytes(wire));
    }
    std::ignore = co_await client->write(asBytes("\n")); // empty line -> detach (control.c:547)
}

/// Collects every line the session emits until the peer closes.
Task<void> collectLines(net::ISocket* client, std::vector<std::string>* out)
{
    auto reader = net::AsyncBufferedReader { client };
    while (true)
    {
        auto line = co_await reader.readLine();
        if (!line.has_value())
            co_return;
        out->push_back(std::move(*line));
    }
}

/// Feeds one pane-output burst, then a notification gated behind it, then the
/// detach line — and NO further pane output. The burst must fully reach the
/// client and the notification must follow it, driven only by the drain
/// continuation: the PTY stays silent after the single burst.
Task<void> burstThenNotifyThenDetach(ControlSession* session,
                                     net::ISocket* client,
                                     vtworkspace::SessionId sessionId,
                                     vtworkspace::TabId tabId,
                                     std::string const* burst)
{
    session->sessionOutput(sessionId, *burst);           // a burst larger than one pump budget
    session->tabTitleChanged(tabId);                     // %window-renamed, gated behind the burst
    std::ignore = co_await client->write(asBytes("\n")); // empty line -> detach
}

/// Runs the session concurrently with the feeder and the collector.
Task<void> driveExchange(ControlSession* session,
                         net::ISocket* client,
                         std::vector<std::string> const* commands,
                         std::vector<std::string>* out)
{
    co_await coro::whenAll(session->run(), feedCommands(client, commands), collectLines(client, out));
}

/// Resizes a pane the way ANOTHER client would — through the host, which applies it and fans it
/// out — then detaches, so the collector sees whatever the session announced.
Task<void> resizeThenDetach(SessionHost* host,
                            net::ISocket* client,
                            vtworkspace::SessionId sessionId,
                            vtbackend::PageSize size)
{
    std::ignore = host->applyPaneSize(sessionId, size);
    std::ignore = co_await client->write(asBytes("\n")); // empty line -> detach
}

/// Runs the session concurrently with the foreign resize and the collector.
Task<void> driveResize(ControlSession* session,
                       SessionHost* host,
                       net::ISocket* client,
                       vtworkspace::SessionId sessionId,
                       vtbackend::PageSize size,
                       std::vector<std::string>* out)
{
    co_await coro::whenAll(
        session->run(), resizeThenDetach(host, client, sessionId, size), collectLines(client, out));
}

/// Runs the session concurrently with the burst producer and the collector.
Task<void> driveBurst(ControlSession* session,
                      net::ISocket* client,
                      vtworkspace::SessionId sessionId,
                      vtworkspace::TabId tabId,
                      std::string const* burst,
                      std::vector<std::string>* out)
{
    co_await coro::whenAll(session->run(),
                           burstThenNotifyThenDetach(session, client, sessionId, tabId, burst),
                           collectLines(client, out));
}

/// An ISocket whose every read fails with a chosen NetError.
///
/// The transport-failure path has no other way in: a socket pair can only deliver a CLEAN
/// close, which is the one case readLine reports as end-of-stream anyway. Writes are accepted
/// and discarded — the session's preamble is not what these cases are about.
class FailingReadSocket final: public net::ISocket
{
  public:
    explicit FailingReadSocket(net::NetError error) noexcept: _error(std::move(error)) {}

    [[nodiscard]] coro::Task<net::IoResult> read(std::span<std::byte> /*buffer*/) override
    {
        co_return std::unexpected(_error);
    }

    [[nodiscard]] coro::Task<net::IoResult> write(std::span<std::byte const> buffer) override
    {
        co_return buffer.size();
    }

    void close() noexcept override { _closed = true; }
    [[nodiscard]] bool isClosed() const noexcept override { return _closed; }

  private:
    net::NetError _error;
    bool _closed = false;
};

/// Drives a ControlSession over an in-memory socket pair: the test writes
/// command lines into one end and reads the protocol replies from it, while the
/// session runs on the other end.
struct ControlHarness
{
    net::PollEventSource source;
    net::EventLoop loop { source };
    SessionHost host;
    net::testing::SocketPair pair = *net::testing::makeSocketPair(loop);
    std::unique_ptr<ControlSession> session;

    /// @param options The session's options.
    /// @param settings Terminal settings for the hosted sessions.
    /// @param connection Transport for the session end; the socket pair's own end when null.
    explicit ControlHarness(ControlSession::Options options = {},
                            vtbackend::Settings settings = {},
                            std::unique_ptr<net::ISocket> connection = nullptr):
        host { loop,
               [](vtbackend::PageSize size) { return std::make_unique<vtpty::MockPty>(size); },
               std::move(settings),
               crispy::defaultEnvironment(),
               /*startPumps=*/false }
    {
        // A fixed clock so guard timestamps are deterministic.
        session = std::make_unique<ControlSession>(
            loop,
            host,
            vthost::ConnectionId { .endpoint = "test", .index = 1 },
            connection ? std::move(connection) : std::move(pair.first),
            [] { return std::int64_t { 1000 }; },
            options);
    }

    /// Runs @p commands (each newline-terminated), then closes the client's
    /// write end so the session's run() loop ends, and returns every line the
    /// session emitted.
    std::vector<std::string> exchange(std::vector<std::string> const& commands)
    {
        auto emitted = std::vector<std::string> {};
        loop.blockOn(driveExchange(session.get(), pair.second.get(), &commands, &emitted));
        return emitted;
    }
};

[[nodiscard]] bool contains(std::vector<std::string> const& lines, std::string_view needle)
{
    return std::ranges::any_of(lines, [&](auto const& line) { return line.contains(needle); });
}

} // namespace

TEST_CASE("splitCommandLine honours single and double quotes", "[vthost][control]")
{
    CHECK(splitCommandLine("new-window") == std::vector<std::string> { "new-window" });
    CHECK(splitCommandLine("send-keys -t %1 'echo hi'")
          == std::vector<std::string> { "send-keys", "-t", "%1", "echo hi" });
    CHECK(splitCommandLine(R"(display-message "a \"b\" c")")
          == std::vector<std::string> { "display-message", "a \"b\" c" });
    CHECK(splitCommandLine("   ").empty());
}

TEST_CASE("the session opens with a guard pair and session-changed", "[vthost][control]")
{
    auto h = ControlHarness {};
    auto const lines = h.exchange({});

    REQUIRE(lines.size() >= 3);
    CHECK(lines[0] == "%begin 1000 0 1");
    CHECK(lines[1] == "%end 1000 0 1");
    CHECK(lines[2] == "%session-changed $0 0");
    CHECK(lines.back() == "%exit");
}

TEST_CASE("a control session reports the transport error that ended it", "[vthost][control]")
{
    // Every readLine failure used to print "end of stream", so a reset or a bad handle was
    // indistinguishable from a peer hanging up cleanly — which is what made a daemon's log
    // unreadable when a second daemon's bind probe poked it.
    auto capture = logstore::scoped_capture { "vthost.tmux" };

    auto h = ControlHarness { {},
                              {},
                              std::make_unique<FailingReadSocket>(
                                  net::makeNetError(net::NetErrorCode::ConnReset, 10054, "recv")) };
    h.loop.blockOn(h.session->run());

    CHECK(capture.contains("connection reset"));
    CHECK(capture.contains("recv"));
    CHECK_FALSE(capture.contains("end of stream"));
}

TEST_CASE("the detach line counts the commands the client issued", "[vthost][control]")
{
    // Zero is a liveness probe's signature — net::listenUnix's bind probe connects and closes
    // without speaking, and the daemon logged that identically to a client that attached and
    // quit. The count is what tells the two apart.
    SECTION("a peer that never speaks")
    {
        auto capture = logstore::scoped_capture { "vthost.tmux" };
        auto h = ControlHarness {};
        std::ignore = h.exchange({});
        CHECK(capture.contains("0 commands"));
    }

    SECTION("a peer that issued commands")
    {
        auto capture = logstore::scoped_capture { "vthost.tmux" };
        auto h = ControlHarness {};
        std::ignore = h.exchange({ "new-window", "list-panes" });
        CHECK(capture.contains("2 commands"));
    }
}

TEST_CASE("new-window creates a tab and notifies %window-add + %layout-change", "[vthost][control]")
{
    auto h = ControlHarness {};
    auto const lines = h.exchange({ "new-window" });

    CHECK(h.host.model().window(h.host.windowId())->tabCount() == 1);
    CHECK(contains(lines, "%window-add @"));
    CHECK(contains(lines, "%layout-change @"));
    // The command's guard block used command number 1 (0 was the opening one).
    CHECK(contains(lines, "%begin 1000 1 1"));
    CHECK(contains(lines, "%end 1000 1 1"));
}

TEST_CASE("split-window adds a pane and re-emits the layout", "[vthost][control]")
{
    auto h = ControlHarness {};
    auto const lines = h.exchange({ "new-window", "split-window -h" });

    auto* tab = h.host.model().window(h.host.windowId())->activeTab();
    REQUIRE(tab != nullptr);
    CHECK(tab->paneCount() == 2);
    // The second %layout-change carries a side-by-side container.
    CHECK(std::ranges::count_if(lines, [](auto const& l) { return l.contains("%layout-change"); }) >= 2);
}

TEST_CASE("an unknown command yields an %error guard", "[vthost][control]")
{
    auto h = ControlHarness {};
    auto const lines = h.exchange({ "frobnicate" });
    CHECK(contains(lines, "%error"));
    CHECK(contains(lines, "unknown command: frobnicate"));
}

TEST_CASE("list-windows reports the created windows inside a guard", "[vthost][control]")
{
    auto h = ControlHarness {};
    auto const lines = h.exchange({ "new-window", "new-window", "list-windows" });

    // Two windows, each on its own body line between a %begin/%end pair.
    CHECK(std::ranges::count_if(lines, [](auto const& l) { return l.starts_with("0: @"); }) == 1);
    CHECK(std::ranges::count_if(lines, [](auto const& l) { return l.starts_with("1: @"); }) == 1);
}

TEST_CASE("send-keys writes into the target pane's PTY", "[vthost][control]")
{
    auto h = ControlHarness {};
    h.host.createTab();
    auto* tab = h.host.model().window(h.host.windowId())->activeTab();
    auto const paneId = tab->rootPane()->id().value;
    auto const session = tab->rootPane()->session();

    h.exchange({ std::format("send-keys -t %{} 'echo hi' Enter", paneId) });

    // The MockPty records stdin; "echo hi" + Enter(\r) must have landed. The
    // host wraps every PTY in a TappingPty for the byte tap, so reach through it.
    auto* terminal = h.host.terminal(session);
    REQUIRE(terminal != nullptr);
    auto& tapped = dynamic_cast<vthost::TappingPty&>(terminal->device());
    auto& mock = dynamic_cast<vtpty::MockPty&>(tapped.inner());
    CHECK(mock.stdinBuffer() == "echo hi\r");
}

TEST_CASE("capture-pane returns the visible text, without SGR by default", "[vthost][control]")
{
    auto h = ControlHarness {};
    h.host.createTab();
    auto* tab = h.host.model().window(h.host.windowId())->activeTab();
    auto const paneId = tab->rootPane()->id().value;
    auto const session = tab->rootPane()->session();
    h.host.terminal(session)->writeToScreen("\033[1;31mRED\033[0m plain\r\n");

    auto const lines = h.exchange({ std::format("capture-pane -p -t %{}", paneId) });

    CHECK(contains(lines, "RED plain")); // the visible text, SGR consumed by the parser
    // No escape sequences leak into a plain capture (the guard/notification lines carry none either).
    CHECK(std::ranges::none_of(lines, [](auto const& l) { return l.contains("\x1b["); }));
}

TEST_CASE("capture-pane trims each row unless -J or -N is given", "[vthost][control]")
{
    // tmux trims every row at its last non-default cell (grid_line_length) and keeps the padding
    // only for -J/-N. Rendering the full page width unconditionally handed every client rows tmux
    // would never emit — and fed Contour's own mirror written space cells across the whole pane, so
    // selecting a replayed region copied lines padded to the pane width.
    auto h = ControlHarness {};
    h.host.createTab();
    auto* tab = h.host.model().window(h.host.windowId())->activeTab();
    auto const paneId = tab->rootPane()->id().value;
    h.host.terminal(tab->rootPane()->session())->writeToScreen("hi");

    auto const width = unbox<std::size_t>(h.host.terminal(tab->rootPane()->session())->pageSize().columns);
    auto const lines = h.exchange({ std::format("capture-pane -p -E 0 -t %{}", paneId),
                                    std::format("capture-pane -p -J -E 0 -t %{}", paneId),
                                    std::format("capture-pane -p -N -E 0 -t %{}", paneId) });

    CHECK(std::ranges::count(lines, "hi") == 1);                               // trimmed by default
    CHECK(std::ranges::count(lines, "hi" + std::string(width - 2, ' ')) == 2); // kept for -J and -N
}

TEST_CASE("a resize another client asked for is announced to this one", "[vthost][control]")
{
    // The host applies a resize and fans it out; the paths that carry it — a native client's
    // applyClientSize, a detach re-resolving the authoritative area — raise no ModelEvents at all,
    // so this callback is the ONLY notice a control-mode client gets. Without it a second client's
    // resize left this one rendering the old cell geometry while the shell behind the pane had
    // already been resized: output wrapping at the wrong column, full-screen apps painting into the
    // wrong rectangle, until the user happened to resize their own window.
    auto h = ControlHarness {};
    h.host.createTab();
    auto* tab = h.host.model().window(h.host.windowId())->activeTab();
    auto const sessionId = tab->rootPane()->session();
    h.host.subscribeStream(h.session.get()); // as the daemon's serveControlClient does

    auto emitted = std::vector<std::string> {};
    h.loop.blockOn(driveResize(h.session.get(),
                               &h.host,
                               h.pair.second.get(),
                               sessionId,
                               vtbackend::PageSize { vtbackend::LineCount(11), vtbackend::ColumnCount(41) },
                               &emitted));
    h.host.unsubscribeStream(h.session.get());

    CHECK(contains(emitted, std::format("%layout-change @{}", tab->id().value)));
}

TEST_CASE("capture-pane stays race-free while the pane is written concurrently",
          "[vthost][control][concurrency]")
{
    auto h = ControlHarness {};
    h.host.createTab();
    auto* tab = h.host.model().window(h.host.windowId())->activeTab();
    auto const paneId = tab->rootPane()->id().value;
    auto* terminal = h.host.terminal(tab->rootPane()->session());
    REQUIRE(terminal != nullptr);

    // A concurrent writer mutates the same grid under _stateMutex (writeToScreen);
    // each capture-pane below reads it under that same lock (the fix). Under TSan an
    // unlocked capture would be flagged racing this writer's line-storage
    // reallocation. (The screen-update writeToScreen posts is drained by the
    // exchange()'s loop, matching the production pump->loop hop.)
    auto stop = std::atomic<bool> { false };
    auto writer = std::thread { [&] {
        for (auto i = 0; !stop.load(std::memory_order_relaxed); ++i)
            terminal->writeToScreen(std::format("line-{}\r\n", i));
    } };

    // exchange() runs the session exactly once (it detaches at the end), so batch
    // many captures into it; each races the writer.
    auto commands = std::vector<std::string> {};
    for (auto i = 0; i < 100; ++i)
        commands.push_back(std::format("capture-pane -p -t %{}", paneId));
    auto const lines = h.exchange(commands);

    stop.store(true, std::memory_order_relaxed);
    writer.join();

    // The captures completed without crashing or deadlocking and produced content;
    // a plain (-p) capture never leaks escape sequences, even mid-write.
    CHECK(!lines.empty());
    CHECK(std::ranges::none_of(lines, [](auto const& l) { return l.contains("\x1b["); }));
}

TEST_CASE("capture-pane -e preserves each cell's SGR rendition", "[vthost][control]")
{
    auto h = ControlHarness {};
    h.host.createTab();
    auto* tab = h.host.model().window(h.host.windowId())->activeTab();
    auto const paneId = tab->rootPane()->id().value;
    auto const session = tab->rootPane()->session();
    h.host.terminal(session)->writeToScreen("\033[1;31mRED\033[0m plain\r\n");

    auto const lines = h.exchange({ std::format("capture-pane -pe -t %{}", paneId) });

    // The bold-red run is re-emitted as SGR (bundled -pe, so the -e is honoured), reset before the
    // default " plain" run.
    CHECK(contains(lines, "\x1b[0;1;31mRED"));
    CHECK(contains(lines, "\x1b[0m plain"));
}

TEST_CASE("capture-pane -S - includes scrollback beyond the visible page", "[vthost][control]")
{
    auto settings = vtbackend::Settings {};
    settings.maxHistoryLineCount = vtbackend::LineCount(1000);
    auto h = ControlHarness { {}, settings };
    h.host.createTab();
    auto* tab = h.host.model().window(h.host.windowId())->activeTab();
    auto const paneId = tab->rootPane()->id().value;
    auto const session = tab->rootPane()->session();

    // Push a distinctive first line far above the 25-row visible page, into scrollback.
    auto* terminal = h.host.terminal(session);
    terminal->writeToScreen("MARKER-FIRST\r\n");
    for (auto const i: std::views::iota(0, 100))
        terminal->writeToScreen(std::format("filler-{}\r\n", i));

    auto const visible = h.exchange({ std::format("capture-pane -p -t %{}", paneId) });
    // A second harness for the history capture (exchange() closes the session).
    auto h2 = ControlHarness { {}, settings };
    h2.host.createTab();
    auto* tab2 = h2.host.model().window(h2.host.windowId())->activeTab();
    auto const paneId2 = tab2->rootPane()->id().value;
    auto* terminal2 = h2.host.terminal(tab2->rootPane()->session());
    terminal2->writeToScreen("MARKER-FIRST\r\n");
    for (auto const i: std::views::iota(0, 100))
        terminal2->writeToScreen(std::format("filler-{}\r\n", i));
    auto const history = h2.exchange({ std::format("capture-pane -p -S - -t %{}", paneId2) });

    CHECK_FALSE(contains(visible, "MARKER-FIRST")); // scrolled out of the visible page
    CHECK(contains(history, "MARKER-FIRST"));       // but retained in, and captured from, history
}

TEST_CASE("%output escapes control bytes and never breaks a guard block", "[vthost][control]")
{
    auto h = ControlHarness {};
    h.host.createTab();
    auto* tab = h.host.model().window(h.host.windowId())->activeTab();
    auto const paneId = tab->rootPane()->id().value;

    // Wire the byte tap exactly like the daemon glue (the pump thread is
    // disabled in the harness, so output is fed directly below).
    h.host.subscribeStream(h.session.get());
    auto const lines = h.exchange({ "list-sessions" });
    h.host.unsubscribeStream(h.session.get());

    // Nothing crashed and the guard block for list-sessions is intact.
    CHECK(contains(lines, "%begin"));
    CHECK(contains(lines, "%end"));
    static_cast<void>(paneId);
}

TEST_CASE("refresh-client -C resizes the client area and answers with layouts", "[vthost][control]")
{
    auto h = ControlHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();

    auto const lines = h.exchange({ "refresh-client -C 100x50" });

    // The proposal was accepted: the host's client area changed, the single
    // full-area pane's terminal followed, and the authoritative layout answer
    // carries the new dimensions.
    CHECK(h.host.pageSize() == vtpty::PageSize { vtpty::LineCount(50), vtpty::ColumnCount(100) });
    REQUIRE(h.host.terminal(session) != nullptr);
    CHECK(h.host.terminal(session)->pageSize()
          == vtpty::PageSize { vtpty::LineCount(50), vtpty::ColumnCount(100) });
    CHECK(contains(lines, "100x50"));
    CHECK(!contains(lines, "%error"));
}

TEST_CASE("refresh-client -C rejects out-of-range and malformed sizes", "[vthost][control]")
{
    auto h = ControlHarness {};
    auto const before = h.host.pageSize();

    SECTION("too small")
    {
        auto const lines = h.exchange({ "refresh-client -C 0x50" });
        CHECK(contains(lines, "size too small or too big"));
    }
    SECTION("too big")
    {
        auto const lines = h.exchange({ "refresh-client -C 10001x50" });
        CHECK(contains(lines, "size too small or too big"));
    }
    SECTION("malformed")
    {
        auto const lines = h.exchange({ "refresh-client -C bogus" });
        CHECK(contains(lines, "bad size argument"));
    }
    SECTION("per-window form")
    {
        auto const lines = h.exchange({ "refresh-client -C @1:80x24" });
        CHECK(contains(lines, "%error"));
    }
    CHECK(h.host.pageSize() == before); // a rejected proposal changes nothing
}

TEST_CASE("refresh-client -A pauses and resumes a pane over the wire", "[vthost][control]")
{
    auto h = ControlHarness {};
    h.host.createTab();
    auto const paneId = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->id().value;

    auto const lines = h.exchange({ std::format("refresh-client -A %{}:pause", paneId),
                                    std::format("refresh-client -A %{}:continue", paneId) });

    CHECK(contains(lines, std::format("%pause %{}", paneId)));
    CHECK(contains(lines, std::format("%continue %{}", paneId)));
    CHECK(!contains(lines, "%error"));
}

TEST_CASE("refresh-client flags and subscriptions are accepted", "[vthost][control]")
{
    auto h = ControlHarness {};
    auto const lines = h.exchange({ "refresh-client -f pause-after=5,no-output",
                                    "refresh-client -f !pause-after,!no-output",
                                    "refresh-client -B mysub:%0:#{pane_title}" });
    CHECK(!contains(lines, "%error"));
}

TEST_CASE("pauseAfterFromSeconds clamps overflowing values to a positive duration", "[vthost][control]")
{
    using std::chrono::milliseconds;
    using vthost::tmux::pauseAfterFromSeconds;

    CHECK(pauseAfterFromSeconds(0) == milliseconds { 0 });
    CHECK(pauseAfterFromSeconds(5) == milliseconds { 5000 });
    // A value whose *1000 overflows milliseconds' signed rep must clamp to a huge
    // POSITIVE threshold — a negative one reads as "always past due" and would
    // pause every pane's output on the first byte.
    CHECK(pauseAfterFromSeconds(10'000'000'000'000'000ULL) > milliseconds { 0 });
    CHECK(pauseAfterFromSeconds(std::numeric_limits<std::uint64_t>::max()) > milliseconds { 0 });
}

TEST_CASE("an output burst drains fully after the PTY goes silent", "[vthost][control]")
{
    auto h = ControlHarness {};
    h.host.createTab();
    auto* tab = h.host.model().window(h.host.windowId())->activeTab();
    auto const paneId = tab->rootPane()->id().value;
    auto const sessionId = tab->rootPane()->session();
    auto const tabId = tab->id();

    // One burst far larger than a single pump's byte budget (~bufferHigh/1/3),
    // then no further pane output. Without a self-sustaining drain, only the
    // first pass would reach the client and the gated %window-renamed would hang.
    auto const burst = std::string(20000, 'x');
    auto out = std::vector<std::string> {};
    h.loop.blockOn(driveBurst(h.session.get(), h.pair.second.get(), sessionId, tabId, &burst, &out));

    // Every byte of the burst arrived, reassembled from the %output lines in order.
    auto const prefix = std::format("%output %{} ", paneId);
    auto reassembled = std::string {};
    auto lastOutputIndex = std::optional<std::size_t> {};
    auto renamedIndex = std::optional<std::size_t> {};
    auto index = std::size_t { 0 };
    for (auto const& line: out)
    {
        if (line.starts_with(prefix))
        {
            reassembled += line.substr(prefix.size());
            lastOutputIndex = index;
        }
        else if (line.starts_with("%window-renamed"))
            renamedIndex = index;
        ++index;
    }

    CHECK(reassembled == burst);
    REQUIRE(lastOutputIndex.has_value());
    REQUIRE(renamedIndex.has_value());
    // The notification flushed AFTER the whole burst — the ordering guarantee.
    CHECK(*renamedIndex > *lastOutputIndex);
    CHECK_FALSE(h.session->peerLost());
}

TEST_CASE("a control client that overflows the write queue is disconnected", "[vthost][control]")
{
    // A tiny write-queue bound: the preamble fits, but the first %output frame of
    // a burst overflows it — the session must disconnect, not drop-and-continue.
    auto h = ControlHarness { ControlSession::Options { .writeQueueMaxBytes = 256 } };
    h.host.createTab();
    auto* tab = h.host.model().window(h.host.windowId())->activeTab();
    auto const sessionId = tab->rootPane()->session();
    auto const tabId = tab->id();

    auto const burst = std::string(20000, 'x');
    auto out = std::vector<std::string> {};
    h.loop.blockOn(driveBurst(h.session.get(), h.pair.second.get(), sessionId, tabId, &burst, &out));

    // The overflow tore the session down (no hang) and dropped the burst rather
    // than emitting a truncated, corrupt stream.
    CHECK(h.session->peerLost());
    CHECK_FALSE(contains(out, "%output"));
}

TEST_CASE("capture-pane reads the DISPLAYED page, not the primary one", "[vthost][control]")
{
    // capture-pane answers "what is in this pane", and a pane running vim/less/htop is showing the
    // ALTERNATE screen. Reading the primary grid there returns the shell prompt the editor replaced
    // — and `-S -` compounds it by counting primary scrollback the pane does not show at all. The
    // native path already mirrors the displayed page for exactly this reason.
    auto h = ControlHarness {};
    h.host.createTab();
    auto* tab = h.host.model().window(h.host.windowId())->activeTab();
    auto const paneId = tab->rootPane()->id().value;
    auto* terminal = h.host.terminal(tab->rootPane()->session());
    REQUIRE(terminal != nullptr);

    terminal->writeToScreen("primary-content\r\n");
    terminal->writeToScreen("\033[?1049h");           // enter the alternate screen, as an editor does
    terminal->writeToScreen("alternate-content\r\n"); // what the user is actually looking at

    auto const lines = h.exchange({ std::format("capture-pane -p -t %{}", paneId) });

    CHECK(contains(lines, "alternate-content"));
    CHECK(std::ranges::none_of(lines, [](auto const& l) { return l.contains("primary-content"); }));
}

TEST_CASE("capture-pane -e keeps the fill rendition of an erased region", "[vthost][control]")
{
    // Screen erases with the CURSOR's pen, so `\e[41m\e[2J` leaves every row blank on red. Those
    // rows are stored un-materialized, and rendering them as bare spaces dropped the colour of the
    // whole cleared region from the capture while rows holding text kept theirs — so replaying the
    // capture painted a default-background screen instead of the red one the pane shows.
    auto h = ControlHarness {};
    h.host.createTab();
    auto* tab = h.host.model().window(h.host.windowId())->activeTab();
    auto const paneId = tab->rootPane()->id().value;
    auto* terminal = h.host.terminal(tab->rootPane()->session());
    REQUIRE(terminal != nullptr);

    terminal->writeToScreen("\033[41m\033[2J");

    auto const lines = h.exchange({ std::format("capture-pane -pe -t %{}", paneId) });
    CHECK(std::ranges::any_of(lines, [](auto const& l) { return l.contains("\x1b[0;41m"); }));
}
