// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/testing/MockTerm.hpp>

#include <vtpty/ProcessPlacement.hpp>

#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/Sockets.hpp>
#include <core/net/Tls.hpp>

#include <catch2/catch_test_macros.hpp>

#include <sys/socket.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <ranges>
#include <string>
#include <thread>
#include <vector>

#include <vthost/SocketPath.hpp>
#include <vthost/WorkspaceCapture.hpp>
#include <vthost/keeperlink/KeeperChannel.hpp>
#include <vthost/keeperlink/KeeperLink.hpp>
#include <vthost/keeperlink/KeeperPtyFactory.hpp>
#include <vthost/keeperlink/Recovery.hpp>
#include <vthost/proto/Pdu.hpp>
#include <vthost/testing/KeeperFixture.hpp>

using namespace std::chrono_literals;
using vthost::testing::HarnessSetup;
using vthost::testing::HarnessStart;
using vthost::testing::KeeperFixture;

namespace keeper = vthost::keeper;
namespace keeperlink = vthost::keeperlink;
namespace fs = std::filesystem;
namespace proto = vthost::proto;

namespace
{

/// How long a daemon the keeper restarts may take to serve: generous, for debug builds on a loaded machine.
constexpr auto RecoveryBound = std::chrono::milliseconds { 60s };

/// Places nothing: holds each child, parked, and crashes the daemon it plays as it is asked to place one.
class CrashingPlacement final: public vtpty::ProcessPlacement
{
  public:
    explicit CrashingPlacement(std::function<void()> crash): _crash { std::move(crash) } {}

    void placeThenRelease(vtpty::ParkedChild child) noexcept override
    {
        _parked.push_back(std::move(child));
        _crash();
    }
    [[nodiscard]] std::optional<int> childOomScoreAdjust() const noexcept override { return std::nullopt; }

    /// @return How many children it holds.
    [[nodiscard]] std::size_t parked() const noexcept { return _parked.size(); }

  private:
    std::function<void()> _crash;
    std::vector<vtpty::ParkedChild> _parked;
};

/// What an attaching client is told first: which run of the daemon serves, and the sessions it shows.
struct Attached
{
    uint64_t daemonInstance = 0;
    std::vector<uint64_t> sessions; ///< Every pane's session, sorted.
    std::string text;               ///< Every row of the screens it was sent, scrollback included.
};

/// How far an attach reads.
enum class AttachDepth : uint8_t
{
    Layout = 0, ///< Up to the first layout.
    Screens,    ///< On, until a screen snapshot came for every session the layout shows.
};

void collectSessions(proto::WirePane const& pane, std::vector<uint64_t>& sessions)
{
    if (!pane.isSplit())
        sessions.push_back(pane.session);
    for (auto const& child: pane.children)
        collectSessions(child, sessions);
}

/// Attaches to @p socketPath and reads up to the first layout.
core::async::Task<void> attachUntil(core::net::EventLoop* loop,
                                    std::string socketPath,
                                    AttachDepth depth,
                                    Attached* attached)
{
    auto connected = co_await core::net::connectUnix(loop, socketPath);
    REQUIRE(connected.has_value());
    auto const socket = std::move(*connected);
    socket->setReceiveDeadline(std::chrono::seconds { 10 });
    auto request = proto::Writer {};
    proto::encodePdu(request, 1, proto::ClientHello { .role = proto::ConnectionRole::Attach });
    auto const bytes = std::vector<std::byte> { request.view().begin(), request.view().end() };
    REQUIRE((co_await socket->write(bytes)).has_value());
    auto pending = std::vector<std::byte> {};
    auto buffer = std::vector<std::byte>(1 << 16);
    auto screensAwaited = std::vector<uint64_t> {};
    while (true)
    {
        auto const read = co_await socket->read(buffer);
        if (!read.has_value() || *read == 0)
            co_return;
        pending.insert(pending.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(*read));
        while (auto frame = proto::decodePdu(pending))
        {
            pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(frame->consumed));
            if (auto const* hello = std::get_if<proto::ServerHello>(&frame->pdu))
                attached->daemonInstance = hello->daemonInstance;
            if (auto const* delta = std::get_if<proto::Delta>(&frame->pdu))
            {
                for (auto const& line: delta->lines)
                {
                    for (auto const& cell: line.cells)
                        if (cell.codepoint >= 0x20 && cell.codepoint < 0x7F)
                            attached->text.push_back(static_cast<char>(cell.codepoint));
                    attached->text.push_back('\n');
                }
                if (delta->snapshot != 0)
                    std::erase(screensAwaited, delta->session);
                if (depth == AttachDepth::Screens && !attached->sessions.empty() && screensAwaited.empty())
                    co_return;
            }
            if (auto const* layout = std::get_if<proto::LayoutState>(&frame->pdu))
            {
                for (auto const& tab: layout->tabs)
                    collectSessions(tab.root, attached->sessions);
                std::ranges::sort(attached->sessions);
                screensAwaited = attached->sessions;
                if (depth == AttachDepth::Layout)
                    co_return;
            }
        }
    }
}

