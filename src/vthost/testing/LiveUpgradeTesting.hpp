// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What the live-upgrade tests of every platform share: a daemon about to be upgraded, with fakes for
/// everything that would leave the process but the handoff, which each platform's tests bring their own of
/// (ExecHandoff over a fake exec on POSIX, SuccessorHandoff over a fake launcher on Windows).

#include <core/async/Task.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/Sockets.hpp>
#include <core/testing/ScopedTempDir.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <vthost/ConnectionAcceptor.hpp>
#include <vthost/DaemonImage.hpp>
#include <vthost/handover/HandoverToken.hpp>
#include <vthost/handover/ImageHandoff.hpp>
#include <vthost/handover/LiveUpgrade.hpp>
#include <vthost/handover/Readiness.hpp>
#include <vthost/testing/FakeImageLeases.hpp>
#include <vthost/testing/FakeReadinessProbe.hpp>
#include <vthost/testing/HandoverTesting.hpp>
#include <vthost/testing/StagingFileSystem.hpp>
#include <vthost/testing/TestHelpers.hpp>

namespace vthost::testing
{

/// What a target binary built from this tree answers, one that can adopt pseudoconsoles.
[[nodiscard]] inline handover::ReadinessReport readyTarget()
{
    return handover::ownReadiness("0.9.1", handover::PseudoconsoleAdoption::Able);
}

/// UpgradeHooks that record what they were asked, in order.
class RecordingHooks final: public handover::UpgradeHooks
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

    [[nodiscard]] std::size_t endingConnections() const override
    {
        return ending ? ending() : std::size_t { 0 };
    }

    std::vector<std::string> events;
    std::function<std::size_t()> ending; ///< How many connections are still ending; none when unset.
    std::function<void()> onClose;       ///< Runs as the clients are closed, while the sessions still run.
    std::function<void()> onAbort;       ///< Runs as the clients are closed at once.
};

/// A connection handler that lets every connection go at once.
[[nodiscard]] inline core::async::Task<void> ignoreConnection(
    ConnectionId /*id*/, std::unique_ptr<core::net::ISocket> /*connection*/)
{
    co_return;
}

/// @return Whether @p result is the refusal @p refusal.
[[nodiscard]] inline bool refusedWith(std::expected<void, handover::UpgradeError> const& result,
                                      handover::UpgradeRefusal refusal)
{
    return !result.has_value() && std::holds_alternative<handover::UpgradeRefusal>(result.error())
           && std::get<handover::UpgradeRefusal>(result.error()) == refusal;
}

/// A daemon about to be upgraded, but for the handoff: a live host, a listener, a store with the target
/// staged, and fakes for everything else that would leave the process.
struct UpgradeBench
{
    /// @param devices The device of each session the host spawns.
    /// @param settings The sessions' terminal settings.
    explicit UpgradeBench(LiveHost::Devices devices, vtbackend::Settings settings = {}):
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

    UpgradeBench(UpgradeBench const&) = delete;
    UpgradeBench& operator=(UpgradeBench const&) = delete;
    UpgradeBench(UpgradeBench&&) = delete;
    UpgradeBench& operator=(UpgradeBench&&) = delete;

    ~UpgradeBench()
    {
        using namespace std::chrono_literals;
        if (!acceptor || !live)
            return;
        // Its accept loop ends before it goes.
        acceptor->close();
        std::ignore = live->loop.runOnce(10ms);
        std::ignore = live->loop.runOnce(10ms);
    }

    /// @return The orchestrator, over this daemon, handing over with @p handoff.
    [[nodiscard]] handover::LiveUpgrade orchestrator(handover::LiveUpgradeConfig config,
                                                     handover::ImageHandoff& handoff)
    {
        return handover::LiveUpgrade { live->loop, live->host, store,       leases,           handoff,
                                       probe,      hooks,      listeners(), std::move(config) };
    }

    /// @return The orchestrator, over this daemon, with the configuration config() and handing over with @p
    /// handoff.
    [[nodiscard]] handover::LiveUpgrade orchestrator(handover::ImageHandoff& handoff)
    {
        return orchestrator(config(), handoff);
    }

    /// Checks that the daemon serves on after a handoff that did not happen: it accepts again, every
    /// session answers, and @p carried -- typed into the first while the upgrade had everything stopped, if
    /// anything -- is delivered once, ahead of what is typed afterwards.
    /// @param ids The daemon's two sessions.
    /// @param carried What was typed into the first session during the upgrade.
    void checkResumed(std::vector<vtworkspace::SessionId> const& ids, std::string_view carried = {}) const
    {
        REQUIRE(ids.size() >= 2);
        CHECK(acceptor->admission() == Admission::Accepting);
        live->host.writeInput(ids[0], "-after");
        live->host.writeInput(ids[1], "second");
        auto const expected = std::string { carried } + "-after";
        auto const shown = live->eventually([&] { return live->pageText(ids[0]).contains(expected); });
        INFO("first session's screen: " << live->pageText(ids[0]));
        REQUIRE(shown);
        REQUIRE(live->eventually([&] { return live->pageText(ids[1]).contains("second"); }));
        if (!carried.empty())
            CHECK(occurrences(live->pageText(ids[0]), std::string { carried }) == 1);
    }

    /// @return The configuration a daemon of release 0.9.0 upgrades with.
    [[nodiscard]] static handover::LiveUpgradeConfig config()
    {
        using namespace std::chrono_literals;
        return handover::LiveUpgradeConfig {
            .release = "0.9.0",
            .daemonInstance = 0xC0FFEE,
            .upgradeGeneration = 2,
            .argv = { "/state/bin/0.9.0-00000000/Contour.AppImage",
                      "daemon",
                      handover::handoverArgument(handover::handoverToken(vtpty::CrossingHandle { 7 })),
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
    [[nodiscard]] std::expected<void, handover::UpgradeError> run(handover::LiveUpgrade& upgrade,
                                                                  handover::UpgradeTarget to) const
    {
        return live->loop.blockOn(upgrade.run(std::move(to)));
    }

    /// @return Whether every session's screen shows @p text.
    [[nodiscard]] bool allShow(std::string_view text) const
    {
        return std::ranges::all_of(live->host.sessionIds(), [&](vtworkspace::SessionId id) {
            return live->pageText(id).contains(text);
        });
    }

    /// @return The acceptor, as the orchestrator takes its listeners.
    [[nodiscard]] std::span<ConnectionAcceptor* const> listeners() { return acceptors; }

    StagingFileSystem files;
    FakeImageLeases leases;
    FakeSymbolicLinks links;
    DaemonImageStore store { appImageStoreConfig("/state/bin", "0.9.1"), files, leases, links };
    FakeReadinessProbe probe;
    RecordingHooks hooks;
    std::filesystem::path target;
    std::string imageId;
    core::testing::ScopedTempDir socketDir { "contour-upgrade" };
    std::string socketPath = (socketDir.path() / "control").string();
    std::unique_ptr<LiveHost> live;
    std::unique_ptr<ConnectionAcceptor> acceptor;

    std::vector<ConnectionAcceptor*> acceptors;
};

} // namespace vthost::testing
