// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Fake services for the switcher popup (window::SwitcherController): discovery that finds what a test says
/// or holds every caller until released, a daemon start that records its spawn, and a world whose listings a
/// test answers by hand, late or never. Shared by the controller's tests and the popup's QML tests.

#include <contour/cli/DaemonDiscovery.hpp>
#include <contour/cli/NewDaemon.hpp>
#include <contour/window/SwitcherController.hpp>

#include <vtpty/ProcessPlacement.hpp>

#include <QtCore/QObject>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <QtTest/QTest>
#include <vtcontrol/ObjectRef.hpp>
#include <vthost/DaemonBinary.hpp>

namespace contour::test
{

/// Spins the event loop until @p predicate holds or three seconds have passed.
template <typename Predicate>
[[nodiscard]] bool switcherWaitUntil(Predicate const& predicate)
{
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!predicate() && std::chrono::steady_clock::now() < deadline)
        QTest::qWait(10);
    return predicate();
}

/// @return A workspace's control id, made from @p name: equal for equal names.
[[nodiscard]] inline vtcontrol::ObjectRef switcherWorkspaceRef(std::string_view name)
{
    auto ref = vtcontrol::ObjectRef { .kind = vtcontrol::ObjectKind::Workspace, .uuid = {} };
    for (auto const [index, character]: std::views::zip(std::views::iota(std::size_t { 0 }), name))
        ref.uuid.bytes.at(index % ref.uuid.bytes.size()) ^= static_cast<uint8_t>(character);
    return ref;
}

/// @return A local daemon's address, with @p workspace.
[[nodiscard]] inline session::SwitchTarget switcherLabelled(std::string name,
                                                            session::WorkspaceTarget workspace = {})
{
    return session::SwitchTarget { .kind = vthost::proto::SwitchDaemonKind::Label,
                                   .daemon = std::move(name),
                                   .workspace = std::move(workspace) };
}

/// Discovery that finds the daemons it was given, at once.
class FixedDiscovery final: public cli::IDaemonDiscovery
{
  public:
    explicit FixedDiscovery(std::vector<cli::DiscoveredDaemon> found): _found(std::move(found)) {}

    [[nodiscard]] std::vector<cli::DiscoveredDaemon> discover() override { return _found; }

  private:
    std::vector<cli::DiscoveredDaemon> _found;
};

/// Discovery that holds every caller until released, and counts how many it holds at once: a socket that
/// takes its time to answer, so an opening can come and go while its discovery is under way. Called on the
/// controller's worker threads.
class GatedDiscovery final: public cli::IDaemonDiscovery
{
  public:
    /// @param found What each discovery answers once released.
    explicit GatedDiscovery(std::vector<cli::DiscoveredDaemon> found = { { .label = "admin",
                                                                           .socket = "/run/contour/admin",
                                                                           .workspaces = 2 } }):
        _found(std::move(found))
    {
    }

    [[nodiscard]] std::vector<cli::DiscoveredDaemon> discover() override
    {
        auto lock = std::unique_lock { _mutex };
        ++_calls;
        ++_running;
        _mostAtOnce = std::max(_mostAtOnce, _running);
        _changed.notify_all();
        _changed.wait(lock, [this] { return _released > 0; });
        --_released;
        --_running;
        return _found;
    }

    /// Lets one held discovery answer.
    void releaseOne()
    {
        auto const lock = std::lock_guard { _mutex };
        ++_released;
        _changed.notify_all();
    }

    /// @return Whether @p count discoveries were started within three seconds.
    [[nodiscard]] bool awaitCalls(int count)
    {
        auto lock = std::unique_lock { _mutex };
        return _changed.wait_for(lock, std::chrono::seconds(3), [&] { return _calls >= count; });
    }

    [[nodiscard]] int calls() const
    {
        auto const lock = std::lock_guard { _mutex };
        return _calls;
    }

    [[nodiscard]] int mostAtOnce() const
    {
        auto const lock = std::lock_guard { _mutex };
        return _mostAtOnce;
    }

