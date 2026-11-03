// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#ifndef _WIN32
    #include <vtpty/Process.hpp>
    #include <vtpty/ProcessPlacement.hpp>
    #include <vtpty/test/ProcessTesting.hpp>

    #include <core/net/Sockets.hpp>
    #include <core/testing/ScopedTempDir.hpp>

    #include <sys/socket.h>
    #include <sys/un.h>

    #include <algorithm>
    #include <charconv>
    #include <chrono>
    #include <csignal>
    #include <cstddef>
    #include <cstring>
    #include <exception>
    #include <filesystem>
    #include <format>
    #include <functional>
    #include <map>
    #include <memory>
    #include <new>
    #include <optional>
    #include <ranges>
    #include <set>
    #include <string>
    #include <string_view>
    #include <tuple>
    #include <utility>
    #include <variant>
    #include <vector>

    #include <unistd.h>

    #include <vthost/ConnectionAcceptor.hpp>
    #include <vthost/DaemonBinary.hpp>
    #include <vthost/DaemonImage.hpp>
    #include <vthost/LoopDrain.hpp>
    #include <vthost/SessionHost.hpp>
    #include <vthost/handover/HandoverCodec.hpp>
    #include <vthost/handover/HandoverRestore.hpp>
    #include <vthost/handover/LiveUpgrade.hpp>
    #include <vthost/handover/Readiness.hpp>
    #include <vthost/testing/FakeHandoverMemory.hpp>
    #include <vthost/testing/FakeImageLeases.hpp>
    #include <vthost/testing/FakeProcessImageReplacer.hpp>
    #include <vthost/testing/FakeReadinessProbe.hpp>
    #include <vthost/testing/HandoverTesting.hpp>
    #include <vthost/testing/StagingFileSystem.hpp>
    #include <vthost/testing/TestHelpers.hpp>

using namespace vthost;
using namespace vthost::handover;
using namespace std::chrono_literals;
using vthost::testing::LiveHost;
using vthost::testing::occurrences;
using vthost::testing::releaseForAdoption;
using vtworkspace::SessionId;

namespace
{

/// What a target binary built from this tree answers.
[[nodiscard]] ReadinessReport readyTarget()
{
    return ownReadiness("0.9.1");
}

/// UpgradeHooks that record what they were asked, in order.
class RecordingHooks final: public UpgradeHooks
{
  public:
    void announceUpgrade(std::string_view toRelease) override
    {
        events.push_back(std::format("announce {}", toRelease));
    }

    void writeWorkspaceSnapshot() override { events.emplace_back("snapshot"); }

    void closeClients() override
    {
        events.emplace_back("close");
        if (onClose)
            onClose();
    }

    void abortClients() override
    {
        events.emplace_back("abort");
        if (onAbort)
            onAbort();
    }

    std::vector<std::string> events;
    std::function<void()> onClose; ///< Runs as the clients are closed, while the sessions still run.
    std::function<void()> onAbort; ///< Runs as the clients are closed at once.
};

/// Thrown from the fake replacer where the exec succeeds: it ends run() as the exec ends the old image.
struct ImageReplaced: std::exception
{
};

/// Counts the output bytes the sessions of a host read.
class ByteCounter final: public SessionStreamEvents
{
  public:
    [[nodiscard]] bool mirrorsScreens() const noexcept override { return false; }
    void sessionOutput(SessionId /*session*/, std::string const& bytes) override { count += bytes.size(); }

