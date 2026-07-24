// SPDX-License-Identifier: Apache-2.0
#include <vtpty/MockPty.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <tuple>
#include <vector>

#include <coro/WhenAll.hpp>
#include <net/EventLoop.h>
#include <net/PollEventSource.h>
#include <net/testing/InMemoryTransport.h>
#include <vthost/SessionHost.h>
#include <vthost/TappingPty.h>
#include <vthost/tmux/ControlSession.h>
#include <vthost/tmux/LayoutString.h>
#include <vthost/tmux/TmuxGateway.h>
#include <vtworkspace/Pane.h>
#include <vtworkspace/Tab.h>

using coro::Task;
using vthost::SessionHost;
using vthost::tmux::ControlSession;
using vthost::tmux::GatewayEvents;
using vthost::tmux::TmuxGateway;
using namespace std::chrono_literals;

namespace
{

/// Records every notification the gateway dispatches.
struct RecordingEvents final: GatewayEvents
{
    std::vector<uint64_t> windowsAdded;
    std::vector<std::string> layouts;
    std::string output;
    bool sawExit = false;

    void windowAdded(uint64_t window) override { windowsAdded.push_back(window); }
    void layoutChanged(uint64_t, std::string_view layout) override { layouts.emplace_back(layout); }
    void outputReceived(uint64_t, std::string_view bytes) override { output += bytes; }
    void exited(std::string_view) override { sawExit = true; }
};

/// Our gateway attached to OUR control-mode server: the loopback validates
/// both protocol halves against each other (the server itself is oracle-
/// verified against tmux 3.7b).
struct LoopbackHarness
{
    net::PollEventSource source;
    net::EventLoop loop { source };
    SessionHost host { loop,
                       [](vtbackend::PageSize size) { return std::make_unique<vtpty::MockPty>(size); },
                       vtbackend::Settings {},
                       /*startPumps=*/false };
    net::testing::SocketPair pair = *net::testing::makeSocketPair(loop);
    std::unique_ptr<ControlSession> server = std::make_unique<ControlSession>(
        loop, host, std::move(pair.first), [] { return std::int64_t { 1000 }; });
    RecordingEvents events;
    std::unique_ptr<TmuxGateway> gateway =
        std::make_unique<TmuxGateway>(loop, std::move(pair.second), events);
};

Task<void> waitUntil(net::EventLoop* loop, std::function<bool()> ready, int iterations = 1000)
{
    for (auto i = 0; i < iterations && !ready(); ++i)
        co_await loop->delay(1ms);
}

Task<void> scenario(LoopbackHarness* h)
{
    // 1. The opening guard completes: notifications are un-gated.
    co_await waitUntil(&h->loop, [&] { return h->gateway->initialised(); });
    REQUIRE(h->gateway->initialised());

    // 2. new-window: the response correlates FIFO; %window-add and
    //    %layout-change arrive as notifications.
    auto newWindowOk = false;
    h->gateway->sendCommand("new-window",
                            [&](bool ok, std::vector<std::string> const&) { newWindowOk = ok; });
    co_await waitUntil(&h->loop, [&] { return !h->events.windowsAdded.empty() && newWindowOk; });
    REQUIRE(newWindowOk);
    REQUIRE(h->events.windowsAdded.size() == 1);
    REQUIRE_FALSE(h->events.layouts.empty());
    // The advertised layout parses with our own codec.
    CHECK(vthost::tmux::parseLayout(h->events.layouts.front()).has_value());

    // 3. An unknown command correlates as an error.
    auto errorSeen = false;
    h->gateway->sendCommand("frobnicate", [&](bool ok, std::vector<std::string> const&) { errorSeen = !ok; });

    // 4. list-windows returns its body lines to the right callback.
    auto listing = std::vector<std::string> {};
    h->gateway->sendCommand("list-windows",
                            [&](bool, std::vector<std::string> const& body) { listing = body; });
    co_await waitUntil(&h->loop, [&] { return errorSeen && !listing.empty(); });
    REQUIRE(errorSeen);
    REQUIRE(listing.size() == 1);
    CHECK(listing.front().starts_with("0: @"));

    // 5. sendKeys lands in the pane's PTY, quoted through send-keys -l.
    auto const paneId = h->host.model().window(h->host.windowId())->activeTab()->rootPane()->id().value;
    auto const session = h->host.model().window(h->host.windowId())->activeTab()->rootPane()->session();
    h->gateway->sendKeys(paneId, "echo 'hi'");
    auto& tapped = dynamic_cast<vthost::TappingPty&>(h->host.terminal(session)->device());
    auto& mock = dynamic_cast<vtpty::MockPty&>(tapped.inner());
    co_await waitUntil(&h->loop, [&] { return !mock.stdinBuffer().empty(); });
    CHECK(mock.stdinBuffer() == "echo 'hi'");

    // 5b. sendRawInput carries control bytes verbatim through send-keys -H —
    //     ESC and CR would be mangled by any quoting path.
    mock.stdinBuffer().clear();
    h->gateway->sendRawInput(paneId, "\x1b[A\rok");
    co_await waitUntil(&h->loop, [&] { return mock.stdinBuffer().size() >= 6; });
    CHECK(mock.stdinBuffer() == "\x1b[A\rok");

    // 6. Detach: the server answers %exit and both loops wind down.
    h->gateway->detach();
    co_await waitUntil(&h->loop, [&] { return h->events.sawExit; });
    CHECK(h->events.sawExit);
}

Task<void> driveLoopback(LoopbackHarness* h)
{
    co_await coro::whenAll(h->server->run(), h->gateway->run(), scenario(h));
}

} // namespace