/// @return What a client attaching to @p f's daemon is told.
[[nodiscard]] Attached attach(KeeperFixture const& f, AttachDepth depth = AttachDepth::Layout)
{
    auto const backend = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *backend };
    auto attached = Attached {};
    loop.blockOn(attachUntil(&loop, vthost::nativeSocketPath(f.socketPath()).string(), depth, &attached));
    return attached;
}

/// Kills @p f's daemon as a crash would, and waits for the keeper's next one to serve.
/// @return The new daemon's pid.
int crashAndRecover(KeeperFixture& f, int signum = SIGKILL)
{
    auto const before = f.daemonPid();
    REQUIRE(before > 0);
    ::kill(before, signum);
    auto const after = f.awaitNewDaemon(before, RecoveryBound);
    INFO(f.log());
    REQUIRE(after > 0);
    return after;
}

/// Waits until @p done holds, for at most @p bound.
[[nodiscard]] bool eventually(std::function<bool()> const& done, std::chrono::milliseconds bound = 20s)
{
    auto const deadline = std::chrono::steady_clock::now() + bound;
    while (!done())
    {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(20ms);
    }
    return true;
}

[[nodiscard]] std::string readFile(fs::path const& path)
{
    auto file = std::ifstream { path };
    return { std::istreambuf_iterator<char> { file }, std::istreambuf_iterator<char> {} };
}

/// @return How often @p needle occurs in @p text.
[[nodiscard]] std::size_t occurrences(std::string_view text, std::string_view needle)
{
    auto count = std::size_t { 0 };
    auto at = text.find(needle);
    while (at != std::string_view::npos)
    {
        ++count;
        at = text.find(needle, at + needle.size());
    }
    return count;
}

/// A TLS client's handshake with @p port, trusting whatever it is shown (first use).
/// @param digest Set to the certificate the daemon presented.
core::async::Task<void> certificateOf(core::net::EventLoop* loop,
                                      std::uint16_t port,
                                      std::optional<std::array<std::byte, 32>>* digest)
{
    auto const trusting = core::net::makeTlsClientContext();
    REQUIRE(trusting.has_value());
    auto connected = co_await core::net::connect(loop, "127.0.0.1", port);
    REQUIRE(connected.has_value());
    auto const socket = (*trusting)->wrap(std::move(*connected), *loop);
    socket->setReceiveDeadline(std::chrono::seconds { 10 });
    auto request = proto::Writer {};
    proto::encodePdu(
        request, 1, proto::ClientHello { .token = "harness", .role = proto::ConnectionRole::Attach });
    auto const bytes = std::vector<std::byte> { request.view().begin(), request.view().end() };
    if (!(co_await socket->write(bytes)).has_value())
        co_return;
    auto buffer = std::vector<std::byte>(4096);
    std::ignore = co_await socket->read(buffer);
    if (auto const* const session = core::net::tlsSessionOf(*socket))
        *digest = session->peerCertificateDigest();
}

/// @return The certificate @p port presents.
[[nodiscard]] std::optional<std::array<std::byte, 32>> certificateOf(std::uint16_t port)
{
    auto const backend = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *backend };
    auto digest = std::optional<std::array<std::byte, 32>> {};
    loop.blockOn(certificateOf(&loop, port, &digest));
    return digest;
}

} // namespace

TEST_CASE("a crashed daemon's sessions come back with the same ids and instance", "[keeper][recovery]")
{
    auto f = KeeperFixture { HarnessSetup { .start = HarnessStart::Serves,
                                            .panes = { "exec sleep 300", "exec sleep 300" },
                                            .snapshot = vthost::testing::HarnessSnapshot::None,
                                            .tcp = vthost::testing::HarnessTcp::Off,
                                            .softDescriptorLimit = std::nullopt } };
    INFO(f.log());
    auto const before = attach(f);
    REQUIRE(before.sessions.size() == 2);
    REQUIRE(before.daemonInstance != 0);

    std::ignore = crashAndRecover(f);
    auto const after = attach(f);
    CHECK(after.daemonInstance == before.daemonInstance);
    CHECK(after.sessions == before.sessions);
    CHECK(f.launches().size() == 2);
}

