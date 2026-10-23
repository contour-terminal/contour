// SPDX-License-Identifier: Apache-2.0
//
// What the terminal pays for status-line extensions, as structure rather than time: a stalled extension never
// makes the parser wait (A1), its code never runs on the thread that feeds the terminal (A1), and a pane
// without extensions starts nothing for them (A2). None of these cases measures the machine; how fast the
// terminal runs with and without extensions is measured apart (`bench-headless throughput extensions ...`).
#include <vtbackend/screen/StatusLineExtensions.hpp>
#include <vtbackend/screen/Terminal.hpp>
#include <vtbackend/testing/MockTerm.hpp>

#include <core/async/testing/ManualExecutor.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <vtextension/Event.hpp>
#include <vtextension/ExtensionHost.hpp>
#include <vtextension/ExtensionLimits.hpp>
#include <vtextension/ExtensionWorkerPool.hpp>
#include <vtextension/SessionExtensions.hpp>
#include <vtextension/StatusLineAdapter.hpp>
#include <vtextension/testing/QueuedOwner.hpp>
#include <vtextension/testing/RunnerHarness.hpp>
#include <vtextension/testing/TestModules.hpp>

using namespace vtextension;
using vtextension::testing::CountingEngineProvider;
using vtextension::testing::QueuedOwner;
using vtextension::testing::RunnerHarness;
using vtextension::testing::toModule;

namespace
{

constexpr auto PaneSize =
    vtbackend::PageSize { .lines = vtbackend::LineCount(10), .columns = vtbackend::ColumnCount(40) };

/// How long a case waits for something another thread must do before it calls it hung. A liveness bound,
/// never a measurement: what it waits for takes milliseconds, and only a deadlock gets anywhere near it.
constexpr auto HangGuard = std::chrono::minutes { 2 };

/// Waits, without a time budget of its own, until @p condition holds or HangGuard has passed.
/// @return Whether @p condition held.
[[nodiscard]] bool eventually(std::function<bool()> const& condition)
{
    auto const deadline = std::chrono::steady_clock::now() + HangGuard;
    while (!condition())
    {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        std::this_thread::yield();
    }
    return true;
}

/// @return One extension, FollowerModule, which follows every event kind and logs from every render.
[[nodiscard]] ExtensionHostConfig follower()
{
    auto config = ExtensionHostConfig {};
    config.extensions.push_back(ExtensionSpec { .name = "follower",
                                                .hostId = "follower@test",
                                                .module = toModule(vtextension::testing::FollowerModule),
                                                .config = {},
                                                .placement = Placement::Segment,
                                                .runCommand = CommandPermission::Deny });
    return config;
}

/// @param i Which hook call.
/// @return What a program writes to make the terminal call its @p i th extension hook: a title, a working
///         directory, or a user variable of a name never used before. Titles and directories coalesce in the
///         extension's queue; user variables of new names do not, so a stalled extension's queue fills.
[[nodiscard]] std::string hookSequence(int i)
{
    switch (i % 3)
    {
        case 0: return std::format("\033]2;title {}\033\\", i);
        case 1: return std::format("\033]7;file://localhost/tmp/{}\033\\", i);
        default: return std::format("\033]1337;SetUserVar=var{}=eA==\033\\", i);
    }
}

/// A lane's worker held inside a job until released, as an extension that never returns would hold it.
class ParkedWorker
{
  public:
    /// Parks the worker that runs @p lane of @p pool, and returns once it is parked.
    ParkedWorker(ExtensionWorkerPool& pool, LaneId lane)
    {
        auto parked = std::promise<void> {};
        auto isParked = parked.get_future();
        pool.post(lane, [&parked, released = _released.get_future().share()] {
            parked.set_value();
            released.wait();
        });
        REQUIRE(isParked.wait_for(HangGuard) == std::future_status::ready);
    }

    ParkedWorker(ParkedWorker const&) = delete;
    ParkedWorker(ParkedWorker&&) = delete;
    ParkedWorker& operator=(ParkedWorker const&) = delete;
    ParkedWorker& operator=(ParkedWorker&&) = delete;

    /// Releases the worker, if not yet released.
    ~ParkedWorker() { release(); }

    /// Lets the worker go on.
    void release()
    {
        if (!_isReleased)
            _released.set_value();
        _isReleased = true;
    }

  private:
    std::promise<void> _released;
    bool _isReleased = false;
};

/// An IExtensionScheduler over another that counts what it is asked for, from any thread.
class CountingScheduler final: public IExtensionScheduler
{
  public:
    explicit CountingScheduler(IExtensionScheduler& inner) noexcept: _inner { inner } {}