    std::size_t count = 0;
};

/// A connection handler that lets every connection go at once.
[[nodiscard]] core::async::Task<void> ignoreConnection(ConnectionId /*id*/,
                                                       std::unique_ptr<core::net::ISocket> /*connection*/)
{
    co_return;
}

/// @return The echoing shell on a PTY of its own.
[[nodiscard]] std::unique_ptr<vtpty::Pty> catDevice(vtpty::PageSize size)
{
    return vthost::testing::shellProcess(size);
}

/// @return The descriptor the last argument names, `--handover-fd=N`.
[[nodiscard]] int handoverFdOf(std::vector<std::string> const& argv)
{
    constexpr auto Prefix = std::string_view { "--handover-fd=" };
    REQUIRE_FALSE(argv.empty());
    REQUIRE(argv.back().starts_with(Prefix));
    auto fd = -1;
    auto const digits = std::string_view { argv.back() }.substr(Prefix.size());
    REQUIRE(std::from_chars(digits.data(), digits.data() + digits.size(), fd).ec == std::errc {});
    return fd;
}

/// @return A descriptor connected to the unix socket at @p path; the kernel completes the connect into the
///         listener's backlog whether or not anything accepts.
[[nodiscard]] int connectRaw(std::string const& path)
{
    auto const fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    auto address = sockaddr_un {};
    address.sun_family = AF_UNIX;
    REQUIRE(path.size() < sizeof(address.sun_path));
    std::memcpy(address.sun_path, path.data(), path.size());
    REQUIRE(::connect(fd, reinterpret_cast<sockaddr const*>(&address), sizeof(address)) == 0);
    return fd;
}

/// @return The pid of @p session's child.
[[nodiscard]] int64_t childOf(SessionHost& host, SessionId session)
{
    auto const* const process = host.hostedSession(session)->process();
    REQUIRE(process != nullptr);
    auto const child = process->handoverChild();
    REQUIRE(child.has_value());
    return child->pid;
}

/// A daemon about to be upgraded: a live host, a listener, a store with the target staged, and fakes for
/// everything that would leave the process.
struct Upgrade
{
    /// @param devices The device of each session the host spawns.
    /// @param settings The sessions' terminal settings.
    explicit Upgrade(LiveHost::Devices devices = catDevice, vtbackend::Settings settings = {}):
        live { std::make_unique<LiveHost>(
            std::move(devices), LiveHost::sequentialIds(), std::move(settings)) }
    {
        std::ignore = files.writeFile("/home/u/Contour.AppImage", "ELF");
        auto staged = store.stage();
        REQUIRE(staged.has_value());
        target = staged->executable;
        imageId = store.imageDirectoryOf(target).filename().string();
        // Staged by the client that asks for the upgrade; its lease ends with that client.
        staged->lease.reset();

        auto listener = core::net::listenUnix(live->loop, socketPath);
        REQUIRE(listener.has_value());
        acceptor = std::make_unique<ConnectionAcceptor>(
            live->loop, "control", std::move(*listener), ignoreConnection);
        acceptors.push_back(acceptor.get());
        live->loop.spawn(acceptor->serve());
    }

    Upgrade(Upgrade const&) = delete;
    Upgrade& operator=(Upgrade const&) = delete;
    Upgrade(Upgrade&&) = delete;
    Upgrade& operator=(Upgrade&&) = delete;

    ~Upgrade()
    {
        if (!acceptor || !live)
            return;
        // Its accept loop ends before it goes.
        acceptor->close();
        std::ignore = live->loop.runOnce(10ms);
        std::ignore = live->loop.runOnce(10ms);
    }

    /// @return The orchestrator, over this daemon.
    [[nodiscard]] LiveUpgrade orchestrator(LiveUpgradeConfig config)
    {
        return LiveUpgrade { live->loop, live->host, store, leases,      replacer,
                             memory,     probe,      hooks, listeners(), std::move(config) };
    }

    /// @return The configuration a daemon of release 0.9.0 upgrades with.
    [[nodiscard]] static LiveUpgradeConfig config()
    {
        return LiveUpgradeConfig {
            .release = "0.9.0",
            .daemonInstance = 0xC0FFEE,
            .argv = { "/state/bin/0.9.0-00000000/Contour.AppImage",
                      "daemon",
                      "--handover-fd=7",
                      "--label",
                      "x" },
            .installedExecutable = {},
            .sandbox = vtpty::SandboxState::Host,
            .sessionStartAttempts = 3,
            .sessionStartRetryDelay = 20ms,
            .settleTimeout = 5000ms,
            .settlePollInterval = 2ms,
        };
    }

    /// @return Upgrades with @p upgrade to @p to, on the host's loop.
    [[nodiscard]] std::expected<void, UpgradeError> run(LiveUpgrade& upgrade, UpgradeTarget to) const
    {
        return live->loop.blockOn(upgrade.run(std::move(to)));
    }

    /// @return Whether every session's screen shows @p text.
    [[nodiscard]] bool allShow(std::string_view text) const
    {
        return std::ranges::all_of(live->host.sessionIds(),
                                   [&](SessionId id) { return live->pageText(id).contains(text); });
    }

    /// @return The acceptor, as the orchestrator takes its listeners.
    [[nodiscard]] std::span<ConnectionAcceptor* const> listeners() { return acceptors; }

    vthost::testing::StagingFileSystem files;
    vthost::testing::FakeImageLeases leases;
    vthost::testing::FakeSymbolicLinks links;
    DaemonImageStore store {
        vthost::testing::appImageStoreConfig("/state/bin", "0.9.1"), files, leases, links
    };
    vthost::testing::FakeProcessImageReplacer replacer;
    vthost::testing::FakeHandoverMemory memory;
    vthost::testing::FakeReadinessProbe probe;
    RecordingHooks hooks;
    std::filesystem::path target;
    std::string imageId;
    core::testing::ScopedTempDir socketDir { "contour-upgrade" };
    std::string socketPath = (socketDir.path() / "control").string();
    std::unique_ptr<LiveHost> live;
    std::unique_ptr<ConnectionAcceptor> acceptor;

    std::vector<ConnectionAcceptor*> acceptors;
};

/// @return Whether @p result is the refusal @p refusal.
[[nodiscard]] bool refusedWith(std::expected<void, UpgradeError> const& result, UpgradeRefusal refusal)
{
    return !result.has_value() && std::holds_alternative<UpgradeRefusal>(result.error())
           && std::get<UpgradeRefusal>(result.error()) == refusal;
}

} // namespace