  private:
    std::vector<cli::DiscoveredDaemon> _found;
    mutable std::mutex _mutex;
    std::condition_variable _changed;
    int _calls = 0;
    int _running = 0;
    int _mostAtOnce = 0;
    int _released = 0;
};

/// A daemon start ("New Daemon…") that holds its worker until released, then succeeds: a spawn that takes its
/// time, so the popup can be closed, left or relabelled while it runs.
class GatedStart
{
  public:
    /// @return The job SwitcherServices::startDaemon returns; it waits for release().
    [[nodiscard]] std::function<std::expected<cli::NamedDaemonStart, std::string>()> job()
    {
        return [this] {
            auto lock = std::unique_lock { _mutex };
            ++_started;
            _changed.notify_all();
            _changed.wait(lock, [this] { return _released; });
            return std::expected<cli::NamedDaemonStart, std::string> { cli::NamedDaemonStart::Started };
        };
    }

    /// @return Whether a job started within three seconds.
    [[nodiscard]] bool awaitStarted()
    {
        auto lock = std::unique_lock { _mutex };
        return _changed.wait_for(lock, std::chrono::seconds(3), [this] { return _started > 0; });
    }

    /// Lets every job, held or to come, answer.
    void release()
    {
        auto const lock = std::lock_guard { _mutex };
        _released = true;
        _changed.notify_all();
    }

  private:
    std::mutex _mutex;
    std::condition_variable _changed;
    int _started = 0;
    bool _released = false;
};

/// A spawner that records what it starts and has the daemon serve once started: "New Daemon…"'s real start
/// (cli::startNamedDaemon) over it. Called on the controller's worker thread.
class RecordingSpawner final: public vthost::DaemonSpawner
{
  public:
    [[nodiscard]] std::expected<void, std::string> spawn(std::span<std::string const> argv,
                                                         vtpty::ProcessPlacement& /*placement*/) override
    {
        auto const lock = std::lock_guard { _mutex };
        _spawned.emplace_back(argv.begin(), argv.end());
        return {};
    }

    [[nodiscard]] vthost::DaemonPresence probe(std::filesystem::path const& /*socketPath*/) override
    {
        auto const lock = std::lock_guard { _mutex };
        return _spawned.empty() ? vthost::DaemonPresence::Absent : vthost::DaemonPresence::Serving;
    }

    [[nodiscard]] std::vector<std::vector<std::string>> spawned() const
    {
        auto const lock = std::lock_guard { _mutex };
        return _spawned;
    }

  private:
    mutable std::mutex _mutex;
    std::vector<std::vector<std::string>> _spawned;
};

/// What the switcher asked the window switcher.
struct SwitcherRequested
{
    session::SwitchTarget target;
    session::SwitchMode mode = session::SwitchMode::Switch;
};

/// A labelled workspace the switcher asked to create.
struct SwitcherCreated
{
    session::SwitchTarget daemon;
    std::string label;
    session::SwitchMode mode = session::SwitchMode::Switch;
};

/// One of the sidebar's workspace menu rows the switcher ran.
struct SwitcherRan
{
    uint64_t workspace = 0;
    actions::Action action;
};