    [[nodiscard]] LaneId assignLane() override
    {
        ++lanes;
        return _inner.assignLane();
    }

    void post(LaneId lane, std::function<void()> job) override
    {
        ++posts;
        _inner.post(lane, std::move(job));
    }

    [[nodiscard]] TimerId schedule(LaneId lane,
                                   std::chrono::milliseconds after,
                                   std::function<void()> job) override
    {
        ++timers;
        return _inner.schedule(lane, after, std::move(job));
    }

    void cancel(TimerId timer) noexcept override { _inner.cancel(timer); }

    [[nodiscard]] core::platform::IClock& clock() noexcept override { return _inner.clock(); }

    std::atomic<std::size_t> lanes = 0;  ///< assignLane() calls.
    std::atomic<std::size_t> posts = 0;  ///< post() calls.
    std::atomic<std::size_t> timers = 0; ///< schedule() calls.

  private:
    IExtensionScheduler& _inner;
};

/// Thread ids, from any thread.
class ThreadIds
{
  public:
    void record()
    {
        auto const lock = std::scoped_lock { _mutex };
        _ids.push_back(std::this_thread::get_id());
    }

    [[nodiscard]] std::vector<std::thread::id> ids() const
    {
        auto const lock = std::scoped_lock { _mutex };
        return _ids;
    }

  private:
    mutable std::mutex _mutex;
    std::vector<std::thread::id> _ids;
};

} // namespace

TEST_CASE("Isolation.hookNeverBlocks", "[vtextension][isolation][threading]")
{
    // An extension whose worker never comes back: the terminal's hooks still return, every one, and what its
    // queue cannot hold is dropped and counted rather than waited for. What this proves is that a hook never
    // waits for the parked consumer; that it never waits at all rests on the code, not on a clock: the
    // producer's path (Runner::Core::post, EventQueue::tryPush) takes no lock and no wait primitive.
    constexpr auto HookCalls = 100'000;

    auto harness = RunnerHarness {};
    auto engines = EngineProvider { *harness.backend, ExtensionLimits {} };
    auto pool = ExtensionWorkerPool(*harness.backend, 1); // one worker: parking it parks every lane
    auto sink = vtbackend::StatusLinePublishSink {};
    auto services = harness.services(pool, engines);
    services.onSnapshotPublished = publisherFor(sink);
    services.onLog = [](std::string_view, LogLevel, std::string_view) {
    };
    auto host = ExtensionHost(follower(), ExtensionHostServices { .engines = engines, .runners = services });
    auto adapter = StatusLineAdapter { host };
    {
        auto mock = vtbackend::MockTerm {
            PaneSize,
            vtbackend::StatusLineExtensionServices {
                .extensions = adapter, .published = sink, .maxUserVars = ExtensionLimits {}.maxUserVars }
        };
        REQUIRE(eventually([&] { return host.subscriptions().test(EventKind::UserVar); }));
        auto worker = ParkedWorker { pool, pool.assignLane() };
        {
            auto const _ = std::scoped_lock { mock.terminal };
            mock.terminal.statusLineExtensionsReplaced(); // the gate reads what the extension follows
        }

        auto finished = std::promise<void> {};
        auto hasFinished = finished.get_future();
        auto parser = std::thread { [&] {
            for (auto const i: std::views::iota(0, HookCalls))
                mock.writeToScreen(hookSequence(i));
            finished.set_value();
        } };
        auto const status = hasFinished.wait_for(HangGuard);
        auto const dropped = host.dropCount();
        worker.release(); // also lets a parser that did block go on, so that the join below returns
        parser.join();

        CHECK(status == std::future_status::ready);
        CHECK(dropped > 0);
        CHECK(dropped < static_cast<uint64_t>(HookCalls));
    }
}

