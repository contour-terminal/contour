// SPDX-License-Identifier: Apache-2.0
//
// The extension runner: events in, frames out, frame pacing, fault recovery and disabling, replies,
// commands, and the threads it may be driven from.
#include <vtbackend/screen/StatusLineCommandCache.hpp>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <expected>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <vtcontrol/ControlError.hpp>
#include <vtextension/CommandLine.hpp>
#include <vtextension/CompiledExtension.hpp>
#include <vtextension/Engine.hpp>
#include <vtextension/ExtensionWorkerPool.hpp>
#include <vtextension/Runner.hpp>
#include <vtextension/testing/RunnerHarness.hpp>
#include <vtextension/testing/TestModules.hpp>

using namespace std::chrono_literals;
using namespace vtextension;
using vtextension::testing::loadModule;
using vtextension::testing::RunnerHarness;
using vtextension::testing::toModule;

namespace
{

/// @return What a test runner called @p name is.
[[nodiscard]] RunnerConfig configFor(std::string name,
                                     ConfigMap config = {},
                                     ExtensionLimits limits = {},
                                     CommandPermission runCommand = CommandPermission::Deny)
{
    return RunnerConfig { .name = std::move(name),
                          .hostId = "test@/extensions/test.wasm",
                          .config = std::move(config),
                          .placement = Placement::Segment,
                          .runCommand = runCommand,
                          .cellPixelSize = {},
                          .limits = limits };
}

/// @return A runner of @p bytes on @p harness, called @p name.
[[nodiscard]] std::unique_ptr<Runner> makeRunner(RunnerHarness& harness,
                                                 std::vector<std::byte> bytes,
                                                 std::string name,
                                                 ConfigMap config = {},
                                                 ExtensionLimits limits = {},
                                                 CommandPermission runCommand = CommandPermission::Deny)
{
    auto module = loadModule(*harness.backend, std::move(bytes), limits);
    return std::make_unique<Runner>(std::move(module.engine),
                                    std::move(module.compiled),
                                    configFor(std::move(name), std::move(config), limits, runCommand),
                                    harness.services());
}

[[nodiscard]] Resized surfaceOf(uint32_t cols, uint32_t rows)
{
    return Resized { .size = SurfaceSize { .cols = cols, .rows = rows } };
}

/// @return The generation of the runner's latest frame; 0 before the first.
[[nodiscard]] uint64_t generationOf(Runner const& runner)
{
    auto const latest = runner.latest();
    return latest ? latest->generation : 0;
}

} // namespace

TEST_CASE("Runner.publishes", "[vtextension][runner]")
{
    auto harness = RunnerHarness {};
    auto const runner = makeRunner(harness, toModule(vtextension::testing::MinimalModule), "minimal");
    CHECK(runner->state() == RunnerState::Starting);
    CHECK(runner->latest() == nullptr);

    runner->post(surfaceOf(10, 1));
    harness.scheduler.runPending();

    CHECK(runner->state() == RunnerState::Running);
    CHECK(runner->subscriptions() == AlwaysDelivered);
    REQUIRE(runner->latest() != nullptr);
    CHECK(runner->latest()->generation == 1);
    CHECK(runner->latest()->sgrText.empty());
    CHECK(harness.published >= 1);
}

TEST_CASE("Runner.zeroColumns", "[vtextension][runner]")
{
    // A surface with nothing to show is never rendered (Review Focus 2) ...
    auto harness = RunnerHarness {};
    auto const runner = makeRunner(harness, toModule(vtextension::testing::MinimalModule), "minimal");
    runner->resize(
        vtbackend::PageSize { .lines = vtbackend::LineCount(1), .columns = vtbackend::ColumnCount(0) });
    harness.scheduler.runPending();
    harness.scheduler.advance(1s);
    CHECK(runner->state() == RunnerState::Running);
    CHECK(runner->latest() == nullptr);

    // ... until it has.
    runner->resize(
        vtbackend::PageSize { .lines = vtbackend::LineCount(1), .columns = vtbackend::ColumnCount(10) });
    harness.scheduler.runPending();
    CHECK(generationOf(*runner) == 1);
}