TEST_CASE("live upgrade keeps every child alive and the same pid", "[vthost][handover][upgrade][threading]")
{
    auto u = Upgrade {};
    u.live->spawnTwo();
    auto const ids = u.live->host.sessionIds();
    REQUIRE(u.live->eventually([&] { return u.allShow("ready"); }));
    auto children = std::map<uint64_t, int64_t> {};
    for (auto const id: ids)
        children.emplace(id.value, childOf(u.live->host, id));
    auto const listenerFd = u.acceptor->listeningDescriptor();
    REQUIRE(listenerFd.has_value());

    auto next = std::unique_ptr<LiveHost> {};
    auto image = std::optional<HandoverImage> {};
    u.replacer.onReplace = [&] {
        // The exec: everything the image names, and the image itself, survive it; the target stays leased.
        auto const& [program, argv] = u.replacer.replacements.back();
        auto const fd = handoverFdOf(argv);
        auto decoded = readHandover(u.memory, fd);
        REQUIRE(decoded.has_value());
        auto expected = descriptorsNamedBy(*decoded);
        expected.push_back(fd);
        CHECK(std::ranges::includes(u.memory.kept, std::set<int>(expected.begin(), expected.end())));
        CHECK(u.leases.holds(u.store.leaseFileOf(u.store.imageDirectoryOf(u.target))) == 1);
        CHECK(u.acceptor->admission() == Admission::Holding);
        image = *decoded;
        // The new image takes the PTYs and children over from the image; the old one is gone.
        releaseForAdoption(u.live->host);
        auto config = LiveHost::sequentialIds();
        config.handover = *std::move(decoded);
        next = std::make_unique<LiveHost>(catDevice, std::move(config));
        throw ImageReplaced {};
    };

    auto upgrade = u.orchestrator(Upgrade::config());
    REQUIRE_THROWS_AS(std::ignore = u.run(upgrade, UpgradeTarget { .imageId = u.imageId }), ImageReplaced);

    // Probed, announced, snapshotted and closed, in that order, then exec'd as the target.
    CHECK(u.probe.asked == std::vector { u.target });
    CHECK(u.hooks.events == std::vector<std::string> { "announce 0.9.1", "snapshot", "close" });
    REQUIRE(u.replacer.replacements.size() == 1);
    auto const& [program, argv] = u.replacer.replacements.front();
    CHECK(program == u.target);
    CHECK(argv.front() == u.target.string());
    CHECK(std::ranges::count_if(argv, [](std::string const& arg) { return arg.starts_with("--handover-fd"); })
          == 1);
    CHECK(std::ranges::find(argv, "--label") != argv.end());

    REQUIRE(image.has_value());
    CHECK(image->daemonInstance == 0xC0FFEE);
    CHECK(image->fromRelease == "0.9.0");
    REQUIRE(image->listeners.size() == 1);
    CHECK(image->listeners.front().name == "control");
    CHECK(image->listeners.front().fd == *listenerFd);
    CHECK(image->leaseFd >= vthost::testing::FakeImageLeases::FirstDescriptor);

    // The old image's host lets go of nothing more as it goes.
    u.acceptor.reset();
    u.live.reset();

    // The same programs, the same pids, still running and answering.
    REQUIRE(next != nullptr);
    CHECK(next->host.handoverOutcome().has_value());
    REQUIRE(next->host.sessionIds() == ids);
    for (auto const id: ids)
    {
        CHECK(childOf(next->host, id) == children.at(id.value));
        CHECK(::kill(static_cast<pid_t>(children.at(id.value)), 0) == 0);
        next->host.writeInput(id, "after-upgrade");
    }
    REQUIRE(next->eventually([&] {
        return std::ranges::all_of(
            ids, [&](SessionId id) { return next->pageText(id).contains("after-upgrade"); });
    }));
}