TEST_CASE("the gateway drives our control-mode server end to end", "[vthost][gateway]")
{
    auto h = LoopbackHarness {};
    h.loop.blockOn(driveLoopback(&h));

    CHECK(h.events.sawExit);
    CHECK(h.host.model().window(h.host.windowId())->tabCount() == 1);
}

namespace
{

Task<void> writeAll(net::ISocket* socket, std::string_view text)
{
    auto const bytes =
        std::span<std::byte const> { reinterpret_cast<std::byte const*>(text.data()), text.size() };
    std::ignore = co_await socket->write(bytes);
}

/// Hand-writes control-mode bytes so a guard body can carry a line that reads
/// exactly like "%end" — the raw capture-pane hazard the number match guards.
Task<void> embeddedEndScenario(net::EventLoop* loop,
                               net::ISocket* server,
                               TmuxGateway* gateway,
                               std::vector<std::string>* body,
                               bool* done)
{
    // The opening guard (command number 0) un-gates the gateway.
    co_await writeAll(server, "%begin 1000 0 0\n%end 1000 0 0\n");
    co_await waitUntil(loop, [&] { return gateway->initialised(); });

    gateway->sendCommand("capture-pane", [body, done](bool, std::vector<std::string> const& lines) {
        *body = lines;
        *done = true;
    });

    // The reply's guard is command number 5. Its body includes a line that reads
    // exactly like a %end for a DIFFERENT command (999): it must stay body, and
    // only the matching "%end ... 5 ..." may close the block.
    co_await writeAll(server, "%begin 1000 5 0\n");
    co_await writeAll(server, "one\n");
    co_await writeAll(server, "%end 1000 999 0\n");
    co_await writeAll(server, "three\n");
    co_await writeAll(server, "%end 1000 5 0\n");

    co_await waitUntil(loop, [&] { return *done; });
    server->close(); // EOF ends gateway->run()
}

Task<void> driveEmbeddedEnd(net::EventLoop* loop,
                            net::ISocket* server,
                            TmuxGateway* gateway,
                            std::vector<std::string>* body,
                            bool* done)
{
    co_await coro::whenAll(gateway->run(), embeddedEndScenario(loop, server, gateway, body, done));
}

} // namespace

TEST_CASE("a guard body line that looks like %end does not close the block early", "[vthost][gateway]")
{
    auto source = net::PollEventSource {};
    auto loop = net::EventLoop { source };
    auto pair = *net::testing::makeSocketPair(loop);

    auto events = RecordingEvents {};
    auto gateway = TmuxGateway { loop, std::move(pair.second), events };

    auto body = std::vector<std::string> {};
    auto done = false;
    auto* server = pair.first.get();

    loop.blockOn(driveEmbeddedEnd(&loop, server, &gateway, &body, &done));

    REQUIRE(done);
    CHECK(body == std::vector<std::string> { "one", "%end 1000 999 0", "three" });
}

namespace
{

/// Counts notification drains alongside window-adds, so the burst boundary's
/// exact timing relative to a partial trailing line is observable.
struct DrainCountingEvents final: GatewayEvents
{
    std::vector<uint64_t> windowsAdded;
    int drains = 0;