TEST_CASE("Isolation.noEngineWhenUnused", "[vtextension][isolation]")
{
    // A pane as the GUI and the daemon build one: its terminal holds the pane's (empty) extensions for life,
    // and is told everything a session tells it. With no extension declared, nothing is built, no engine is
    // created, no lane assigned and no worker thread started -- even with every service at hand.
    auto harness = RunnerHarness {};
    auto counting = CountingEngineProvider { *harness.backend };
    auto pool = ExtensionWorkerPool(*harness.backend, 2);
    auto scheduler = CountingScheduler { pool };
    auto builds = core::async::testing::ManualExecutor {};
    auto owner = std::make_shared<QueuedOwner>();
    {
        auto extensions = SessionExtensions { SessionExtensionCallbacks {}, owner };
        auto mock = vtbackend::MockTerm { PaneSize, extensions.terminalServices() };
        extensions.rebuild(DeclaredExtensions {},
                           vtbackend::ImageSize { vtbackend::Width(8), vtbackend::Height(16) },
                           SessionExtensionServices {
                               .engines = counting,
                               .retirement = counting,
                               .scheduler = scheduler,
                               .builds = builds,
                               .grants = harness.grants,
                               .capture = harness.capture,
                               .wallClock = harness.wallClock,
                               .localTime = harness.localTime,
                               .environment = harness.environment,
                               .fileSystem = harness.fileSystem,
                               .lookup = { .fileSystem = harness.fileSystem, .bundledDirectories = {} },
                               .limits = {} });
        // Nothing handed back to the owner either: checked before the owner's queue is run.
        CHECK(owner->pending() == 0);
        owner->run();

        // Every hook a session drives.
        for (auto const i: std::views::iota(0, 30))
            mock.writeToScreen(hookSequence(i));
        mock.writeToScreen("\033]9;4;1;50\033\\"); // progress
        mock.writeToScreen("\033[2$~");            // the indicator status line
        mock.terminal.sendFocusInEvent();
        mock.terminal.sendFocusOutEvent();
        {
            auto const _ = std::scoped_lock { mock.terminal };
            mock.terminal.foregroundProcessChanged("vim");
            mock.terminal.setStatusLineFontMetrics(vtbackend::StatusLineFontMetrics {
                .pointSize = 12, .cellPixelSize = { vtbackend::Width(10), vtbackend::Height(22) } });
            mock.terminal.statusLineExtensionsReplaced();
        }
        mock.terminal.resizeScreen(
            vtbackend::PageSize { .lines = vtbackend::LineCount(20), .columns = vtbackend::ColumnCount(60) });
        mock.terminal.refreshRenderBuffer();

        CHECK(extensions.empty());
        CHECK(extensions.subscriptions().none());
    }
    CHECK(counting.acquired == 0);
    CHECK(builds.pending() == 0);
    CHECK(owner->pending() == 0);
    CHECK(pool.runningWorkers() == 0);
    CHECK(scheduler.lanes == 0);
    CHECK(scheduler.posts == 0);
    CHECK(scheduler.timers == 0);
}

TEST_CASE("Isolation.renderNotOnParserThread", "[vtextension][isolation][threading]")
{
    // The extension's instance runs on its lane: every render it does -- recorded from inside the instance,
    // by the host's `log` call -- and every frame published happen off the thread that parses the output
    // which caused them, and off the thread that set the pane up.
    auto harness = RunnerHarness {};
    auto engines = EngineProvider { *harness.backend, ExtensionLimits {} };
    auto pool = ExtensionWorkerPool(*harness.backend, 1);
    auto sink = vtbackend::StatusLinePublishSink {};
    auto renders = ThreadIds {};
    auto publishes = ThreadIds {};
    auto services = harness.services(pool, engines);
    services.onSnapshotPublished = [&, publish = publisherFor(sink)] {
        publishes.record();
        publish();
    };
    services.onLog = [&](std::string_view, LogLevel, std::string_view) {
        renders.record();
    };
    auto parserId = std::thread::id {};
    {
        auto host =
            ExtensionHost(follower(), ExtensionHostServices { .engines = engines, .runners = services });
        auto adapter = StatusLineAdapter { host };
        auto mock = vtbackend::MockTerm {
            PaneSize,
            vtbackend::StatusLineExtensionServices {
                .extensions = adapter, .published = sink, .maxUserVars = ExtensionLimits {}.maxUserVars }
        };
        REQUIRE(eventually([&] { return host.subscriptions().test(EventKind::Title); }));
        {
            auto const _ = std::scoped_lock { mock.terminal };
            mock.terminal.statusLineExtensionsReplaced();
        }
        auto const rendersBefore = renders.ids().size();

        auto parser = std::thread { [&] {
            for (auto const i: std::views::iota(0, 300))
                mock.writeToScreen(hookSequence(i));
        } };
        parserId = parser.get_id();
        parser.join();
        // Each event asks for a render; renders are throttled, so fewer happen, but at least one after them.
        CHECK(eventually([&] { return renders.ids().size() > rendersBefore; }));
    }
    pool.stop();

    auto const isOffTheirThreads = [&](std::thread::id id) {
        return id != parserId && id != std::this_thread::get_id();
    };
    REQUIRE_FALSE(renders.ids().empty());
    REQUIRE_FALSE(publishes.ids().empty());
    CHECK(std::ranges::all_of(renders.ids(), isOffTheirThreads));
    CHECK(std::ranges::all_of(publishes.ids(), isOffTheirThreads));
}
