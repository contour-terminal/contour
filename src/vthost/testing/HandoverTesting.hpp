// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What the live-upgrade tests share: a host with pumping sessions on a loop of its own, children that echo
/// what they are sent, and the in-process stand-in for the exec that lets a second host adopt the first one's
/// PTYs and children.

#include <vtpty/Process.hpp>
#include <vtpty/ProcessPlacement.hpp>
#include <vtpty/Pty.hpp>
#include <vtpty/test/ProcessTesting.hpp>

#ifdef _WIN32
    #include <vtpty/ConPty.hpp>
#else
    #include <vtpty/UnixPty.hpp>
#endif

#include <core/Environment.hpp>
#include <core/net/PlatformLoop.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <vtcontrol/testing/SequentialUuidSource.hpp>
#include <vthost/LoopDrain.hpp>
#include <vthost/SessionHost.hpp>
#include <vthost/TappingPty.hpp>
#include <vthost/handover/HandoverCapture.hpp>
#include <vthost/handover/HandoverCodec.hpp>

namespace vthost::testing
{

#ifndef _WIN32
/// A shell that tells it is ready, then echoes what it is sent onto the screen, unchanged.
inline constexpr auto CatScript = "stty raw -echo; printf ready; exec cat";

/// @return A child running @p script on a PTY of its own.
/// @param size The PTY's size.
/// @param script What `/bin/sh -c` runs.
/// @param placement Where the child is placed.
[[nodiscard]] inline std::unique_ptr<vtpty::Pty> shellProcess(
    vtpty::PageSize size,
    std::string script = CatScript,
    std::shared_ptr<vtpty::ProcessPlacement> placement = std::make_shared<vtpty::NoPlacement>())
{
    return std::make_unique<vtpty::Process>(vtpty::testing::execInfo("/bin/sh", { "-c", std::move(script) }),
                                            vtpty::createPty(size, std::nullopt),
                                            /*escapeSandbox=*/false,
                                            std::move(placement));
}

#else
/// A shell that tells it is ready, then shows what it is sent on the screen: the console echoes each line
/// typed to a command interpreter that reads it, behind a prompt of one character, so that what is typed does
/// not wrap where a test looks for it.
/// @param size The pseudoconsole's size.
[[nodiscard]] inline std::unique_ptr<vtpty::Pty> shellProcess(vtpty::PageSize size)
{
    auto exec = vtpty::testing::execInfo("cmd.exe", { "/q", "/k", "echo ready" });
    exec.env = { { "PROMPT", "$G" } };
    return std::make_unique<vtpty::Process>(exec,
                                            vtpty::createPty(size, std::nullopt),
                                            /*escapeSandbox=*/false,
                                            std::make_shared<vtpty::NoPlacement>());
}

/// Skips the test where the bundled conpty.dll is not beside the test binary (CONTOUR_EMBED_OPENCONSOLE=OFF):
/// only its pseudoconsoles can be handed over, and every capture would be refused. Called on the test's own
/// thread, before any session is spawned.
inline void requireBundledConsoleHost()
{
    if (vtpty::ConPty { vtpty::PageSize { vtpty::LineCount(24), vtpty::ColumnCount(80) } }.consoleHost()
        != vtpty::ConsoleHost::Bundled)
        SKIP("the bundled conpty.dll is not beside this test");
}
#endif

/// Lets go of every session's PTY and child without closing or signalling either, as the handoff does -- the
/// exec on POSIX, the old daemon's end once its successor took over on Windows: the in-process stand-in for
/// it, after which destroying @p host closes nothing and ends no program, and a second host adopts them from
/// the image a capture took, under the very descriptors or handles the new release would inherit them as.
/// @param host A host every session of which runs on a PTY of its own, a child process on it or none.
inline void releaseForAdoption(SessionHost& host)
{
    for (auto const id: host.sessionIds())
    {
        auto& device = host.hostedSession(id)->terminal().device();
        auto* const tapped = dynamic_cast<TappingPty*>(&device);
        auto& inner = tapped != nullptr ? tapped->inner() : device;
        auto* const process = dynamic_cast<vtpty::Process*>(&inner);
        // The child first: on Windows its exit watcher closes the pseudoconsole, and stops with it.
        if (process != nullptr)
            std::ignore = process->releaseChild();
        auto& pty = process != nullptr ? process->pty() : inner;
#ifdef _WIN32
        std::ignore = dynamic_cast<vtpty::ConPty&>(pty).releaseHandles();
#else
        std::ignore = dynamic_cast<vtpty::UnixPty&>(pty).releaseDescriptors();
#endif
    }
}

/// A host with pumping sessions, on a loop of its own, as a daemon runs it.
struct LiveHost
{
    /// Makes the device of each session the host spawns, at the size asked for.
    using Devices = std::function<std::unique_ptr<vtpty::Pty>(vtpty::PageSize)>;