TEST_CASE("Runner.renderThrottle", "[vtextension][runner]")
{
    // Renders are at least minFrameInterval apart: a resize right after a render waits for it.
    auto harness = RunnerHarness {};
    auto const limits = ExtensionLimits {};
    auto const runner =
        makeRunner(harness, toModule(vtextension::testing::MinimalModule), "minimal", {}, limits);
    runner->post(surfaceOf(10, 1));
    harness.scheduler.runPending();
    REQUIRE(generationOf(*runner) == 1);

    runner->post(surfaceOf(20, 1));
    harness.scheduler.runPending();
    CHECK(generationOf(*runner) == 1);
    harness.scheduler.advance(limits.minFrameInterval - 1ms);
    CHECK(generationOf(*runner) == 1);
    harness.scheduler.advance(1ms);
    CHECK(generationOf(*runner) == 2);
}

TEST_CASE("Runner.backoffThenDisable", "[vtextension][runner]")
{
    // The malicious guest's render traps on a one-row surface: every instance faults on its first frame.
    auto harness = RunnerHarness {};
    auto const limits = ExtensionLimits {};
    REQUIRE(limits.faultsBeforeDisable == 3);
    auto const runner =
        makeRunner(harness, toModule(vtextension::testing::MaliciousModule), "malicious", {}, limits);
    runner->post(surfaceOf(80, 1));
    harness.scheduler.runPending();
    CHECK(runner->state() == RunnerState::BackingOff);
    CHECK(runner->latest() == nullptr);

    // The first back-off is initialBackoff ...
    harness.scheduler.advance(limits.initialBackoff - 1ms);
    CHECK(runner->state() == RunnerState::BackingOff);
    harness.scheduler.advance(1ms);
    CHECK(runner->state() == RunnerState::BackingOff); // re-instantiated, and faulted again

    // ... the second twice that, and the third fault disables.
    harness.scheduler.advance((2 * limits.initialBackoff) - 1ms);
    CHECK(runner->state() == RunnerState::BackingOff);
    harness.scheduler.advance(1ms);
    CHECK(runner->state() == RunnerState::Disabled);

    REQUIRE(runner->latest() != nullptr);
    CHECK(runner->latest()->sgrText == "⚠ malicious disabled: extension trapped");
    CHECK(runner->subscriptions().none());
    CHECK(harness.scheduler.pendingTimers() == 0);

    // A disabled extension stays disabled.
    runner->post(surfaceOf(81, 1));
    harness.scheduler.advance(limits.faultWindow);
    CHECK(runner->state() == RunnerState::Disabled);
}

TEST_CASE("Runner.faultWindow", "[vtextension][runner]")
{
    // Faults further apart than faultWindow never add up to a disable.
    auto harness = RunnerHarness {};
    auto limits = ExtensionLimits {};
    limits.faultWindow = 1s;
    limits.initialBackoff = 2s;
    auto const runner =
        makeRunner(harness, toModule(vtextension::testing::MaliciousModule), "malicious", {}, limits);
    runner->post(surfaceOf(80, 1));
    harness.scheduler.runPending();
    REQUIRE(runner->state() == RunnerState::BackingOff);
    for ([[maybe_unused]] auto const round: std::views::iota(0, 5))
    {
        // Every (re)start that loads tells the host its subscriptions: that is how a restart shows.
        auto const published = harness.published;
        // Each restart faults again, 2 s after the previous fault: the 1 s window holds only one, so
        // the back-off never grows either.
        harness.scheduler.advance(limits.initialBackoff);
        CHECK(runner->state() == RunnerState::BackingOff);
        CHECK(harness.published == published + 1);
    }
}

TEST_CASE("Runner.destroyedWithWorkPending", "[vtextension][runner]")
{
    // Destroying a runner never waits for its lane, and no callback reaches the host afterwards.
    auto harness = RunnerHarness {};
    {
        auto const runner = makeRunner(harness, toModule(vtextension::testing::MinimalModule), "minimal");
        runner->post(surfaceOf(10, 1));
    }
    harness.scheduler.runPending();
    harness.scheduler.advance(1s);
    CHECK(harness.published == 0);
    CHECK(harness.scheduler.pendingTimers() == 0);
}

