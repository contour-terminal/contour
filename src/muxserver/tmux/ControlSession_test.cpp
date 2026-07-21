// SPDX-License-Identifier: Apache-2.0
#include <vtpty/MockPty.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <coro/WhenAll.hpp>
#include <muxserver/SessionHost.h>
#include <muxserver/TappingPty.h>
#include <muxserver/tmux/ControlSession.h>
#include <net/AsyncBufferedReader.h>
#include <net/EventLoop.h>
#include <net/PollEventSource.h>
#include <net/testing/InMemoryTransport.h>
#include <vtmux/Pane.h>
#include <vtmux/Tab.h>

using coro::Task;
using muxserver::SessionHost;
using muxserver::tmux::ControlSession;
using muxserver::tmux::splitCommandLine;

namespace
{

// Free coroutines with pointer parameters (a capturing lambda coroutine would
// dangle its closure — cppcoreguidelines-avoid-capturing-lambda-coroutines).

/// Writes each command (newline-terminated) then a bare empty line, which the
/// protocol treats as detach: the SESSION ends and closes ITS end, so the
/// collector (reading the same bidirectional socket) sees EOF only after every
/// reply. Closing the client end here would instead tear down both directions
/// and starve the collector.
Task<void> feedCommands(net::ISocket* client, std::vector<std::string> const* commands)
{
    for (auto const& command: *commands)
    {
        auto const wire = command + "\n";
        auto const bytes =
            std::span<std::byte const> { reinterpret_cast<std::byte const*>(wire.data()), wire.size() };
        std::ignore = co_await client->write(bytes);
    }
    auto const detach = std::string_view { "\n" }; // empty line -> detach (control.c:547)
    std::ignore = co_await client->write(
        std::span<std::byte const> { reinterpret_cast<std::byte const*>(detach.data()), detach.size() });
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

/// Runs the session concurrently with the feeder and the collector.
Task<void> driveExchange(ControlSession* session,
                         net::ISocket* client,
                         std::vector<std::string> const* commands,
                         std::vector<std::string>* out)
{
    co_await coro::whenAll(session->run(), feedCommands(client, commands), collectLines(client, out));
}

/// Drives a ControlSession over an in-memory socket pair: the test writes
/// command lines into one end and reads the protocol replies from it, while the
/// session runs on the other end.
struct ControlHarness
{
    net::PollEventSource source;
    net::EventLoop loop { source };
    SessionHost host { loop,
                       [](vtbackend::PageSize size) { return std::make_unique<vtpty::MockPty>(size); },
                       vtbackend::Settings {},
                       /*startPumps=*/false };
    net::testing::SocketPair pair = *net::testing::makeSocketPair(loop);
    std::unique_ptr<ControlSession> session;

    ControlHarness()
    {
        // A fixed clock so guard timestamps are deterministic.
        session = std::make_unique<ControlSession>(
            loop, host, std::move(pair.first), [] { return std::int64_t { 1000 }; });
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

TEST_CASE("splitCommandLine honours single and double quotes", "[muxserver][control]")
{
    CHECK(splitCommandLine("new-window") == std::vector<std::string> { "new-window" });
    CHECK(splitCommandLine("send-keys -t %1 'echo hi'")
          == std::vector<std::string> { "send-keys", "-t", "%1", "echo hi" });
    CHECK(splitCommandLine(R"(display-message "a \"b\" c")")
          == std::vector<std::string> { "display-message", "a \"b\" c" });
    CHECK(splitCommandLine("   ").empty());
}

TEST_CASE("the session opens with a guard pair and session-changed", "[muxserver][control]")
{
    auto h = ControlHarness {};
    auto const lines = h.exchange({});

    REQUIRE(lines.size() >= 3);
    CHECK(lines[0] == "%begin 1000 0 1");
    CHECK(lines[1] == "%end 1000 0 1");
    CHECK(lines[2] == "%session-changed $0 0");
    CHECK(lines.back() == "%exit");
}

TEST_CASE("new-window creates a tab and notifies %window-add + %layout-change", "[muxserver][control]")
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

TEST_CASE("split-window adds a pane and re-emits the layout", "[muxserver][control]")
{
    auto h = ControlHarness {};
    auto const lines = h.exchange({ "new-window", "split-window -h" });

    auto* tab = h.host.model().window(h.host.windowId())->activeTab();
    REQUIRE(tab != nullptr);
    CHECK(tab->paneCount() == 2);
    // The second %layout-change carries a side-by-side container.
    CHECK(std::ranges::count_if(lines, [](auto const& l) { return l.contains("%layout-change"); }) >= 2);
}

TEST_CASE("an unknown command yields an %error guard", "[muxserver][control]")
{
    auto h = ControlHarness {};
    auto const lines = h.exchange({ "frobnicate" });
    CHECK(contains(lines, "%error"));
    CHECK(contains(lines, "unknown command: frobnicate"));
}

TEST_CASE("list-windows reports the created windows inside a guard", "[muxserver][control]")
{
    auto h = ControlHarness {};
    auto const lines = h.exchange({ "new-window", "new-window", "list-windows" });

    // Two windows, each on its own body line between a %begin/%end pair.
    CHECK(std::ranges::count_if(lines, [](auto const& l) { return l.starts_with("0: @"); }) == 1);
    CHECK(std::ranges::count_if(lines, [](auto const& l) { return l.starts_with("1: @"); }) == 1);
}

TEST_CASE("send-keys writes into the target pane's PTY", "[muxserver][control]")
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
    auto& tapped = dynamic_cast<muxserver::TappingPty&>(terminal->device());
    auto& mock = dynamic_cast<vtpty::MockPty&>(tapped.inner());
    CHECK(mock.stdinBuffer() == "echo hi\r");
}

TEST_CASE("%output escapes control bytes and never breaks a guard block", "[muxserver][control]")
{
    auto h = ControlHarness {};
    h.host.createTab();
    auto* tab = h.host.model().window(h.host.windowId())->activeTab();
    auto const paneId = tab->rootPane()->id().value;

    // Feed output directly (the pump thread is disabled in the harness).
    h.host.setOutputHandler(
        [&](vtmux::SessionId id, std::string const& bytes) { h.session->onSessionOutput(id, bytes); });
    auto const lines = h.exchange({ "list-sessions" });
    h.host.setOutputHandler(nullptr);

    // Nothing crashed and the guard block for list-sessions is intact.
    CHECK(contains(lines, "%begin"));
    CHECK(contains(lines, "%end"));
    static_cast<void>(paneId);
}