    /// @param devices Makes the device of each session, in spawn order.
    /// @param config How the host is set up; its sessions always pump.
    /// @param settings The sessions' terminal settings.
    explicit LiveHost(Devices devices,
                      SessionHostConfig config = sequentialIds(),
                      vtbackend::Settings settings = {}):
        host { loop,
               [devices = std::move(devices)](vtpty::PageSize size,
                                              std::optional<vtpty::Process::ExecInfo> const&) {
                   return devices(size);
               },
               std::move(settings),
               core::defaultEnvironment(),
               pumping(std::move(config)) }
    {
    }

    core::net::PlatformLoop loop;
    SessionHost host;
    LoopDrain drain { loop }; ///< Declared after the host: destroyed first, as LoopDrain asks.

    /// @return A configuration naming objects with sequential UUIDs, as the tests name them.
    [[nodiscard]] static SessionHostConfig sequentialIds()
    {
        return SessionHostConfig { .uuids = std::make_shared<vtcontrol::testing::SequentialUuidSource>() };
    }

    /// Spawns two sessions side by side in one tab.
    void spawnTwo()
    {
        auto* const tab = host.createTab();
        REQUIRE(tab != nullptr);
        host.splitActivePane(tab->id(), vtworkspace::SplitState::Vertical);
        REQUIRE(host.sessionCount() == 2);
    }

    /// @return The main page of @p session's screen, as text.
    [[nodiscard]] std::string pageText(vtworkspace::SessionId session)
    {
        auto& terminal = host.hostedSession(session)->terminal();
        auto const _ = std::lock_guard { terminal };
        return terminal.primaryScreen().renderMainPageText();
    }

    /// Turns the loop until @p condition holds, for at most ten seconds.
    /// @return Whether it held.
    [[nodiscard]] bool eventually(std::function<bool()> const& condition)
    {
        using namespace std::chrono_literals;
        auto const deadline = std::chrono::steady_clock::now() + 10s;
        while (!condition())
        {
            if (std::chrono::steady_clock::now() >= deadline)
                return false;
            std::ignore = loop.runOnce(10ms);
        }
        return true;
    }

    /// What the orchestrator does ahead of the capture: stops every pump, and turns the loop until it has
    /// handled what they posted.
    void settle()
    {
        REQUIRE(eventually([this] { return host.stopAllPumps().has_value(); }));
        REQUIRE(eventually([this] { return host.pumpEventsInFlight() == 0; }));
    }

    /// @param listeners The listening sockets the image names.
    /// @param leaseFd The lease descriptor the image names.
    /// @param instance The daemon instance the image names.
    /// @return The capture.
    [[nodiscard]] std::expected<handover::HandoverImage, handover::HandoverError> capture(
        std::vector<handover::InheritedListener> const& listeners = {},
        int leaseFd = -1,
        uint64_t instance = 0xC0FFEE)
    {
        return handover::captureHandover(host, listeners, leaseFd, instance, "0.7.1");
    }

  private:
    [[nodiscard]] static SessionHostConfig pumping(SessionHostConfig config)
    {
        config.pumping = SessionPumping::Threaded;
        return config;
    }
};

} // namespace vthost::testing