TEST_CASE("Runner.logBudget", "[vtextension][runner]")
{
    // The guest's cabi_realloc logs on every allocation, and loading 40 settings allocates over 80 times
    // in one call: only maxLogsPerCall of those messages reach the host.
    auto harness = RunnerHarness {};
    auto const limits = ExtensionLimits {};
    auto config = ConfigMap {};
    for (auto const index: std::views::iota(0, 40))
        config.emplace_back("key" + std::to_string(index), "value");
    auto const runner = makeRunner(
        harness, toModule(vtextension::testing::ReentrantReallocModule), "logger", std::move(config), limits);
    harness.scheduler.runPending();
    CHECK(runner->state() == RunnerState::Running);
    CHECK(harness.logged.size() == limits.maxLogsPerCall);
}

TEST_CASE("Runner.retiresOnTeardown", "[vtextension][runner]")
{
    // The runner's lane hands its engine and module back instead of releasing them.
    auto harness = RunnerHarness {};
    auto weakEngine = std::weak_ptr<Engine> {};
    {
        auto module = loadModule(*harness.backend, toModule(vtextension::testing::MinimalModule));
        weakEngine = module.engine;
        auto const runner = Runner(
            std::move(module.engine), std::move(module.compiled), configFor("minimal"), harness.services());
        harness.scheduler.runPending();
    }
    harness.scheduler.runPending();
    REQUIRE(harness.retirement.count() == 1);
    CHECK_FALSE(weakEngine.expired());
    static_cast<void>(harness.retirement.take());
    CHECK(weakEngine.expired());
}