TEST_CASE("live upgrade loses no output from a flooding program", "[vthost][handover][upgrade][threading]")
{
    // Enough output that the switch falls in the middle of it, at the PTY's backpressure.
    static constexpr auto FloodBytes = std::size_t { 50 } << 20;
    auto oldCount = ByteCounter {};
    auto newCount = ByteCounter {};
    auto u = Upgrade { [](vtpty::PageSize size) {
        return vthost::testing::shellProcess(
            size, std::format("stty raw -echo; head -c {} /dev/zero | tr '\\0' x; exec cat", FloodBytes));
    } };
    u.live->host.subscribeStream(&oldCount);
    REQUIRE(u.live->host.createTab() != nullptr);
    auto const id = u.live->host.sessionIds().front();
    REQUIRE(u.live->eventually([&] { return oldCount.count > 0; }));
    auto const child = childOf(u.live->host, id);

    auto next = std::unique_ptr<LiveHost> {};
    auto countAtExec = std::size_t { 0 };
    u.replacer.onReplace = [&] {
        countAtExec = oldCount.count;
        auto image = readHandover(u.memory, handoverFdOf(u.replacer.replacements.back().second));
        REQUIRE(image.has_value());
        releaseForAdoption(u.live->host);
        auto config = LiveHost::sequentialIds();
        config.handover = *std::move(image);
        next = std::make_unique<LiveHost>(catDevice, std::move(config));
        // Before its loop first turns: the new host's first output is posted, not yet fanned out.
        next->host.subscribeStream(&newCount);
        throw ImageReplaced {};
    };
    auto upgrade = u.orchestrator(Upgrade::config());
    REQUIRE_THROWS_AS(std::ignore = u.run(upgrade, UpgradeTarget { .imageId = u.imageId }), ImageReplaced);
    u.live->host.unsubscribeStream(&oldCount);
    u.acceptor.reset();
    u.live.reset();

    // The switch fell inside the flood, and every byte arrived exactly once, through one host or the other.
    CHECK(countAtExec < FloodBytes);
    REQUIRE(next != nullptr);
    auto const deadline = std::chrono::steady_clock::now() + 120s;
    while (oldCount.count + newCount.count < FloodBytes && std::chrono::steady_clock::now() < deadline)
        std::ignore = next->loop.runOnce(10ms);
    // A moment longer, for a byte that would arrive twice.
    for ([[maybe_unused]] auto const turn: std::views::iota(0, 30))
        std::ignore = next->loop.runOnce(10ms);
    CHECK(oldCount.count == countAtExec);
    CHECK(newCount.count > 0);
    CHECK(oldCount.count + newCount.count == FloodBytes);
    CHECK(childOf(next->host, id) == child);
    next->host.unsubscribeStream(&newCount);
}

TEST_CASE("exec failure resumes the old daemon with no lost input", "[vthost][handover][upgrade][threading]")
{
    auto u = Upgrade {};
    u.live->spawnTwo();
    auto const ids = u.live->host.sessionIds();
    REQUIRE(u.live->eventually([&] { return u.allShow("ready"); }));

    auto fd = -1;
    u.replacer.onReplace = [&] {
        fd = handoverFdOf(u.replacer.replacements.back().second);
        // Typed while the daemon was stopped: carried, and delivered once it resumes.
        u.live->host.writeInput(ids[0], "carried");
    };
    auto upgrade = u.orchestrator(Upgrade::config());
    auto const result = u.run(upgrade, UpgradeTarget { .imageId = u.imageId });

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error()
          == UpgradeError { ExecFailure { std::make_error_code(std::errc::no_such_file_or_directory) } });
    CHECK(upgrade.state() == UpgradeState::Idle);
    // Every descriptor made to survive the exec is close-on-exec again; the memory object is gone.
    CHECK(u.memory.kept.empty());
    CHECK(u.memory.closedOnExecAgain.size() == u.memory.keepCalls);
    CHECK(u.memory.closedOnExecAgain.contains(fd));
    CHECK(u.memory.discarded.contains(fd));
    CHECK(u.memory.objects.empty());
    CHECK(u.leases.heldCount() == 0);

    // Accepting again, and every session answering, the carried input first.
    CHECK(u.acceptor->admission() == Admission::Accepting);
    auto const client = connectRaw(u.socketPath);
    REQUIRE(u.live->eventually([&] { return u.acceptor->acceptedCount() == 1; }));
    ::close(client);
    u.live->host.writeInput(ids[0], "-after");
    u.live->host.writeInput(ids[1], "second");
    REQUIRE(u.live->eventually([&] { return u.live->pageText(ids[0]).contains("carried-after"); }));
    REQUIRE(u.live->eventually([&] { return u.live->pageText(ids[1]).contains("second"); }));
    CHECK(occurrences(u.live->pageText(ids[0]), "carried") == 1);
}

TEST_CASE("unreadable format refuses before quiescing", "[vthost][handover][upgrade][threading]")
{
    auto u = Upgrade {};
    REQUIRE(u.live->host.createTab() != nullptr);
    auto const id = u.live->host.sessionIds().front();
    REQUIRE(u.live->eventually([&] { return u.allShow("ready"); }));

    auto newer = readyTarget();
    newer.handoverFormats = { HandoverFormatVersion + 1, HandoverFormatVersion + 1 };
    u.probe.answer = newer;
    auto upgrade = u.orchestrator(Upgrade::config());
    CHECK(refusedWith(u.run(upgrade, UpgradeTarget { .imageId = u.imageId }),
                      UpgradeRefusal::FormatUnreadable));

    // Nothing announced, stopped or written; the target's lease let go.
    CHECK(u.hooks.events.empty());
    CHECK(u.replacer.replacements.empty());
    CHECK(u.memory.objects.empty());
    CHECK(u.leases.heldCount() == 0);
    CHECK(u.acceptor->admission() == Admission::Accepting);
    u.live->host.writeInput(id, "still-here");
    REQUIRE(u.live->eventually([&] { return u.live->pageText(id).contains("still-here"); }));
}

