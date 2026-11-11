// SPDX-License-Identifier: Apache-2.0
#include <vtpty/MockPty.hpp>
#include <vtpty/ProcessPlacement.hpp>
#include <vtpty/test/ProcessTesting.hpp>

#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <vthost/LoopDrain.hpp>
#include <vthost/SessionEnd.hpp>
#include <vthost/SessionHost.hpp>
#include <vthost/handover/HandoverImage.hpp>
#include <vthost/keeperlink/KeeperChannel.hpp>
#include <vthost/keeperlink/KeeperChild.hpp>
#include <vthost/keeperlink/KeeperLink.hpp>
#include <vthost/keeperlink/KeeperPtyFactory.hpp>
#include <vthost/testing/KeeperFixture.hpp>

using namespace std::chrono_literals;
using vthost::keeperlink::KeeperChannel;
using vthost::keeperlink::KeeperChild;
using vthost::keeperlink::KeeperLink;
using vthost::keeperlink::KeeperPtyFactory;
using vthost::testing::HarnessStart;
using vthost::testing::KeeperFixture;

namespace keeper = vthost::keeper;
namespace fs = std::filesystem;

namespace
{

/// @return Whether @p name is the one /bin/sh runs as: "sh" itself, or the shell it hands over to where it is
///         a launcher (macOS runs bash, or whatever /private/var/select/sh names).
[[nodiscard]] bool isShellName(std::optional<std::string> const& name)
{
    using namespace std::string_view_literals;
    return name && std::ranges::contains(std::array { "sh"sv, "bash"sv, "dash"sv, "zsh"sv, "ksh"sv }, *name);
}

/// A real keeper, whose daemon the test plays: its own process spawns through the keeper, as a daemon does.
struct KeeperChildHarness
{
    KeeperFixture keeper { HarnessStart::Relays };
    std::unique_ptr<core::net::IoBackend> backend = core::net::makeDefaultBackend();
    core::net::EventLoop loop { *backend };
    int losses = 0;
    std::optional<KeeperLink> link;
    std::optional<KeeperPtyFactory> factory;