TEST_CASE("recovered scrollback comes from the snapshot, which is never set aside", "[keeper][recovery]")
{
    auto f = KeeperFixture { HarnessSetup { .start = HarnessStart::Serves,
                                            .panes = { "echo SNAPSHOT-MARKER; exec sleep 300" },
                                            .snapshot = vthost::testing::HarnessSnapshot::Kept,
                                            .tcp = vthost::testing::HarnessTcp::Off,
                                            .softDescriptorLimit = std::nullopt } };
    INFO(f.log());
    // Written once the workspace was: the marker is on the pane's screen by then.
    REQUIRE(eventually([&] { return fs::exists(f.snapshotPath()) && fs::file_size(f.snapshotPath()) > 0; }));
    std::this_thread::sleep_for(1500ms); // one more write, with the marker in it

    for (auto const restart: std::views::iota(1, 4))
    {
        std::ignore = crashAndRecover(f);
        CAPTURE(restart);
        CHECK(eventually([&] {
            return occurrences(f.log(), "seeded from the snapshot") >= static_cast<std::size_t>(restart);
        }));
    }
    // The marker the first daemon's pane showed is in the recovered pane's scrollback, behind the divider.
    auto const seen = attach(f, AttachDepth::Screens);
    CHECK(seen.text.contains("SNAPSHOT-MARKER"));
    CHECK(seen.text.contains("Restored"));
    // Read every time, and never moved: no crash loop turns it into a retry or a failure.
    auto names = std::vector<std::string> {};
    for (auto const& entry: fs::directory_iterator { f.directory() })
        names.push_back(entry.path().filename().string());
    CHECK(fs::exists(f.snapshotPath()));
    CHECK(std::ranges::none_of(names, [](std::string const& name) {
        return name.ends_with(".restored") || name.ends_with(".failed") || name.ends_with(".corrupt");
    }));
}

TEST_CASE("a recovered pane's history is seeded from the snapshot pane of its UUID", "[keeper][recovery]")
{
    auto source =
        vtbackend::MockTerm<> { vtbackend::PageSize { vtbackend::LineCount(24), vtbackend::ColumnCount(80) },
                                vtbackend::LineCount(200) };
    source.terminal.writeToScreen("first-row\r\nsecond-row\r\n");
    auto const uuid = vtcontrol::Uuid { .bytes = { 1, 2, 3 } };
    auto restore = vthost::WorkspaceRestore {};
    restore.snapshot.workspaces.push_back(vthost::WorkspaceLayoutSnapshot {});
    auto& tab = restore.snapshot.workspaces.front().tabs.emplace_back();
    tab.root.pane.identity = vthost::ObjectIdentityRecord { .uuid = uuid.bytes, .label = std::nullopt };
    tab.root.pane.history = vthost::capturePaneHistory(source.terminal, 1000);
    auto const seed = keeperlink::snapshotSeederOf(std::move(restore));

    auto const historyText = [](vtbackend::Terminal const& terminal) {
        auto text = std::string {};
        auto const& grid = terminal.primaryScreen().grid();
        for (auto const row: std::views::iota(-static_cast<int>(grid.historyLineCount().value), 1))
            text += grid.lineText(vtbackend::LineOffset(row)) + "\n";
        return text;
    };

    auto seeded =
        vtbackend::MockTerm<> { vtbackend::PageSize { vtbackend::LineCount(24), vtbackend::ColumnCount(80) },
                                vtbackend::LineCount(200) };
    auto named = vthost::handover::HandoverSession {};
    named.uuid = uuid;
    seed(named, seeded.terminal);
    CHECK(historyText(seeded.terminal).contains("first-row"));
    CHECK(historyText(seeded.terminal).contains("second-row"));

    // A pane the snapshot does not hold stays fresh.
    auto other =
        vtbackend::MockTerm<> { vtbackend::PageSize { vtbackend::LineCount(24), vtbackend::ColumnCount(80) },
                                vtbackend::LineCount(200) };
    auto unknown = vthost::handover::HandoverSession {};
    unknown.uuid = vtcontrol::Uuid { .bytes = { 9 } };
    seed(unknown, other.terminal);
    CHECK_FALSE(historyText(other.terminal).contains("first-row"));
}

TEST_CASE("a same-size recovery still delivers SIGWINCH", "[keeper][recovery]")
{
    auto f = KeeperFixture { HarnessSetup {
        .start = HarnessStart::Serves,
        .panes = { "trap 'echo w >> @DIR@/winched' WINCH; : > @DIR@/ready; while :; do sleep 0.05; done" },
        .snapshot = vthost::testing::HarnessSnapshot::None,
        .tcp = vthost::testing::HarnessTcp::Off,
        .softDescriptorLimit = std::nullopt } };
    INFO(f.log());
    REQUIRE(eventually([&] { return fs::exists(f.directory() / "ready"); }));
    // Nothing resized it while the first daemon ran.
    CHECK_FALSE(fs::exists(f.directory() / "winched"));

    std::ignore = crashAndRecover(f);
    // Rebuilt at the size it had, which sends no signal by itself: asked to redraw all the same.
    CHECK(eventually([&] { return readFile(f.directory() / "winched").contains('w'); }));
}