TEST_CASE("an unresponsive target refuses before quiescing", "[vthost][handover][upgrade]")
{
    auto u = Upgrade {};
    u.probe.answer = std::unexpected(UpgradeRefusal::TargetUnresponsive);
    auto upgrade = u.orchestrator(Upgrade::config());
    CHECK(refusedWith(u.run(upgrade, UpgradeTarget { .imageId = u.imageId }),
                      UpgradeRefusal::TargetUnresponsive));
    CHECK(u.hooks.events.empty());
    CHECK(u.leases.heldCount() == 0);
}

TEST_CASE("a target that cannot load the daemon's configuration refuses before quiescing",
          "[vthost][handover][upgrade][threading]")
{
    auto u = Upgrade {};
    REQUIRE(u.live->host.createTab() != nullptr);
    auto const id = u.live->host.sessionIds().front();
    REQUIRE(u.live->eventually([&] { return u.allShow("ready"); }));
    auto broken = readyTarget();
    broken.configProblem = "cannot load config: yaml-cpp: error at line 3";
    u.probe.answer = broken;

    auto config = Upgrade::config();
    config.configFile = "/home/u/.config/contour/contour.yml";
    config.profile = "work";
    auto upgrade = u.orchestrator(std::move(config));
    CHECK(refusedWith(u.run(upgrade, UpgradeTarget { .imageId = u.imageId }),
                      UpgradeRefusal::ConfigUnreadable));

    // The target was asked about the very file and profile the daemon runs on.
    REQUIRE(u.probe.queries.size() == 1);
    CHECK(u.probe.queries.front().executable == u.target);
    CHECK(u.probe.queries.front().configFile == "/home/u/.config/contour/contour.yml");
    CHECK(u.probe.queries.front().profile == "work");
    // Refused while the daemon still serves: nothing announced, stopped or written.
    CHECK(u.hooks.events.empty());
    CHECK(u.replacer.replacements.empty());
    CHECK(u.leases.heldCount() == 0);
    CHECK(u.acceptor->admission() == Admission::Accepting);
    u.live->host.writeInput(id, "still-here");
    REQUIRE(u.live->eventually([&] { return u.live->pageText(id).contains("still-here"); }));
}

TEST_CASE("a daemon already on built-in defaults upgrades to a target that cannot load the configuration",
          "[vthost][handover][upgrade][threading]")
{
    auto u = Upgrade {};
    REQUIRE(u.live->host.createTab() != nullptr);
    REQUIRE(u.live->eventually([&] { return u.allShow("ready"); }));
    auto broken = readyTarget();
    broken.configProblem = "cannot load config: yaml-cpp: error at line 3";
    u.probe.answer = broken;

    auto config = Upgrade::config();
    config.configFile = "/home/u/.config/contour/contour.yml";
    config.runningConfig = RunningConfig::BuiltInDefaults;
    auto upgrade = u.orchestrator(std::move(config));
    auto const result = u.run(upgrade, UpgradeTarget { .imageId = u.imageId });

    // Not refused for the configuration: it went on as far as the exec, which the fake fails.
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error()
          == UpgradeError { ExecFailure { std::make_error_code(std::errc::no_such_file_or_directory) } });
    CHECK(u.replacer.replacements.size() == 1);
}

TEST_CASE("oversize in-flight tail refuses before quiescing", "[vthost][handover][upgrade][threading]")
{
    auto settings = vtbackend::Settings {};
    settings.inFlightTailLimit = 16;
    auto u = Upgrade {
        [](vtpty::PageSize size) {
            return vthost::testing::shellProcess(
                size, "stty raw -echo; printf '\\033]2;0123456789012345678901234567890123456789'; exec cat");
        },
        settings,
    };
    REQUIRE(u.live->host.createTab() != nullptr);
    auto const id = u.live->host.sessionIds().front();
    REQUIRE(u.live->eventually([&] {
        auto& terminal = u.live->host.hostedSession(id)->terminal();
        auto const _ = std::lock_guard { terminal };
        return terminal.inFlightTailCompleteness() == vtbackend::TailCompleteness::Overflowed;
    }));

    auto upgrade = u.orchestrator(Upgrade::config());
    CHECK(refusedWith(u.run(upgrade, UpgradeTarget { .imageId = u.imageId }),
                      UpgradeRefusal::InFlightTailTooLarge));
    CHECK(u.hooks.events.empty());
    CHECK(u.replacer.replacements.empty());

    // The session runs on: the sequence ends, and output shows.
    u.live->host.writeInput(id, "\033\\ok");
    REQUIRE(u.live->eventually([&] { return u.live->pageText(id).contains("ok"); }));
}