/// The world a switcher test runs against: what discovery finds, the saved hosts, and listings that wait for
/// the test to answer them (answer()), or answer at once from @c workspaces when @c answerAtOnce is set.
struct SwitcherWorld
{
    std::vector<cli::DiscoveredDaemon> found;                                 ///< What discovery finds.
    std::vector<window::SwitcherDaemon> hosts;                                ///< The saved hosts.
    std::map<std::string, std::vector<window::SwitcherWorkspace>> workspaces; ///< Each daemon's, by name.
    std::map<std::string, std::string> failures; ///< Each daemon whose listing fails, and why, by name.
    /// Listings answered by hand: a daemon in here waits for answer(), queued on the event loop otherwise.
    std::vector<std::string> heldBack;
    std::vector<std::string> listed;                       ///< The daemons listed, by name, in order.
    std::map<std::string, window::SwitcherListed> pending; ///< The listings held back, by name.
    std::vector<SwitcherRequested> requested;
    std::vector<SwitcherCreated> created;
    std::vector<SwitcherRan> ran;
    std::function<void()> onRan; ///< What running a sidebar menu row does besides being recorded.
    /// Each start's job instead of the real one's, when set: a test's GatedStart.
    std::function<std::function<std::expected<cli::NamedDaemonStart, std::string>()>()> startJob;
    std::vector<std::string> startedLabels;
    RecordingSpawner spawner;
    vthost::InstalledDaemonBinary binary { "contour" };
    vtpty::NoPlacement placement;
    std::chrono::system_clock::time_point now =
        std::chrono::sys_days { std::chrono::year { 2026 } / std::chrono::November / 12 };
    command::ShortcutStyle style = command::ShortcutStyle::Text;
    QObject receiver; ///< Where a listing is answered from, later, as the real one is.

    /// Answers the held-back listing of @p name from @c workspaces or @c failures.
    void answer(std::string const& name)
    {
        auto const found = pending.find(name);
        if (found == pending.end())
            return;
        auto done = std::move(found->second);
        pending.erase(found);
        done(answerFor(name));
    }

    [[nodiscard]] std::expected<std::vector<window::SwitcherWorkspace>, std::string> answerFor(
        std::string const& name) const
    {
        if (auto const failed = failures.find(name); failed != failures.end())
            return std::unexpected(failed->second);
        auto const listedHere = workspaces.find(name);
        return listedHere != workspaces.end() ? listedHere->second
                                              : std::vector<window::SwitcherWorkspace> {};
    }

    /// @return The services over this world; borrowed, it outlives the controller.
    [[nodiscard]] window::SwitcherServices services()
    {
        return window::SwitcherServices {
            .discovery = std::make_shared<FixedDiscovery>(found),
            .savedHosts = [this] { return hosts; },
            .list =
                [this](vtworkspace::WorkspaceId,
                       session::SwitchTarget const& daemon,
                       window::SwitcherListed done) {
                    listed.push_back(daemon.daemon);
                    if (std::ranges::contains(heldBack, daemon.daemon))
                    {
                        pending[daemon.daemon] = std::move(done);
                        return;
                    }
                    QMetaObject::invokeMethod(
                        &receiver,
                        [this, name = daemon.daemon, done = std::move(done)] { done(answerFor(name)); },
                        Qt::QueuedConnection);
                },
            .request = [this](vtworkspace::WorkspaceId,
                              session::SwitchTarget target,
                              session::SwitchMode mode) { requested.push_back({ std::move(target), mode }); },
            .startDaemon = [this](std::string const& label)
                -> std::function<std::expected<cli::NamedDaemonStart, std::string>()> {
                startedLabels.push_back(label);
                if (startJob)
                    return startJob();
                return [this, label] {
                    return cli::startNamedDaemon("/run/contour/" + label,
                                                 { .spawner = spawner,
                                                   .binary = binary,
                                                   .placement = placement,
                                                   .options = {},
                                                   .timeout = std::chrono::seconds { 1 } });
                };
            },
            .createWorkspace =
                [this](vtworkspace::WorkspaceId,
                       session::SwitchTarget const& daemon,
                       std::string label,
                       session::SwitchMode mode) { created.push_back({ daemon, std::move(label), mode }); },
            .now = [this] { return now; },
            .style = style,
            .workspaceMenu =
                [](vtworkspace::WorkspaceId, uint64_t) {
                    return command::WorkspaceMenuState { .position = 1, .count = 4, .joinable = {} };
                },
            .runWorkspaceAction =
                [this](vtworkspace::WorkspaceId, uint64_t workspace, actions::Action const& action) {
                    ran.push_back({ workspace, action });
                    if (onRan)
                        onRan();
                },
        };
    }
};

} // namespace contour::test