TEST_CASE("a parked child left by a crashed daemon is released by the next", "[keeper][recovery]")
{
    // The test plays the first daemon, and dies in the middle of placing its one child: after Spawned, before
    // Placed, the gate still the keeper's to hand on.
    auto f = KeeperFixture { HarnessSetup { .start = HarnessStart::RelaysThenServes,
                                            .panes = {},
                                            .snapshot = vthost::testing::HarnessSnapshot::None,
                                            .tcp = vthost::testing::HarnessTcp::Off,
                                            .softDescriptorLimit = std::nullopt } };
    INFO(f.log());
    auto const backend = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *backend };
    auto link = std::optional<keeperlink::KeeperLink> {};
    link.emplace(loop, keeperlink::KeeperChannel { f.takeChannel() }, keeper::ChannelVersion, [] {});
    link->post(keeper::Serving { .hungRestart = keeper::HungRestart::Off, .heartbeatTimeoutMs = 30'000 });
    REQUIRE(f.launches().size() == 1);
    auto const relay = f.launches().front();
    auto const placement = std::make_shared<CrashingPlacement>([&] {
        ::kill(relay, SIGKILL);
        ::shutdown(link->channelHandle(), SHUT_RDWR);
    });
    auto factory = std::optional<keeperlink::KeeperPtyFactory> {};
    factory.emplace(*link, placement);
    auto child = factory->make(
        vtpty::PageSize { vtpty::LineCount(24), vtpty::ColumnCount(80) },
        vtpty::Process::ExecInfo {
            .program = "/bin/sh",
            .arguments = { "-c", std::format("echo > {}/ran; exec sleep 300", f.directory().string()) },
            .workingDirectory = f.directory(),
            .env = {},
            .removedEnvironment = {} },
        vtpty::SandboxEscape::Contained);
    REQUIRE(child->start().has_value());
    REQUIRE(placement->parked() == 1);

    // The next daemon finds it parked, places it, and lets it run; the dead one's gate end is still held
    // here, so that only that release can have done it.
    auto const recovered = f.awaitNewDaemon(relay, RecoveryBound);
    REQUIRE(recovered > 0);
    CHECK(eventually([&] { return fs::exists(f.directory() / "ran"); }));
    CHECK(f.log().contains("was never placed; placing it now"));
    auto const status = f.client()->status();
    REQUIRE(status.has_value());
    CHECK(status->sessions == 1);

    child.reset();
    factory.reset();
    link.reset();
}

TEST_CASE("300 sessions recover", "[keeper][recovery]")
{
    constexpr auto Sessions = 300;
    auto f = KeeperFixture { HarnessSetup {
        .start = HarnessStart::Serves,
        .panes = std::ranges::to<std::vector>(std::views::repeat(std::string { "exec sleep 600" }, Sessions)),
        .snapshot = vthost::testing::HarnessSnapshot::None,
        .tcp = vthost::testing::HarnessTcp::Off,
        // A common default, below what 300 sessions take in the daemon.
        .softDescriptorLimit = 1024 } };
    INFO(f.log());
    auto const before = attach(f);
    REQUIRE(before.sessions.size() == static_cast<std::size_t>(Sessions));

    std::ignore = crashAndRecover(f);
    auto const after = attach(f);
    CHECK(after.daemonInstance == before.daemonInstance);
    CHECK(after.sessions == before.sessions);
}

TEST_CASE("a recovered daemon presents the certificate it made up before the crash", "[keeper][recovery]")
{
    auto f = KeeperFixture { HarnessSetup { .start = HarnessStart::Serves,
                                            .panes = {},
                                            .snapshot = vthost::testing::HarnessSnapshot::None,
                                            .tcp = vthost::testing::HarnessTcp::Loopback,
                                            .softDescriptorLimit = std::nullopt } };
    INFO(f.log());
    auto const port = f.nativeTcpPort();
    REQUIRE(port != 0);
    auto const before = certificateOf(port);
    REQUIRE(before.has_value());

    std::ignore = crashAndRecover(f);
    // The listener is the one the keeper kept: the same port, and the same certificate on it.
    CHECK(f.nativeTcpPort() == port);
    CHECK(certificateOf(port) == before);
}