TEST_CASE("a session no new image could adopt refuses before quiescing",
          "[vthost][handover][upgrade][threading]")
{
    // A device that is no PTY of this process, as an SSH or channel session's is.
    auto u = Upgrade { [](vtpty::PageSize size) -> std::unique_ptr<vtpty::Pty> {
        return std::make_unique<vthost::testing::StallingPty>(size);
    } };
    REQUIRE(u.live->host.createTab() != nullptr);
    auto upgrade = u.orchestrator(Upgrade::config());
    CHECK(refusedWith(u.run(upgrade, UpgradeTarget { .imageId = u.imageId }),
                      UpgradeRefusal::UnsupportedSession));
    CHECK(u.hooks.events.empty());
    CHECK(u.live->host.sessionCount() == 1);
}

TEST_CASE("a target outside the store is refused", "[vthost][handover][upgrade]")
{
    auto u = Upgrade {};
    auto upgrade = u.orchestrator(Upgrade::config());
    for (auto const* const imageId: { "../../usr/bin", "/usr/bin/contour", "0.9.1-00000000", "" })
    {
        INFO(imageId);
        CHECK(refusedWith(u.run(upgrade, UpgradeTarget { .imageId = imageId }), UpgradeRefusal::NotStaged));
    }
    CHECK(u.probe.asked.empty());
    CHECK(u.hooks.events.empty());
    CHECK(u.replacer.replacements.empty());
}

TEST_CASE("Flatpak refuses with Sandboxed", "[vthost][handover][upgrade]")
{
    auto u = Upgrade {};
    auto config = Upgrade::config();
    config.sandbox = vtpty::SandboxState::Flatpak;
    auto upgrade = u.orchestrator(std::move(config));
    CHECK(refusedWith(u.run(upgrade, UpgradeTarget { .imageId = u.imageId }), UpgradeRefusal::Sandboxed));
    CHECK(u.probe.asked.empty());
    CHECK(u.leases.heldCount() == 0);
    CHECK(u.hooks.events.empty());
}

namespace
{
/// @return Devices whose first session runs at once, and whose every later one is parked by @p placement
///         before its terminal is set up.
[[nodiscard]] LiveHost::Devices firstRunsThenParked(
    std::shared_ptr<vtpty::testing::HoldingPlacement> placement)
{
    return [placement = std::move(placement), spawned = 0](vtpty::PageSize size) mutable {
        return vthost::testing::shellProcess(
            size,
            vthost::testing::CatScript,
            spawned++ == 0
                ? std::shared_ptr<vtpty::ProcessPlacement> { std::make_shared<vtpty::NoPlacement>() }
                : std::shared_ptr<vtpty::ProcessPlacement> { placement });
    };
}
} // namespace

TEST_CASE("a session still starting is refused before anything is announced",
          "[vthost][handover][upgrade][threading]")
{
    auto placement = std::make_shared<vtpty::testing::HoldingPlacement>();
    auto u = Upgrade { firstRunsThenParked(placement) };
    u.live->spawnTwo();
    auto const ids = u.live->host.sessionIds();
    REQUIRE(u.live->eventually([&] { return u.live->pageText(ids[0]).contains("ready"); }));

    auto upgrade = u.orchestrator(Upgrade::config());
    CHECK(
        refusedWith(u.run(upgrade, UpgradeTarget { .imageId = u.imageId }), UpgradeRefusal::SessionStarting));
    // No client was told, snapshotted for or closed for nothing.
    CHECK(u.hooks.events.empty());
    CHECK(u.replacer.replacements.empty());
    placement->releaseAll();
    REQUIRE(u.live->eventually([&] { return u.live->pageText(ids[1]).contains("ready"); }));
}

TEST_CASE("a session that keeps starting is waited for, then refused, and every session runs on",
          "[vthost][handover][upgrade][threading]")
{
    // The second session starts as the clients go, after the checks: its child is parked before its terminal
    // is set up.
    auto placement = std::make_shared<vtpty::testing::HoldingPlacement>();
    auto u = Upgrade { firstRunsThenParked(placement) };
    REQUIRE(u.live->host.createTab() != nullptr);
    auto const first = u.live->host.sessionIds().front();
    REQUIRE(u.live->eventually([&] { return u.live->pageText(first).contains("ready"); }));
    u.hooks.onClose = [&] {
        REQUIRE(u.live->host.createTab() != nullptr);
    };

    auto upgrade = u.orchestrator(Upgrade::config());
    CHECK(
        refusedWith(u.run(upgrade, UpgradeTarget { .imageId = u.imageId }), UpgradeRefusal::SessionStarting));
    CHECK(u.replacer.replacements.empty());
    CHECK(u.acceptor->admission() == Admission::Accepting);
    u.live->host.writeInput(first, "resumed");
    REQUIRE(u.live->eventually([&] { return u.live->pageText(first).contains("resumed"); }));
    placement->releaseAll();
    auto const ids = u.live->host.sessionIds();
    REQUIRE(ids.size() == 2);
    REQUIRE(u.live->eventually([&] { return u.live->pageText(ids[1]).contains("ready"); }));
}