TEST_CASE("Runner.threading.engineReleasedByTheOwner", "[vtextension][runner][threading]")
{
    // A runner on the real pool is destroyed and its engine released on this thread; a new engine and
    // runner on the same lane then run normally.
    auto harness = RunnerHarness {};
    auto pool = ExtensionWorkerPool(*harness.backend, 1);
    auto const waitFor = [](auto const& condition) {
        auto const deadline = std::chrono::steady_clock::now() + 30s;
        while (!condition() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        return condition();
    };

    auto weakEngine = std::weak_ptr<Engine> {};
    {
        auto module = loadModule(*harness.backend, toModule(vtextension::testing::MinimalModule));
        weakEngine = module.engine;
        auto runner = Runner(
            std::move(module.engine), std::move(module.compiled), configFor("first"), harness.services(pool));
        runner.post(surfaceOf(10, 1));
        REQUIRE(waitFor([&] { return runner.latest() != nullptr; }));
    }
    REQUIRE(waitFor([&] { return harness.retirement.count() == 1; }));
    CHECK_FALSE(weakEngine.expired()); // The lane did not release it.
    static_cast<void>(harness.retirement.take());
    CHECK(weakEngine.expired()); // This thread did.

    auto module = loadModule(*harness.backend, toModule(vtextension::testing::MinimalModule));
    auto runner = Runner(
        std::move(module.engine), std::move(module.compiled), configFor("second"), harness.services(pool));
    runner.post(surfaceOf(10, 1));
    CHECK(waitFor([&] { return runner.latest() != nullptr; }));
    CHECK(runner.state() == RunnerState::Running);
}

TEST_CASE("Runner.threading.pool", "[vtextension][runner][threading]")
{
    // On the production pool: events posted from several threads while the lane renders; the runner
    // publishes and is torn down cleanly. Asserts structure, never durations.
    auto harness = RunnerHarness {};
    auto pool =
        ExtensionWorkerPool(*harness.backend, 2); // Destroyed (joined) before the services its jobs reach.
    auto module = loadModule(*harness.backend, toModule(vtextension::testing::MinimalModule));
    {
        auto runner = Runner(std::move(module.engine),
                             std::move(module.compiled),
                             configFor("minimal"),
                             harness.services(pool));
        auto posters = std::vector<std::thread> {};
        for (auto const thread: std::views::iota(0U, 2U))
            posters.emplace_back([&runner, thread] {
                for (auto const index: std::views::iota(1U, 200U))
                {
                    runner.post(surfaceOf(index, 1));
                    runner.post(CwdChanged { .cwd = std::to_string(thread) });
                }
            });
        for (auto& poster: posters)
            poster.join();

        // Eventually the last size is rendered.
        auto const deadline = std::chrono::steady_clock::now() + 30s;
        while (!runner.latest() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        CHECK(runner.latest() != nullptr);
        CHECK(runner.state() == RunnerState::Running);
    }
    CHECK(harness.published >= 1); // The runner is gone: no callback races this read.
}

#ifdef VTEXTENSION_TEST_MODULE_DIR
using vtextension::testing::readTestModule;

namespace
{
/// A time zone 90 minutes ahead of UTC.
[[nodiscard]] std::chrono::local_seconds aheadByNinetyMinutes(
    std::chrono::system_clock::time_point when) noexcept
{
    return std::chrono::local_seconds { std::chrono::floor<std::chrono::seconds>(when).time_since_epoch()
                                        + std::chrono::minutes { 90 } };
}
} // namespace

TEST_CASE("Runner.echo.publishes", "[vtextension][runner][guest]")
{
    auto harness = RunnerHarness {};
    auto const runner = makeRunner(harness, readTestModule("echo.wasm"), "echo", { { "text", "hi" } });
    runner->post(surfaceOf(10, 1));
    harness.scheduler.runPending();
    REQUIRE(runner->latest() != nullptr);
    CHECK(runner->latest()->sgrText == "hi");
}

TEST_CASE("Runner.state.utcOffset", "[vtextension][runner][guest]")
{
    // The offset in `state` is the injected time zone's at the wall clock's instant, not the machine's.
    auto harness = RunnerHarness {};
    harness.localTime = &aheadByNinetyMinutes;
    auto const runner = makeRunner(harness, readTestModule("echo.wasm"), "echo", { { "state", "describe" } });
    runner->post(surfaceOf(80, 1));
    harness.scheduler.runPending();
    REQUIRE(runner->latest() != nullptr);
    CHECK(runner->latest()->sgrText.ends_with(";0;90;0pt 0x0"));
}

TEST_CASE("Runner.font.surface", "[vtextension][runner][guest]")
{
    // The surface takes its cell size from the font, followed or not: echo asks its surface for the cell
    // size (CSI 16 t) and shows the answer, `CSI 6 ; height ; width t`, as hex.
    auto harness = RunnerHarness {};
    auto const runner = makeRunner(harness, readTestModule("echo.wasm"), "echo", { { "text", "\033[16t" } });
    runner->post(
        FontChanged { .metrics = FontMetrics { .pointSize = 12, .cellWidth = 10, .cellHeight = 20 } });
    runner->post(surfaceOf(40, 1));
    harness.scheduler.runPending();
    harness.scheduler.advance(ExtensionLimits {}.minFrameInterval);
    REQUIRE(runner->latest() != nullptr);
    CHECK(runner->latest()->sgrText == "1b5b363b32303b313074"); // ESC [ 6 ; 2 0 ; 1 0 t
}

TEST_CASE("Runner.font.redraws", "[vtextension][runner][guest]")
{
    // A frame drawn for cells of another size is stale, whether the extension follows the font or not.
    auto harness = RunnerHarness {};
    auto const runner = makeRunner(harness, readTestModule("echo.wasm"), "echo", { { "text", "A" } });
    runner->post(surfaceOf(40, 1));
    harness.scheduler.runPending();
    auto const before = generationOf(*runner);
    REQUIRE(before >= 1);

    runner->post(
        FontChanged { .metrics = FontMetrics { .pointSize = 14, .cellWidth = 11, .cellHeight = 22 } });
    harness.scheduler.advance(1s);
    CHECK(generationOf(*runner) == before + 1);
    CHECK(runner->subscriptions() == AlwaysDelivered);
}

TEST_CASE("Runner.font.sameCellSize", "[vtextension][runner][guest]")
{
    // Only a new cell size makes a frame stale: a change of point size alone, or the same metrics told again
    // (a replay, the extensions replaced), redraws nothing.
    auto harness = RunnerHarness {};
    auto const runner = makeRunner(harness, readTestModule("echo.wasm"), "echo", { { "text", "A" } });
    runner->post(
        FontChanged { .metrics = FontMetrics { .pointSize = 12, .cellWidth = 10, .cellHeight = 20 } });
    runner->post(surfaceOf(40, 1));
    harness.scheduler.runPending();
    auto const drawn = generationOf(*runner);
    REQUIRE(drawn >= 1);

    runner->post(
        FontChanged { .metrics = FontMetrics { .pointSize = 14, .cellWidth = 10, .cellHeight = 20 } });
    harness.scheduler.advance(1s);
    CHECK(generationOf(*runner) == drawn);

    runner->post(
        FontChanged { .metrics = FontMetrics { .pointSize = 14, .cellWidth = 10, .cellHeight = 20 } });
    harness.scheduler.advance(1s);
    CHECK(generationOf(*runner) == drawn);
}

TEST_CASE("Runner.font.restart", "[vtextension][runner][guest]")
{
    // A font told while the extension is backing off from a fault reaches the instance created after it:
    // echo traps while unfocused, so its first instance faults; the cell size and the focus arrive during the
    // back-off, and the next instance asks its surface for the cell size (CSI 16 t) and shows the answer.
    auto harness = RunnerHarness {};
    auto const runner = makeRunner(
        harness, readTestModule("echo.wasm"), "echo", { { "text", "\033[16t" }, { "trap", "unfocused" } });
    runner->post(surfaceOf(40, 1));
    harness.scheduler.runPending();
    REQUIRE(runner->state() == RunnerState::BackingOff);

    runner->post(
        FontChanged { .metrics = FontMetrics { .pointSize = 12, .cellWidth = 10, .cellHeight = 20 } });
    runner->post(FocusChanged { .state = FocusState::Focused });
    harness.scheduler.advance(1s);
    REQUIRE(runner->state() == RunnerState::Running);
    REQUIRE(runner->latest() != nullptr);
    CHECK(runner->latest()->sgrText == "1b5b363b32303b313074"); // ESC [ 6 ; 2 0 ; 1 0 t
}

TEST_CASE("Runner.subscriptions", "[vtextension][runner][guest]")
{
    // Only the kinds the extension subscribed to are delivered: echo describes every event it gets.
    auto harness = RunnerHarness {};
    auto const runner = makeRunner(
        harness, readTestModule("echo.wasm"), "echo", { { "events", "describe" }, { "subscribe", "cwd" } });
    runner->post(surfaceOf(10, 1));
    harness.scheduler.runPending();
    CHECK(runner->subscriptions() == (AlwaysDelivered | EventKind::Cwd));

    runner->post(CwdChanged { .cwd = "/x" });
    harness.scheduler.advance(1s);
    CHECK(runner->latest()->sgrText == "cwd:/x");

    runner->post(TitleChanged { .title = "unsubscribed" });
    harness.scheduler.advance(1s);
    CHECK(runner->latest()->sgrText == "cwd:/x");
}

TEST_CASE("Runner.frameClamp", "[vtextension][runner][guest]")
{
    // The frame-flood guest requests 1000 frames of 0 ms per render, and asks for a render on every
    // event: renders are paced at minFrameInterval all the same.
    auto harness = RunnerHarness {};
    auto const limits = ExtensionLimits {};
    auto const runner =
        makeRunner(harness, readTestModule("hostile.wasm"), "hostile", { { "mode", "frame-flood" } }, limits);
    runner->post(surfaceOf(80, 1));
    harness.scheduler.runPending();
    REQUIRE(generationOf(*runner) == 1);

    constexpr auto Elapsed = std::chrono::milliseconds { 1s };
    harness.scheduler.advance(Elapsed);
    auto const renders = static_cast<int64_t>(generationOf(*runner));
    INFO("renders over " << Elapsed.count() << " ms: " << renders);
    CHECK(renders <= 1 + (Elapsed / limits.minFrameInterval));
    // The requested frames did arrive: the guest kept rendering, at the clamped rate.
    CHECK(renders >= Elapsed / limits.minFrameInterval);
    CHECK(runner->state() == RunnerState::Running);
}

TEST_CASE("Runner.disable", "[vtextension][runner][guest]")
{
    auto harness = RunnerHarness {};
    auto const runner =
        makeRunner(harness, readTestModule("hostile.wasm"), "hostile", { { "mode", "trap" } });
    runner->post(surfaceOf(80, 1));
    harness.scheduler.advance(10s);
    CHECK(runner->state() == RunnerState::Disabled);
    REQUIRE(runner->latest() != nullptr);
    CHECK(runner->latest()->sgrText.starts_with("⚠ hostile disabled"));
    // The notice is drawn, so that a take-over status line shows it too.
    REQUIRE_FALSE(runner->latest()->lines.empty());
}

TEST_CASE("Runner.repliesRouted", "[vtextension][runner][guest]")
{
    // echo draws DA1; its private terminal answers; the answer comes back as a reply event, which echo
    // renders as hex.
    auto harness = RunnerHarness {};
    auto const runner = makeRunner(harness, readTestModule("echo.wasm"), "echo", { { "text", "\033[c" } });
    runner->post(surfaceOf(40, 1));
    harness.scheduler.runPending();
    harness.scheduler.advance(ExtensionLimits {}.minFrameInterval);
    REQUIRE(runner->latest() != nullptr);
    INFO("rendered: " << runner->latest()->sgrText);
    CHECK(runner->latest()->sgrText.starts_with("1b5b3f")); // ESC [ ?
}

TEST_CASE("Runner.runCommand", "[vtextension][runner][guest]")
{
    auto harness = RunnerHarness {};
    harness.capture.answer = "hi";
    auto const runner =
        makeRunner(harness, readTestModule("spawner.wasm"), "spawner", {}, {}, CommandPermission::Allow);
    runner->post(surfaceOf(40, 1));
    harness.scheduler.runPending();
    REQUIRE(runner->latest() != nullptr);
    CHECK(runner->latest()->sgrText == "ok:0:hi:");
    CHECK(harness.capture.commands == std::vector<std::string> { "'echo' 'hello'" });
    CHECK(harness.capture.timeouts == std::vector { vtbackend::MaxCommandRuntime });
    CHECK(harness.permissionRequests.empty());
}

TEST_CASE("Runner.runCommand.shellDialect", "[vtextension][runner][guest]")
{
    // The command line is written for the shell the capture runs it with, whatever the host.
    auto harness = RunnerHarness {};
    harness.shellDialect = ShellDialect::WindowsCmd;
    harness.capture.answer = "hi";
    auto const runner =
        makeRunner(harness, readTestModule("spawner.wasm"), "spawner", {}, {}, CommandPermission::Allow);
    runner->post(surfaceOf(40, 1));
    harness.scheduler.runPending();
    REQUIRE(runner->latest() != nullptr);
    CHECK(runner->latest()->sgrText == "ok:0:hi:");
    CHECK(harness.capture.commands == std::vector<std::string> { R"(""echo" "hello"")" });
}

TEST_CASE("Runner.runCommand.timedOut", "[vtextension][runner][guest]")
{
    auto harness = RunnerHarness {};
    harness.capture.answer = std::unexpected(
        vtpty::CommandCaptureError { .kind = vtpty::CommandCaptureError::Kind::TimedOut, .cause = {} });
    auto const runner =
        makeRunner(harness, readTestModule("spawner.wasm"), "spawner", {}, {}, CommandPermission::Allow);
    runner->post(surfaceOf(40, 1));
    harness.scheduler.runPending();
    REQUIRE(runner->latest() != nullptr);
    CHECK(runner->latest()->sgrText == "err:timed-out");
}

TEST_CASE("Runner.runCommand.pending", "[vtextension][runner][guest]")
{
    // Unanswered: the call fails at once, and the host is asked exactly once, however often it retries.
    auto harness = RunnerHarness {};
    auto const runner =
        makeRunner(harness, readTestModule("spawner.wasm"), "spawner", {}, {}, CommandPermission::Ask);
    runner->post(surfaceOf(40, 1));
    harness.scheduler.runPending();
    REQUIRE(runner->latest() != nullptr);
    CHECK(runner->latest()->sgrText == "err:pending");
    CHECK(harness.capture.commands.empty());
    REQUIRE(harness.permissionRequests.size() == 1);
    CHECK(harness.permissionRequests.front().hostId == "test@/extensions/test.wasm");
    CHECK_FALSE(harness.permissionRequests.front().digest.empty());

    runner->post(surfaceOf(41, 1));
    harness.scheduler.advance(1s);
    CHECK(generationOf(*runner) == 2);
    CHECK(harness.permissionRequests.size() == 1);

    // Once granted, the next call runs.
    harness.grants.record(harness.permissionRequests.front().hostId,
                          harness.permissionRequests.front().digest,
                          GrantState::Granted);
    harness.capture.answer = "now";
    runner->post(surfaceOf(42, 1));
    harness.scheduler.advance(1s);
    CHECK(runner->latest()->sgrText == "ok:0:now:");
}

TEST_CASE("Runner.control", "[vtextension][runner][control]")
{
    // Control verbs are granted with the commands, and asked for the same way: once, at the first refusal.
    auto harness = RunnerHarness {};
    harness.controlAnswer =
        R"({"version":"9.8.7","protocolVersion":4,"platform":"linux","hostname":"h","uptimeSeconds":1})";
    auto const module =
        vtextension::testing::withManifest(toModule(vtextension::testing::ControlReportingModule),
                                           "name = reporter\nversion = 1\ncontrol read\n");
    auto const runner = makeRunner(harness, module, "reporter", {}, {}, CommandPermission::Ask);
    runner->post(surfaceOf(80, 1));
    harness.scheduler.runPending();
    REQUIRE(runner->latest() != nullptr);
    CHECK(runner->latest()->sgrText == "method 'daemon.info' needs grant 'read': not granted yet");
    CHECK(harness.controlMethods.empty());
    REQUIRE(harness.permissionRequests.size() == 1);

    // The guest renders again on its own timer; it is asked no more.
    harness.scheduler.advance(1s);
    CHECK(harness.permissionRequests.size() == 1);

    harness.grants.record(harness.permissionRequests.front().hostId,
                          harness.permissionRequests.front().digest,
                          GrantState::Granted);
    harness.scheduler.advance(1s);
    CHECK(runner->latest()->sgrText == "9.8.7");
    CHECK_FALSE(harness.controlMethods.empty());
    CHECK(harness.controlMethods.front() == "daemon.info");
}

TEST_CASE("Runner.control.deny", "[vtextension][runner][control]")
{
    // Extensions that may not run commands may not control the terminal either; nobody is asked.
    auto harness = RunnerHarness {};
    auto const module =
        vtextension::testing::withManifest(toModule(vtextension::testing::ControlReportingModule),
                                           "name = reporter\nversion = 1\ncontrol read\n");
    auto const runner = makeRunner(harness, module, "reporter", {}, {}, CommandPermission::Deny);
    runner->post(surfaceOf(80, 1));
    harness.scheduler.runPending();
    REQUIRE(runner->latest() != nullptr);
    CHECK(runner->latest()->sgrText == "method 'daemon.info' needs grant 'read': denied");
    CHECK(harness.permissionRequests.empty());
}

TEST_CASE("Runner.control.allowIsNoGrant", "[vtextension][runner][control]")
{
    // `allow` pre-approves commands only: control waits for the user's answer.
    auto harness = RunnerHarness {};
    auto const module =
        vtextension::testing::withManifest(toModule(vtextension::testing::ControlReportingModule),
                                           "name = reporter\nversion = 1\ncontrol read\n");
    auto const runner = makeRunner(harness, module, "reporter", {}, {}, CommandPermission::Allow);
    runner->post(surfaceOf(80, 1));
    harness.scheduler.runPending();
    REQUIRE(runner->latest() != nullptr);
    CHECK(runner->latest()->sgrText.ends_with("not granted yet"));
    CHECK(harness.permissionRequests.size() == 1);
}

TEST_CASE("Runner.control.notServed", "[vtextension][runner][control]")
{
    // A host serving no control API -- the GUI's -- answers every verb `unsupported`, and asks nobody to
    // grant what it cannot do.
    auto harness = RunnerHarness {};
    auto module =
        loadModule(*harness.backend,
                   vtextension::testing::withManifest(toModule(vtextension::testing::ControlReportingModule),
                                                      "name = reporter\nversion = 1\ncontrol read\n"));
    auto services = harness.services();
    services.callControl = nullptr;
    auto const runner = std::make_unique<Runner>(std::move(module.engine),
                                                 std::move(module.compiled),
                                                 configFor("reporter", {}, {}, CommandPermission::Ask),
                                                 std::move(services));
    runner->post(surfaceOf(80, 1));
    harness.scheduler.runPending();
    REQUIRE(runner->latest() != nullptr);
    CHECK(runner->latest()->sgrText == "the control API is not served here");
    CHECK(harness.permissionRequests.empty());
}

TEST_CASE("Runner.threading.controlClosesItsOwnPane", "[vtextension][runner][control][threading]")
{
    // An extension closing its own pane: the host destroys the Runner while it serves the call, and answers
    // only then -- as a daemon's loop does. The destructor must not wait for that answer, which it would
    // itself be holding up, on the thread every session of the daemon shares.
    auto harness = RunnerHarness {};
    auto closeAsked = std::promise<void> {};
    auto asked = closeAsked.get_future();
    auto closeServed = std::promise<void> {};
    auto const served = closeServed.get_future().share();
    auto calls = std::atomic<int> { 0 };
    auto answeredInTime = false; // Written on the lane, read once the pool is joined.
    auto pool = ExtensionWorkerPool(*harness.backend, 1);

    // A deadline far beyond the test's run, so that only a destructor that waits for the call misses it.
    auto limits = ExtensionLimits {};
    limits.callDeadline = 10s;
    auto module =
        loadModule(*harness.backend,
                   vtextension::testing::withManifest(toModule(vtextension::testing::PaneClosingModule),
                                                      "name = closer\nversion = 1\ncontrol write\n"),
                   limits);
    auto const config = configFor("closer", {}, limits, CommandPermission::Ask);
    harness.grants.record(config.hostId, module.compiled->manifest()->digest, GrantState::Granted);
    auto services = harness.services(pool);
    services.callControl =
        [&](std::string_view /*method*/,
            std::string_view /*params*/,
            vtcontrol::ControlGrants /*granted*/,
            std::chrono::milliseconds timeout) -> std::expected<std::string, vtcontrol::ControlError> {
        if (calls.fetch_add(1) == 0)
        {
            closeAsked.set_value();
            answeredInTime = served.wait_for(timeout) == std::future_status::ready;
        }
        if (!answeredInTime)
            return std::unexpected(vtcontrol::ControlError {
                .code = vtcontrol::ControlErrorCode::Failed, .message = "timed out", .candidates = {} });
        return "{}";
    };
    auto runner = std::make_unique<Runner>(
        std::move(module.engine), std::move(module.compiled), config, std::move(services));
    runner->post(surfaceOf(10, 1));

    // The host's side: the close destroys the pane, and its runner with it, before the call is answered.
    REQUIRE(asked.wait_for(30s) == std::future_status::ready);
    runner.reset();
    closeServed.set_value();
    pool.stop();
    CHECK(answeredInTime);
    CHECK(calls.load() == 1); // A render after the Runner is gone reaches no host.
}

TEST_CASE("Runner.threading.spinNeverBlocksThePoster", "[vtextension][runner][threading][guest]")
{
    // A guest that spins in every render is cut off by its deadline, backed off, and disabled; the
    // thread posting to it is never held up meanwhile, and its events are dropped once the queue is full.
    auto harness = RunnerHarness {};
    auto pool = ExtensionWorkerPool(*harness.backend, 1);
    auto module = loadModule(*harness.backend, readTestModule("hostile.wasm"));
    auto runner = Runner(std::move(module.engine),
                         std::move(module.compiled),
                         configFor("hostile", { { "mode", "spin" } }),
                         harness.services(pool));
    runner.post(surfaceOf(80, 1));
    for (auto const index: std::views::iota(0U, 1000U))
        runner.post(ReplyBytes { .bytes = { static_cast<uint8_t>(index) } });

    auto const deadline = std::chrono::steady_clock::now() + 60s;
    while (runner.state() != RunnerState::Disabled && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    CHECK(runner.state() == RunnerState::Disabled);
    REQUIRE(runner.latest() != nullptr);
    CHECK(runner.latest()->sgrText == "⚠ hostile disabled: extension call exceeded its deadline");
}
#else
TEST_CASE("Runner: the Rust-guest cases", "[vtextension][runner][guest]")
{
    SKIP("the Rust test guests are not built (CONTOUR_EXTENSIONS_BUNDLED=OFF)");
}
#endif