    void windowAdded(uint64_t window) override { windowsAdded.push_back(window); }
    void notificationsDrained() override { ++drains; }
};

Task<void> partialBurstScenario(net::EventLoop* loop,
                                net::ISocket* server,
                                TmuxGateway* gateway,
                                DrainCountingEvents* events)
{
    // Un-gate the gateway (opening guard, command number 0).
    co_await writeAll(server, "%begin 1000 0 0\n%end 1000 0 0\n");
    co_await waitUntil(loop, [&] { return gateway->initialised(); });

    // One socket write carrying a COMPLETE notification followed by a PARTIAL
    // (unterminated) one: the single-threaded loop resumes the gateway only after
    // the whole write lands, so the reader delivers @1 with "%window-add @2" still
    // buffered WITHOUT a newline. Gating on hasBufferedLine() alone would drain here
    // -- mid-burst -- because a partial line contains no '\n'.
    auto const drainsBefore = events->drains;
    co_await writeAll(server, "%window-add @1\n%window-add @2");
    co_await waitUntil(loop, [&] { return !events->windowsAdded.empty(); });
    REQUIRE(events->windowsAdded == std::vector<uint64_t> { 1 });
    CHECK(events->drains == drainsBefore); // no drain while @2 is a partial trailing line

    // Completing @2 empties the buffer, so exactly one drain fires now.
    co_await writeAll(server, "\n");
    co_await waitUntil(loop, [&] { return events->windowsAdded.size() == 2; });
    REQUIRE(events->windowsAdded == (std::vector<uint64_t> { 1, 2 }));
    CHECK(events->drains == drainsBefore + 1);

    server->close(); // EOF ends gateway->run()
}

Task<void> drivePartialBurst(net::EventLoop* loop,
                             net::ISocket* server,
                             TmuxGateway* gateway,
                             DrainCountingEvents* events)
{
    co_await coro::whenAll(gateway->run(), partialBurstScenario(loop, server, gateway, events));
}

} // namespace

TEST_CASE("a partial trailing line does not trigger a premature notification drain", "[vthost][gateway]")
{
    auto source = net::PollEventSource {};
    auto loop = net::EventLoop { source };
    auto pair = *net::testing::makeSocketPair(loop);

    auto events = DrainCountingEvents {};
    auto gateway = TmuxGateway { loop, std::move(pair.second), events };
    auto* server = pair.first.get();

    loop.blockOn(drivePartialBurst(&loop, server, &gateway, &events));

    CHECK(events.windowsAdded == (std::vector<uint64_t> { 1, 2 }));
}

// {{{ live oracle: the gateway drives a REAL tmux -C client
#ifndef _WIN32

    #include <sys/wait.h>

    #include <csignal>
    #include <cstdio>
    #include <cstdlib>
    #include <filesystem>
    #include <format>

    #ifdef __APPLE__
        #include <util.h>
    #elif defined(__FreeBSD__) || defined(__DragonFly__) || defined(__OpenBSD__)
        #include <libutil.h>
    #else
        #include <pty.h>
    #endif
    #include <unistd.h>

    #include <net/Sockets.h>

namespace
{

/// Runs a shell command via popen (::system is concurrency-mt-unsafe).
/// @return The command's exit status, or -1 on spawn failure.
int runShell(std::string const& command)
{
    auto* pipe = ::popen(command.c_str(), "r");
    if (pipe == nullptr)
        return -1;
    auto buffer = std::array<char, 256> {};
    while (::fgets(buffer.data(), buffer.size(), pipe) != nullptr)
        ;
    return ::pclose(pipe);
}

/// A real `tmux -C new-session` on its own PTY and private server socket.
/// tmux's client insists on a terminal, so the child gets a fresh pty slave;
/// the master fd becomes the gateway's transport via net::adoptFd.
struct RealTmux
{
    pid_t pid = -1;
    int master = -1;
    std::string socketPath;

    static std::unique_ptr<RealTmux> launch()
    {
        if (runShell("command -v tmux >/dev/null 2>&1") != 0)
            return nullptr;

        auto self = std::make_unique<RealTmux>();
        self->socketPath =
            (std::filesystem::temp_directory_path() / std::format("cmux-gw-{}.sock", ::getpid())).string();

        auto master = -1;
        auto const pid = ::forkpty(&master, nullptr, nullptr, nullptr);
        if (pid < 0)
            return nullptr;
        if (pid == 0)
        {
            ::execlp("tmux",
                     "tmux",
                     "-C",
                     "-f",
                     "/dev/null",
                     "-S",
                     self->socketPath.c_str(),
                     "new-session",
                     nullptr);
            ::_exit(127);
        }
        self->pid = pid;
        self->master = master;
        return self;
    }