TEST_CASE("a session no image could adopt that starts during the settle is refused, every session resumed",
          "[vthost][handover][upgrade][threading]")
{
    // The first session is a program on a PTY; the second, started as the clients go, a device that is no PTY
    // of this process, as an SSH session's is: the capture refuses it after every session was stopped once.
    auto spawned = 0;
    auto u = Upgrade { [&](vtpty::PageSize size) -> std::unique_ptr<vtpty::Pty> {
        if (spawned++ == 0)
            return catDevice(size);
        return std::make_unique<vthost::testing::StallingPty>(size);
    } };
    REQUIRE(u.live->host.createTab() != nullptr);
    auto const first = u.live->host.sessionIds().front();
    REQUIRE(u.live->eventually([&] { return u.live->pageText(first).contains("ready"); }));
    u.hooks.onClose = [&] {
        REQUIRE(u.live->host.createTab() != nullptr);
    };

    auto upgrade = u.orchestrator(Upgrade::config());
    CHECK(refusedWith(u.run(upgrade, UpgradeTarget { .imageId = u.imageId }),
                      UpgradeRefusal::UnsupportedSession));
    CHECK(u.acceptor->admission() == Admission::Accepting);
    u.live->host.writeInput(first, "resumed");
    REQUIRE(u.live->eventually([&] { return u.live->pageText(first).contains("resumed"); }));
}

TEST_CASE("a client that does not go is closed at once half way through the settle",
          "[vthost][handover][upgrade][threading]")
{
    auto u = Upgrade {};
    REQUIRE(u.live->host.createTab() != nullptr);
    REQUIRE(u.live->eventually([&] { return u.allShow("ready"); }));
    // An attached client whose orderly close never ends: its peer reads nothing.
    auto stuck = ByteCounter {};
    u.live->host.noteClientAttached(&stuck);
    u.hooks.onAbort = [&] {
        u.live->host.noteClientDetached(&stuck);
    };
    auto config = Upgrade::config();
    config.settleTimeout = 400ms;
    auto upgrade = u.orchestrator(std::move(config));

    // Not QuiesceTimedOut: the upgrade went on to the exec, which the fake fails.
    auto const result = u.run(upgrade, UpgradeTarget { .imageId = u.imageId });
    REQUIRE_FALSE(result.has_value());
    CHECK(std::holds_alternative<ExecFailure>(result.error()));
    CHECK(u.hooks.events == std::vector<std::string> { "announce 0.9.1", "snapshot", "close", "abort" });
    CHECK(u.replacer.replacements.size() == 1);
}

TEST_CASE("a daemon that shuts down while an upgrade settles resumes no listener it destroyed",
          "[vthost][handover][upgrade][threading]")
{
    auto u = Upgrade {};
    REQUIRE(u.live->host.createTab() != nullptr);
    auto const id = u.live->host.sessionIds().front();
    REQUIRE(u.live->eventually([&] { return u.allShow("ready"); }));
    // A client that does not go holds the upgrade in its settle wait, its listener held.
    auto stuck = ByteCounter {};
    u.live->host.noteClientAttached(&stuck);
    auto config = Upgrade::config();
    config.settleTimeout = 60s;
    auto upgrade = u.orchestrator(std::move(config));
    auto result = std::optional<std::expected<void, UpgradeError>> {};
    u.live->loop.spawn([](LiveUpgrade* running,
                          std::string image,
                          std::optional<std::expected<void, UpgradeError>>* into) -> core::async::Task<void> {
        *into = co_await running->run(UpgradeTarget { .imageId = std::move(image) });
    }(&upgrade, u.imageId, &result));
    REQUIRE(u.live->eventually([&] { return std::ranges::contains(u.hooks.events, "close"); }));
    REQUIRE(u.acceptor->admission() == Admission::Holding);

    // As runDaemon() ends: the listeners go, and only then does the drain unwind the upgrade, which rolls
    // back. ASan names a listener it resumed after it went.
    upgrade.forgetListeners();
    u.acceptor->close();
    std::ignore = u.live->loop.runOnce(10ms);
    u.acceptor.reset();
    {
        auto const drain = LoopDrain { u.live->loop };
    }
    CHECK_FALSE(result.has_value());
    CHECK(upgrade.state() == UpgradeState::Idle);

    // Every session was resumed all the same.
    u.live->host.noteClientDetached(&stuck);
    u.live->host.writeInput(id, "after-shutdown");
    REQUIRE(u.live->eventually([&] { return u.live->pageText(id).contains("after-shutdown"); }));
}