    /// @param hangupGrace How long the keeper gives a closed child before it kills it.
    /// @param placement Places each child before it runs.
    explicit KeeperChildHarness(
        std::chrono::milliseconds hangupGrace,
        std::shared_ptr<vtpty::ProcessPlacement> placement = std::make_shared<vtpty::NoPlacement>())
    {
        link.emplace(loop, KeeperChannel { keeper.takeChannel() }, vthost::keeper::ChannelVersion, [this] {
            ++losses;
        });
        link->post(keeper::Serving { .hungRestart = keeper::HungRestart::Off, .heartbeatTimeoutMs = 30'000 });
        factory.emplace(*link, std::move(placement), hangupGrace);
    }

    KeeperChildHarness(KeeperChildHarness const&) = delete;
    KeeperChildHarness& operator=(KeeperChildHarness const&) = delete;
    KeeperChildHarness(KeeperChildHarness&&) = delete;
    KeeperChildHarness& operator=(KeeperChildHarness&&) = delete;

    ~KeeperChildHarness()
    {
        // A clean end, not a crash: the keeper restarts nothing, and ends the shells as it goes.
        link->post(keeper::Shutdown {});
        factory.reset();
        link.reset();
    }

    /// @return A started child running @p exec.
    [[nodiscard]] std::unique_ptr<KeeperChild> start(vtpty::Process::ExecInfo exec)
    {
        auto child = factory->make(vtpty::PageSize { vtpty::LineCount(24), vtpty::ColumnCount(80) },
                                   std::move(exec),
                                   vtpty::SandboxEscape::Contained);
        auto const started = child->start();
        INFO(keeper.log());
        REQUIRE(started.has_value());
        return child;
    }

    /// Turns the loop until @p done holds, for at most @p bound.
    /// @return Whether it came to hold.
    [[nodiscard]] bool turnUntil(std::function<bool()> const& done, std::chrono::milliseconds bound = 10s)
    {
        auto const deadline = std::chrono::steady_clock::now() + bound;
        while (!done())
        {
            if (std::chrono::steady_clock::now() >= deadline)
                return false;
            std::ignore = loop.runOnce(10ms);
        }
        return true;
    }
};

/// @return Whether @p pid names a process (a zombie counts) as signal 0 finds it.
[[nodiscard]] bool exists(int64_t pid)
{
    return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno != ESRCH;
}

/// A program that ignores the hangup, as a shell's `nohup` child would, and sleeps.
[[nodiscard]] vtpty::Process::ExecInfo hangupIgnoringSleep()
{
    return vtpty::testing::execInfo("/bin/sh", { "-c", "trap '' HUP; exec sleep 30" });
}

} // namespace

TEST_CASE("a session spawned through the keeper runs and reports its exit status", "[keeper][keeperlink]")
{
    auto h = KeeperChildHarness { 200ms };
    auto const child = h.start(vtpty::testing::execInfo("/bin/sh", { "-c", "exit 7" }));
    REQUIRE(child->session().has_value());
    CHECK(child->pid() > 0);
    CHECK(child->crossing().kind == vtpty::CrossingKind::Keeper);
    CHECK(child->crossing().keeperSession == static_cast<uint64_t>(*child->session()));

    INFO(h.keeper.log());
    REQUIRE(h.turnUntil([&] { return child->checkStatus().has_value(); }));
    auto const status = *child->checkStatus();
    REQUIRE(std::holds_alternative<vtpty::NormalExit>(status));
    CHECK(std::get<vtpty::NormalExit>(status).exitCode == 7);
    CHECK(child->spawnProgress() == vtpty::SpawnProgress::Settled);
    CHECK(h.losses == 0);
}

TEST_CASE("cwd and foreground name answer for a keeper child", "[keeper][keeperlink]")
{
    auto h = KeeperChildHarness { 200ms };
    auto const directory =
        fs::canonical(fs::temp_directory_path()) / std::format("keeper-cwd-{}", std::random_device {}());
    fs::create_directories(directory);
    auto exec = vtpty::testing::execInfo("sleep", { "30" });
    exec.workingDirectory = directory;
    auto child = h.start(exec);
    // Whether its spawn is still in flight or settled already: never the keeper's own directory.
    CHECK(child->workingDirectory() == directory.string());

    INFO(h.keeper.log());
    CHECK(h.turnUntil([&] { return child->foregroundProcessName() == "sleep"; }));
    CHECK(child->workingDirectory() == directory.string());
    CHECK_FALSE(child->checkStatus().has_value());

    auto const pid = child->pid();
    child.reset();
    CHECK(h.turnUntil([&] { return !exists(pid); }, 10s));
    fs::remove_all(directory);
}

TEST_CASE("closing a keeper child hangs it up and the keeper kills it after the grace",
          "[keeper][keeperlink]")
{
    auto h = KeeperChildHarness { 500ms };
    auto const child = h.start(hangupIgnoringSleep());
    INFO(h.keeper.log());
    // Running the program, its hangup ignored.
    REQUIRE(h.turnUntil([&] { return child->foregroundProcessName() == "sleep"; }));

    auto const closedAt = std::chrono::steady_clock::now();
    child->close();
    CHECK(child->isClosed());
    // It ignores the hangup, and lives on until the keeper kills it. Measured by when it is gone, never by
    // whether it is there at some moment, which a loaded machine reaches late: the first look that finds it
    // gone is at or after its death, so the span can only be overstated, and the kill can only have come no
    // earlier than the grace allows (the Release left the loop after closedAt).
    REQUIRE(h.turnUntil([&] { return !exists(child->pid()); }, 10s));
    auto const goneAt = std::chrono::steady_clock::now();
    CHECK(goneAt - closedAt >= 500ms);
    CHECK(h.losses == 0);
}

TEST_CASE("destroying a keeper child never blocks", "[keeper][keeperlink]")
{
    auto h = KeeperChildHarness { 5'000ms };
    auto child = h.start(hangupIgnoringSleep());
    INFO(h.keeper.log());
    REQUIRE(h.turnUntil([&] { return child->foregroundProcessName() == "sleep"; }));
    auto const pid = child->pid();

    // Its program ignores the hangup and is killed only after the grace: a destructor that waited for it
    // would take that long.
    auto const before = std::chrono::steady_clock::now();
    child.reset();
    CHECK(std::chrono::steady_clock::now() - before < 1s);
    CHECK(exists(pid));
    CHECK(h.turnUntil([&] { return !exists(pid); }, 15s));
}

TEST_CASE("terminating a keeper child goes through the keeper", "[keeper][keeperlink]")
{
    auto h = KeeperChildHarness { 5'000ms };
    INFO(h.keeper.log());

    SECTION("Normal asks it to end")
    {
        auto const child = h.start(vtpty::testing::execInfo("sleep", { "30" }));
        REQUIRE(h.turnUntil([&] { return child->foregroundProcessName() == "sleep"; }));
        child->terminate(vtpty::TerminationHint::Normal);
        REQUIRE(h.turnUntil([&] { return child->checkStatus().has_value(); }));
        CHECK(std::get<vtpty::SignalExit>(*child->checkStatus()).signum == SIGTERM);
        CHECK_FALSE(child->isClosed());
    }

    SECTION("Kill ends it outright, a hangup ignored or not")
    {
        auto const child = h.start(hangupIgnoringSleep());
        REQUIRE(h.turnUntil([&] { return child->foregroundProcessName() == "sleep"; }));
        child->terminate(vtpty::TerminationHint::Kill);
        REQUIRE(h.turnUntil([&] { return child->checkStatus().has_value(); }));
        CHECK(std::get<vtpty::SignalExit>(*child->checkStatus()).signum == SIGKILL);
    }

    SECTION("Hangup closes it")
    {
        auto const child = h.start(vtpty::testing::execInfo("sleep", { "30" }));
        REQUIRE(h.turnUntil([&] { return child->foregroundProcessName() == "sleep"; }));
        child->terminate(vtpty::TerminationHint::Hangup);
        CHECK(child->isClosed());
        // Hung up as its terminal closed, well within the grace.
        CHECK(h.turnUntil([&] { return !exists(child->pid()); }, 3s));
    }
}

TEST_CASE("a keeper child runs under the daemon's environment, none of the keeper's", "[keeper][keeperlink]")
{
    auto h = KeeperChildHarness { 200ms };
    INFO(h.keeper.log());
    // 4: not set, as in this process; 3: the keeper's own leaked into it.
    auto const child =
        h.start(vtpty::testing::execInfo("/bin/sh",
                                         { "-c",
                                           std::format("if [ -n \"${{{}+x}}\" ]; then exit 3; fi; exit 4",
                                                       vthost::testing::KeeperOnlyVariable) }));
    REQUIRE(h.turnUntil([&] { return child->checkStatus().has_value(); }));
    CHECK(std::get<vtpty::NormalExit>(*child->checkStatus()).exitCode == 4);

    // The control: the keeper does run its children under the variable -- its daemon, whose environment it
    // is -- so the shell's not seeing it is the spawn's doing, not the variable's absence.
#ifdef __linux__
    REQUIRE_FALSE(h.keeper.launches().empty());
    auto file = std::ifstream { std::format("/proc/{}/environ", h.keeper.launches().front()) };
    auto const entries =
        std::string { std::istreambuf_iterator<char> { file }, std::istreambuf_iterator<char> {} };
    CHECK(entries.contains(std::format("{}=1", vthost::testing::KeeperOnlyVariable)));
#endif
}

TEST_CASE("a keeper child's spawn is in progress until its placement releases it", "[keeper][keeperlink]")
{
    auto const placement = std::make_shared<vtpty::testing::HoldingPlacement>();
    auto h = KeeperChildHarness { 200ms, placement };
    INFO(h.keeper.log());
    auto const child = h.start(vtpty::testing::execInfo("sleep", { "30" }));
    // Parked on its gate, before its terminal is set up.
    std::ignore = h.turnUntil([] { return false; }, 200ms);
    CHECK(child->spawnProgress() == vtpty::SpawnProgress::InProgress);

    placement->releaseAll();
    CHECK(h.turnUntil([&] { return child->spawnProgress() == vtpty::SpawnProgress::Settled; }));
}

namespace
{

/// A host that takes over @p image as the new image of a live upgrade does, binding every keeper child it
/// names to @p factory's link again.
struct UpgradedHost
{
    /// Names, for each session the image holds, the child it is bound to.
    using KeptOf = std::function<vthost::keeperlink::KeptChild(vthost::handover::HandoverSession const&)>;

    /// @return The child as a live upgrade's image names it: by its keeper session and pid, its status
    ///         unknown.
    [[nodiscard]] static vthost::keeperlink::KeptChild asHandedOver(
        vthost::handover::HandoverSession const& session)
    {
        return vthost::keeperlink::KeptChild { .session = keeper::KeeperSessionId { session.keeperSession },
                                               .pid = session.child.pid,
                                               .exited = std::nullopt,
                                               .settle = {} };
    }

    vthost::SessionHost host;
    vthost::LoopDrain drain;

    UpgradedHost(core::net::EventLoop& loop,
                 KeeperPtyFactory const& factory,
                 vthost::handover::HandoverImage image,
                 KeptOf keptOf = asHandedOver,
                 std::chrono::milliseconds patience = vthost::DefaultAnnouncedStatusPatience):
        host { loop,
               [](vtpty::PageSize size, std::optional<vtpty::Process::ExecInfo> const&) {
                   return std::make_unique<vtpty::MockPty>(size);
               },
               vtbackend::Settings {},
               core::defaultEnvironment(),
               vthost::SessionHostConfig { .pumping = vthost::SessionPumping::Threaded,
                                           .announcedStatusPatience = patience,
                                           .handover = std::move(image),
                                           .adoptChild =
                                               [&factory, keptOf = std::move(keptOf)](
                                                   vthost::handover::HandoverSession const& session,
                                                   std::unique_ptr<vtpty::Pty> pty) {
                                                   return factory.adopt(keptOf(session), std::move(pty));
                                               } } },
        drain { loop }
    {
    }
};

/// Records how each session ended.
struct EndRecorder final: vthost::SessionStreamEvents
{
    std::vector<vthost::SessionEnd> ends;
    void sessionEnded(vtworkspace::SessionId /*session*/, vthost::SessionEnd cause) override
    {
        ends.push_back(cause);
    }
};

/// @return A handover image of @p children, each released to the next image as an exec would (no Release).
[[nodiscard]] vthost::handover::HandoverImage imageOf(std::vector<std::unique_ptr<KeeperChild>>& children)
{
    auto image = vthost::handover::HandoverImage {};
    auto id = uint64_t { 1 };
    for (auto& child: children)
    {
        auto session = vthost::handover::HandoverSession {};
        session.id = vtworkspace::SessionId { id++ };
        session.device = vthost::handover::SessionDevice::KeeperChild;
        session.keeperSession = std::to_underlying(*child->session());
        session.child = child->crossing().child.value_or(vtpty::AdoptedChild {});
        auto released = child->releaseHandover();
        REQUIRE(released.has_value());
        session.pty = *released;
        image.sessions.push_back(std::move(session));
    }
    children.clear(); // gone with the old image: nothing of it releases the sessions
    image.nextSessionId = vtworkspace::SessionId { id };
    return image;
}

} // namespace

TEST_CASE("after a live upgrade, a keeper-held shell's exit reaches the new daemon, and closing its pane "
          "hangs it up",
          "[keeper][keeperlink][upgrade]")
{
    auto h = KeeperChildHarness { 200ms };
    INFO(h.keeper.log());

    SECTION("its exit, by the keeper's word")
    {
        // Killed by a signal: only the keeper, which reaps it, can say so; a PTY's hangup alone reads as an
        // exit.
        auto children = std::vector<std::unique_ptr<KeeperChild>> {};
        children.push_back(
            h.start(vtpty::testing::execInfo("/bin/sh", { "-c", "read line; kill -KILL $$" })));
        REQUIRE(h.turnUntil([&] { return isShellName(children.front()->foregroundProcessName()); }));
        auto upgraded = UpgradedHost { h.loop, *h.factory, imageOf(children) };
        REQUIRE(upgraded.host.sessionCount() == 1);
        auto ends = EndRecorder {};
        upgraded.host.subscribeStream(&ends);
        upgraded.host.writeInput(upgraded.host.sessionIds().front(), "go\n");
        // The loop busy elsewhere well past the hangup: the keeper's word is waited for, not a moment.
        std::this_thread::sleep_for(300ms);
        REQUIRE(h.turnUntil([&] { return !ends.ends.empty(); }));
        CHECK(ends.ends == std::vector { vthost::SessionEnd::Killed });
        upgraded.host.unsubscribeStream(&ends);
    }

    SECTION("its close, as a release to the keeper")
    {
        auto children = std::vector<std::unique_ptr<KeeperChild>> {};
        children.push_back(h.start(vtpty::testing::execInfo("sleep", { "300" })));
        REQUIRE(h.turnUntil([&] { return children.front()->foregroundProcessName() == "sleep"; }));
        auto const pid = children.front()->pid();
        auto upgraded = UpgradedHost { h.loop, *h.factory, imageOf(children) };
        REQUIRE(upgraded.host.sessionCount() == 1);
        CHECK(exists(pid)); // the old image's going released nothing
        upgraded.host.closeSession(upgraded.host.sessionIds().front());
        // The keeper let go of its master too: the program is hung up.
        CHECK(h.turnUntil([&] { return !exists(pid); }, 10s));
    }
}

TEST_CASE("a session whose keeper never tells its exit ends as exited once the patience runs out",
          "[keeper][keeperlink][upgrade]")
{
    auto h = KeeperChildHarness { 200ms };
    INFO(h.keeper.log());
    auto children = std::vector<std::unique_ptr<KeeperChild>> {};
    children.push_back(h.start(vtpty::testing::execInfo("/bin/sh", { "-c", "read line; kill -KILL $$" })));
    REQUIRE(h.turnUntil([&] { return isShellName(children.front()->foregroundProcessName()); }));
    // Bound to a keeper session the keeper never reports on: its Exited goes to a name nothing watches.
    auto upgraded = UpgradedHost {
        h.loop,
        *h.factory,
        imageOf(children),
        [](vthost::handover::HandoverSession const& session) {
            auto kept = UpgradedHost::asHandedOver(session);
            kept.session = keeper::KeeperSessionId { 999'999 };
            return kept;
        },
        500ms,
    };
    auto ends = EndRecorder {};
    upgraded.host.subscribeStream(&ends);
    auto const askedAt = std::chrono::steady_clock::now();
    upgraded.host.writeInput(upgraded.host.sessionIds().front(), "go\n");
    REQUIRE(h.turnUntil([&] { return !ends.ends.empty(); }));
    // Not before the patience ran out: until then the session waits for the word.
    CHECK(std::chrono::steady_clock::now() - askedAt >= 500ms);
    CHECK(ends.ends == std::vector { vthost::SessionEnd::Exited });
    upgraded.host.unsubscribeStream(&ends);
}

TEST_CASE("a recovered session whose program ended while no daemon ran is closed at once with its status",
          "[keeper][keeperlink][recovery]")
{
    auto h = KeeperChildHarness { 200ms };
    INFO(h.keeper.log());
    // Its program still holds the terminal: only the status the keeper kept can end the session.
    auto children = std::vector<std::unique_ptr<KeeperChild>> {};
    children.push_back(h.start(vtpty::testing::execInfo("sleep", { "300" })));
    REQUIRE(h.turnUntil([&] { return children.front()->foregroundProcessName() == "sleep"; }));
    auto upgraded = UpgradedHost {
        h.loop,
        *h.factory,
        imageOf(children),
        [](vthost::handover::HandoverSession const& session) {
            auto kept = UpgradedHost::asHandedOver(session);
            kept.exited = vtpty::SignalExit { .signum = SIGKILL };
            return kept;
        },
    };
    auto ends = EndRecorder {};
    upgraded.host.subscribeStream(&ends);
    REQUIRE(h.turnUntil([&] { return !ends.ends.empty(); }, 2s));
    CHECK(ends.ends == std::vector { vthost::SessionEnd::Killed });
    upgraded.host.unsubscribeStream(&ends);
}

namespace
{
/// @return @p status as the channel carries it, which compares.
[[nodiscard]] std::optional<keeper::WireExitStatus> wireOf(std::optional<vtpty::ExitStatus> const& status)
{
    return status.transform(&keeper::WireExitStatus::from);
}
} // namespace

TEST_CASE("a child's exit is settled once, by its status or by the keeper's loss", "[keeper][keeperlink]")
{
    using vthost::keeperlink::ChildExit;
    struct Row
    {
        std::string_view name;
        std::function<void(ChildExit&)> settle;
        std::optional<vtpty::ExitStatus> status;
    };
    auto const rows = std::array {
        Row { .name = "recorded",
              .settle = [](ChildExit& exit) { exit.record(vtpty::NormalExit { .exitCode = 3 }); },
              .status = vtpty::NormalExit { .exitCode = 3 } },
        Row { .name = "the first record stands",
              .settle =
                  [](ChildExit& exit) {
                      exit.record(vtpty::SignalExit { .signum = SIGKILL });
                      exit.record(vtpty::NormalExit { .exitCode = 3 });
                  },
              .status = vtpty::SignalExit { .signum = SIGKILL } },
        Row {
            .name = "abandoned", .settle = [](ChildExit& exit) { exit.abandon(); }, .status = std::nullopt },
        Row { .name = "abandoned after a record keeps the record",
              .settle =
                  [](ChildExit& exit) {
                      exit.record(vtpty::NormalExit { .exitCode = 7 });
                      exit.abandon();
                  },
              .status = vtpty::NormalExit { .exitCode = 7 } },
    };
    for (auto const& row: rows)
    {
        CAPTURE(row.name);
        auto exit = ChildExit {};
        auto before = 0;
        exit.whenKnown([&] { ++before; });
        CHECK(before == 0);
        row.settle(exit);
        CHECK(before == 1); // once, however many settle it
        auto after = 0;
        exit.whenKnown([&] { ++after; });
        CHECK(after == 1); // at once, once settled
        CHECK(wireOf(exit.status()) == wireOf(row.status));
    }
}

TEST_CASE("Exited reaches the watched child once; the keeper's loss settles every wait",
          "[keeper][keeperlink]")
{
    using vthost::keeperlink::KeeperExits;
    auto exits = KeeperExits {};
    auto const first = exits.watch(keeper::KeeperSessionId { 1 });
    auto const second = exits.watch(keeper::KeeperSessionId { 2 });
    auto settled = 0;
    second->whenKnown([&] { ++settled; });

    exits.exited(keeper::KeeperSessionId { 1 }, vtpty::NormalExit { .exitCode = 1 });
    // Told again -- what a keeper re-sends to the new image of a live upgrade -- nothing changes.
    exits.exited(keeper::KeeperSessionId { 1 }, vtpty::NormalExit { .exitCode = 2 });
    // For a session nobody watches: ignored.
    exits.exited(keeper::KeeperSessionId { 99 }, vtpty::NormalExit { .exitCode = 9 });
    CHECK(wireOf(first->status()) == wireOf(vtpty::NormalExit { .exitCode = 1 }));
    CHECK(settled == 0);

    exits.abandonAll();
    CHECK(settled == 1);
    CHECK_FALSE(second->status().has_value());
}

TEST_CASE("a session waiting for its exit status that is closed on request ends as requested at once",
          "[keeper][keeperlink][upgrade]")
{
    auto h = KeeperChildHarness { 200ms };
    INFO(h.keeper.log());
    auto children = std::vector<std::unique_ptr<KeeperChild>> {};
    children.push_back(h.start(vtpty::testing::execInfo("/bin/sh", { "-c", "read line; kill -KILL $$" })));
    REQUIRE(h.turnUntil([&] { return isShellName(children.front()->foregroundProcessName()); }));
    // Bound to a keeper session the keeper never reports on: its end waits the whole patience.
    auto upgraded = UpgradedHost {
        h.loop,
        *h.factory,
        imageOf(children),
        [](vthost::handover::HandoverSession const& session) {
            auto kept = UpgradedHost::asHandedOver(session);
            kept.session = keeper::KeeperSessionId { 999'999 };
            return kept;
        },
        30s,
    };
    auto ends = EndRecorder {};
    upgraded.host.subscribeStream(&ends);
    auto const session = upgraded.host.sessionIds().front();
    upgraded.host.writeInput(session, "go\n");
    // Its program is gone and its pane waits for the word; a close asked for does not.
    std::ignore = h.turnUntil([] { return false; }, 500ms);
    REQUIRE(ends.ends.empty());
    upgraded.host.closeSession(session);
    REQUIRE(h.turnUntil([&] { return !ends.ends.empty(); }, 2s));
    CHECK(ends.ends == std::vector { vthost::SessionEnd::Requested });
    CHECK(upgraded.host.sessionCount() == 0);
    upgraded.host.unsubscribeStream(&ends);
}