    /// Yields the master fd to the caller (the gateway's transport owns it).
    [[nodiscard]] int takeMaster() noexcept
    {
        auto const fd = master;
        master = -1;
        return fd;
    }

    ~RealTmux()
    {
        if (!socketPath.empty())
            std::ignore = runShell(std::format("tmux -S {} kill-server >/dev/null 2>&1", socketPath));
        if (master >= 0)
            ::close(master);
        if (pid > 0)
        {
            auto status = 0;
            for (auto i = 0; i < 50 && ::waitpid(pid, &status, WNOHANG) == 0; ++i)
                ::usleep(100'000);
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &status, 0);
        }
        std::error_code ec;
        std::filesystem::remove(socketPath, ec);
    }
};

Task<void> oracleScenario(net::EventLoop* loop, TmuxGateway* gateway, RecordingEvents* events)
{
    co_await waitUntil(loop, [&] { return gateway->initialised(); }, 15000);
    REQUIRE(gateway->initialised());

    // The initial window's layout notification (or list-windows) must parse
    // with our own layout codec.
    auto listing = std::vector<std::string> {};
    gateway->sendCommand("list-windows -F '#{window_layout}'",
                         [&](bool, std::vector<std::string> const& body) { listing = body; });
    co_await waitUntil(loop, [&] { return !listing.empty(); }, 15000);
    REQUIRE_FALSE(listing.empty());
    CHECK(vthost::tmux::parseLayout(listing.front()).has_value());

    // Type through send-keys, then poll capture-pane until the shell echoed.
    gateway->sendKeys(0, "echo oracle-ok");
    gateway->sendCommand("send-keys -t %0 Enter");
    auto captured = std::string {};
    for (auto attempt = 0; attempt < 50 && !captured.contains("oracle-ok"); ++attempt)
    {
        auto done = false;
        gateway->sendCommand("capture-pane -p -t %0", [&](bool, std::vector<std::string> const& body) {
            for (auto const& line: body)
                captured += line + "\n";
            done = true;
        });
        co_await waitUntil(loop, [&] { return done; }, 15000);
        if (!captured.contains("oracle-ok"))
        {
            captured.clear();
            co_await loop->delay(100ms);
        }
    }
    CHECK(captured.contains("oracle-ok"));

    // The hex channel (send-keys -H) must deliver raw bytes — including the
    // CR — through the real tmux server as well.
    gateway->sendRawInput(0, "echo oracle-hex\r");
    captured.clear();
    for (auto attempt = 0; attempt < 50 && !captured.contains("oracle-hex"); ++attempt)
    {
        auto done = false;
        gateway->sendCommand("capture-pane -p -t %0", [&](bool, std::vector<std::string> const& body) {
            for (auto const& line: body)
                captured += line + "\n";
            done = true;
        });
        co_await waitUntil(loop, [&] { return done; }, 15000);
        if (!captured.contains("oracle-hex"))
        {
            captured.clear();
            co_await loop->delay(100ms);
        }
    }
    CHECK(captured.contains("oracle-hex"));

    gateway->detach();
    co_await waitUntil(loop, [&] { return events->sawExit; }, 15000);
    CHECK(events->sawExit);
}

Task<void> driveOracle(net::EventLoop* loop, TmuxGateway* gateway, RecordingEvents* events)
{
    co_await coro::whenAll(gateway->run(), oracleScenario(loop, gateway, events));
}

} // namespace

TEST_CASE("the gateway drives a real tmux -C client", "[vthost][gateway][oracle]")
{
    auto tmux = RealTmux::launch();
    if (!tmux)
    {
        SKIP("tmux not available");
    }

    auto source = net::PollEventSource {};
    auto loop = net::EventLoop { source };
    auto transport = net::adoptFd(loop, tmux->takeMaster());
    REQUIRE(transport.has_value());

    auto events = RecordingEvents {};
    auto gateway = TmuxGateway { loop, std::move(*transport), events };
    loop.blockOn(driveOracle(&loop, &gateway, &events));

    CHECK(events.sawExit);
}

#endif // !_WIN32
// }}}