TEST_CASE("an exception after the clients went rolls everything back",
          "[vthost][handover][upgrade][threading]")
{
    auto u = Upgrade {};
    u.live->spawnTwo();
    auto const ids = u.live->host.sessionIds();
    REQUIRE(u.live->eventually([&] { return u.allShow("ready"); }));
    // What a large image under a memory limit does as it is written.
    u.memory.onWrite = [] {
        throw std::bad_alloc {};
    };

    auto upgrade = u.orchestrator(Upgrade::config());
    REQUIRE_THROWS_AS(std::ignore = u.run(upgrade, UpgradeTarget { .imageId = u.imageId }), std::bad_alloc);

    // Not wedged: accepting, idle, and every session answering.
    CHECK(upgrade.state() == UpgradeState::Idle);
    CHECK(u.acceptor->admission() == Admission::Accepting);
    CHECK(u.memory.kept.empty());
    CHECK(u.replacer.replacements.empty());
    for (auto const id: ids)
        u.live->host.writeInput(id, "after-throw");
    REQUIRE(u.live->eventually([&] { return u.allShow("after-throw"); }));
}

TEST_CASE("an image larger than a new image may read is refused, and rolled back",
          "[vthost][handover][upgrade][threading]")
{
    auto u = Upgrade {};
    REQUIRE(u.live->host.createTab() != nullptr);
    auto const id = u.live->host.sessionIds().front();
    REQUIRE(u.live->eventually([&] { return u.allShow("ready"); }));

    auto config = Upgrade::config();
    config.maxImageSize = 16;
    auto upgrade = u.orchestrator(std::move(config));
    CHECK(refusedWith(u.run(upgrade, UpgradeTarget { .imageId = u.imageId }), UpgradeRefusal::ImageTooLarge));
    CHECK(u.memory.objects.empty());
    CHECK(u.replacer.replacements.empty());
    CHECK(u.acceptor->admission() == Admission::Accepting);
    u.live->host.writeInput(id, "still-here");
    REQUIRE(u.live->eventually([&] { return u.live->pageText(id).contains("still-here"); }));
}

TEST_CASE("a second upgrade during an upgrade is refused with InProgress",
          "[vthost][handover][upgrade][threading]")
{
    // A session that starts as the clients go keeps the first upgrade waiting between its tries, on the loop.
    auto placement = std::make_shared<vtpty::testing::HoldingPlacement>();
    auto u = Upgrade { firstRunsThenParked(placement) };
    REQUIRE(u.live->host.createTab() != nullptr);
    REQUIRE(u.live->eventually([&] { return u.allShow("ready"); }));
    auto upgrade = u.orchestrator(Upgrade::config());
    auto second = std::optional<std::expected<void, UpgradeError>> {};
    auto asked = false;
    u.hooks.onClose = [&] {
        if (std::exchange(asked, true))
            return;
        REQUIRE(u.live->host.createTab() != nullptr);
        u.live->loop.spawn(
            [](LiveUpgrade* running,
               std::string id,
               std::optional<std::expected<void, UpgradeError>>* into) -> core::async::Task<void> {
                *into = co_await running->run(UpgradeTarget { .imageId = std::move(id) });
            }(&upgrade, u.imageId, &second));
    };
    CHECK(
        refusedWith(u.run(upgrade, UpgradeTarget { .imageId = u.imageId }), UpgradeRefusal::SessionStarting));
    REQUIRE(second.has_value());
    CHECK(refusedWith(*second, UpgradeRefusal::InProgress));
    CHECK(u.probe.asked.size() == 1); // the second asked nothing
    CHECK(upgrade.state() == UpgradeState::Idle);
    placement->releaseAll();
}

TEST_CASE("a daemon running in place upgrades to its installed executable",
          "[vthost][handover][upgrade][threading]")
{
    auto u = Upgrade {};
    REQUIRE(u.live->host.createTab() != nullptr);
    REQUIRE(u.live->eventually([&] { return u.allShow("ready"); }));
    std::ignore = u.files.writeFile("/usr/bin/contour", "ELF");
    auto packaged = DaemonImageStore {
        vthost::testing::linuxPackageStoreConfig("/state/bin", "0.9.1"), u.files, u.leases, u.links
    };
    auto config = Upgrade::config();
    config.installedExecutable = "/usr/bin/contour";
    auto upgrade = LiveUpgrade { u.live->loop, u.live->host, packaged, u.leases,      u.replacer,
                                 u.memory,     u.probe,      u.hooks,  u.listeners(), std::move(config) };
    auto image = std::optional<HandoverImage> {};
    u.replacer.onReplace = [&] {
        auto decoded = readHandover(u.memory, handoverFdOf(u.replacer.replacements.back().second));
        REQUIRE(decoded.has_value());
        image = *std::move(decoded);
    };
    CHECK_FALSE(u.run(upgrade, UpgradeTarget {}).has_value());
    CHECK(u.probe.asked == std::vector<std::filesystem::path> { "/usr/bin/contour" });
    REQUIRE(u.replacer.replacements.size() == 1);
    CHECK(u.replacer.replacements.front().first == "/usr/bin/contour");
    REQUIRE(image.has_value());
    CHECK(image->leaseFd == -1); // no image, no lease

    // An image name means nothing to a store that stages nothing.
    CHECK(refusedWith(u.run(upgrade, UpgradeTarget { .imageId = u.imageId }), UpgradeRefusal::NotStaged));
}
#endif
