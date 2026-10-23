// SPDX-License-Identifier: Apache-2.0
//
// The extensions bundled with Contour, each loaded from the module the build made and run through a Runner on
// test doubles: what each one draws, the commands it runs, and that being installed loads none of them.
#include <vtbackend/core/Color.hpp>
#include <vtbackend/core/Primitives.hpp>

#include <core/platform/testing/InMemoryFileSystem.hpp>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <vtextension/CommandLine.hpp>
#include <vtextension/ExtensionComposition.hpp>
#include <vtextension/Manifest.hpp>
#include <vtextension/Runner.hpp>
#include <vtextension/testing/Harness.hpp>
#include <vtextension/testing/RunnerHarness.hpp>
#include <vtextension/testing/TestModules.hpp>
#include <vtextension/wasm/DefaultBackend.hpp>

using namespace std::chrono_literals;
using namespace vtextension;
using vtextension::testing::RunnerHarness;

namespace
{

/// One bundled extension: the name a configuration selects it by, which is the name it is installed under.
struct BundledExtension
{
    std::string_view name;                  ///< `<name>.wasm` in the bundled directory.
    std::string_view artifact;              ///< What cargo calls the module it builds.
    std::vector<std::string_view> commands; ///< The commands its manifest declares.
};

/// Every extension Contour ships.
[[nodiscard]] std::vector<BundledExtension> bundledExtensions()
{
    return {
        { .name = "clock", .artifact = "clock.wasm", .commands = {} },
        { .name = "git", .artifact = "git.wasm", .commands = { "status" } },
        { .name = "sysload", .artifact = "sysload.wasm", .commands = { "uptime", "cpu-windows" } },
        { .name = "agent-status", .artifact = "agent_status.wasm", .commands = {} },
        { .name = "plasma", .artifact = "plasma.wasm", .commands = {} },
    };
}

} // namespace

TEST_CASE("Bundled: an installed extension loads only when a profile declares it", "[vtextension][bundled]")
{
    // The bundled directory holds every module, as an installation does; a profile names one of them.
    auto const directory = std::filesystem::path { "/usr/share/contour/extensions" };
    auto fileSystem = core::platform::testing::InMemoryFileSystem {};
    for (auto const& extension: bundledExtensions())
        REQUIRE(
            fileSystem.writeFile(directory / (std::string(extension.name) + ".wasm"), "WASM").has_value());
    auto const directories = std::array { directory };
    auto const lookup = ExtensionModuleLookup { .fileSystem = fileSystem, .bundledDirectories = directories };

    SECTION("none declared, none loaded")
    {
        auto const composed = composeExtensions(DeclaredExtensions {}, lookup, {}, {});
        CHECK(composed.host.extensions.empty());
        CHECK(composed.diagnostics.empty());
    }

    SECTION("one declared, only that one loaded, under the name it is installed as")
    {
        auto declared = DeclaredExtensions {};
        declared.extensions.push_back(DeclaredExtension {
            .name = "agent-status", .path = {}, .config = {}, .runCommand = CommandPermission::Ask });
        auto const composed = composeExtensions(declared, lookup, {}, {});
        CHECK(composed.diagnostics.empty());
        REQUIRE(composed.host.extensions.size() == 1);
        CHECK(composed.host.extensions[0].name == "agent-status");
        CHECK_FALSE(composed.host.extensions[0].module.empty());
    }
}

#ifdef VTEXTENSION_TEST_MODULE_DIR
using vtextension::testing::loadModule;
using vtextension::testing::readTestModule;
using vtextension::testing::RunningGuest;

namespace
{

/// The size of a cell of the surfaces here, in pixels.
constexpr auto CellPixelSize = vtbackend::ImageSize { vtbackend::Width(8), vtbackend::Height(16) };

/// @return A runner of the bundled module @p artifact on @p harness.
[[nodiscard]] std::unique_ptr<Runner> makeRunner(RunnerHarness& harness,
                                                 std::string_view artifact,
                                                 ConfigMap config = {},
                                                 CommandPermission runCommand = CommandPermission::Allow)
{
    auto module = loadModule(*harness.backend, readTestModule(artifact));
    return std::make_unique<Runner>(std::move(module.engine),
                                    std::move(module.compiled),
                                    RunnerConfig { .name = std::string(artifact),
                                                   .hostId = "bundled@/usr/share/contour/extensions",
                                                   .config = std::move(config),
                                                   .placement = Placement::Segment,
                                                   .runCommand = runCommand,
                                                   .cellPixelSize = CellPixelSize,
                                                   .limits = {} },
                                    harness.services());
}

[[nodiscard]] Resized surfaceOf(uint32_t cols, uint32_t rows)
{
    return Resized { .size = SurfaceSize { .cols = cols, .rows = rows } };
}

/// @return The text of @p runner's latest frame; empty before the first.
[[nodiscard]] std::string textOf(Runner const& runner)
{
    auto const latest = runner.latest();
    return latest ? latest->sgrText : std::string {};
}

/// @return The time of day @p hours : @p minutes : 56 UTC on some day.
[[nodiscard]] std::chrono::system_clock::time_point at(int hours, int minutes)
{
    using namespace std::chrono;
    return sys_days { year { 2026 } / October / 21 } + std::chrono::hours { hours }
           + std::chrono::minutes { minutes } + 56s;
}

/// When daylight saving ends in Central Europe in 2026: 03:00 CEST becomes 02:00 CET.
constexpr auto DaylightSavingEnds =
    std::chrono::sys_days { std::chrono::year { 2026 } / std::chrono::October / 25 }
    + std::chrono::hours { 1 };

/// Central European time: two hours ahead of UTC until @ref DaylightSavingEnds, one hour from then.
[[nodiscard]] std::chrono::local_seconds centralEurope(std::chrono::system_clock::time_point when) noexcept
{
    using namespace std::chrono;
    auto const offset = when < DaylightSavingEnds ? hours { 2 } : hours { 1 };
    return local_seconds { floor<seconds>(when).time_since_epoch() + offset };
}

/// A time zone two hours ahead of UTC all year.
[[nodiscard]] std::chrono::local_seconds aheadByTwoHours(std::chrono::system_clock::time_point when) noexcept
{
    using namespace std::chrono;
    return local_seconds { floor<seconds>(when).time_since_epoch() + hours { 2 } };
}

[[nodiscard]] FocusChanged focused()
{
    return FocusChanged { .state = FocusState::Focused };
}

[[nodiscard]] FocusChanged unfocused()
{
    return FocusChanged { .state = FocusState::Unfocused };
}

/// @return The top row of @p runner's latest frame, which must exist.
[[nodiscard]] vtbackend::Line topRowOf(Runner const& runner)
{
    REQUIRE(runner.latest() != nullptr);
    REQUIRE_FALSE(runner.latest()->lines.empty());
    return runner.latest()->lines[0];
}

/// What `git status --porcelain=v2 --branch` prints in a repository on `main` with one change.
constexpr auto DirtyMain = std::string_view { "# branch.oid 0123456789abcdef\n"
                                              "# branch.head main\n"
                                              "1 .M N... 100644 100644 100644 0123456 0123456 README.md\n" };

} // namespace

TEST_CASE("Bundled: every module declares what it is", "[vtextension][bundled][guest]")
{
    auto const backend = vtextension::wasm::makeDefaultBackend();
    for (auto const& extension: bundledExtensions())
    {
        INFO(extension.name);
        auto const module = loadModule(*backend, readTestModule(extension.artifact));
        auto const& manifest = module.compiled->manifest();
        REQUIRE(manifest.has_value());
        CHECK(manifest->name == extension.name);
        auto names = std::vector<std::string_view> {};
        for (auto const& command: manifest->commands)
            names.emplace_back(command.name);
        CHECK(names == extension.commands);
    }
}

TEST_CASE("Bundled: every declared command can be written for both shells", "[vtextension][bundled][guest]")
{
    // A command runs through the platform shell (CommandCapture takes one command line), and an argument the
    // shell would interpret is refused rather than quoted: cmd.exe's metacharacters cannot be. So a declared
    // argv must survive both dialects, or the command can never run on that platform.
    auto const backend = vtextension::wasm::makeDefaultBackend();
    for (auto const& extension: bundledExtensions())
    {
        auto const module = loadModule(*backend, readTestModule(extension.artifact));
        auto const& manifest = module.compiled->manifest();
        REQUIRE(manifest.has_value());
        for (auto const& command: manifest->commands)
        {
            INFO(extension.name << ": " << command.name);
            auto argv = std::vector<std::string> {};
            for (auto const& element: command.argvTemplate)
                argv.emplace_back(placeholderName(element) ? std::string { "/home/user/src" } : element);
            for (auto const dialect: { ShellDialect::Posix, ShellDialect::WindowsCmd })
            {
                INFO((dialect == ShellDialect::Posix ? "POSIX" : "cmd.exe"));
                CHECK(shellCommandLine(argv, dialect).has_value());
            }
        }
    }
}

TEST_CASE("Bundled: together they follow every event kind", "[vtextension][bundled][guest]")
{
    // The bundle doubles as the first conformance suite: some extension of it subscribes to each kind.
    // plasma follows the ones its text names.
    auto harness = RunnerHarness {};
    auto kinds = EventKinds {};
    for (auto const& extension: bundledExtensions())
    {
        INFO(extension.name);
        auto config = ConfigMap {};
        if (extension.name == "plasma")
            config.emplace_back("text", "{mode} {title} {tabs}");
        auto const runner =
            makeRunner(harness, extension.artifact, std::move(config), CommandPermission::Deny);
        runner->post(surfaceOf(40, 1));
        harness.scheduler.runPending();
        REQUIRE(runner->state() == RunnerState::Running);
        kinds |= runner->subscriptions();
    }
    CHECK(kinds
          == (AlwaysDelivered
              | EventKinds { EventKind::Title,
                             EventKind::Cwd,
                             EventKind::ForegroundProcess,
                             EventKind::InputMode,
                             EventKind::Tabs,
                             EventKind::UserVar,
                             EventKind::Progress,
                             EventKind::Focus,
                             EventKind::Font }));
}

TEST_CASE("Bundled: clock", "[vtextension][bundled][guest]")
{
    auto harness = RunnerHarness {};

    SECTION("the local time of day, in the zone the host reports")
    {
        harness.wallClock.setNow(at(12, 34));
        harness.localTime = &aheadByTwoHours;
        auto const runner = makeRunner(harness, "clock.wasm");
        runner->post(surfaceOf(10, 1));
        harness.scheduler.runPending();
        CHECK(textOf(*runner) == "14:34");

        // The next minute is drawn once it has come.
        harness.wallClock.advance(1min);
        harness.scheduler.advance(1min);
        CHECK(textOf(*runner) == "14:35");
    }

    SECTION("across the end of daylight saving")
    {
        using namespace std::chrono;
        harness.localTime = &centralEurope;
        harness.wallClock.setNow(DaylightSavingEnds - 30s);
        auto const runner = makeRunner(harness, "clock.wasm");
        runner->post(surfaceOf(10, 1));
        harness.scheduler.runPending();
        CHECK(textOf(*runner) == "02:59");

        harness.wallClock.advance(30s);
        harness.scheduler.advance(30s);
        CHECK(textOf(*runner) == "02:00");
    }
}

TEST_CASE("Bundled: clock wakes once a minute", "[vtextension][bundled][guest]")
{
    // At 12:34:56.000 the next minute is four seconds away; at 12:35:00.010, a minute less 10 ms.
    auto guest = RunningGuest(readTestModule("clock.wasm"));
    guest.services.snapshot.now = std::chrono::floor<std::chrono::milliseconds>(at(12, 34));
    std::ignore = guest.loadAndRender({});
    REQUIRE(guest.services.frameRequests.size() == 1);
    CHECK(guest.services.frameRequests.back() == 4s);

    guest.services.snapshot.now += 4010ms;
    REQUIRE(guest.instance().update(TimerFired {}).has_value());
    REQUIRE(guest.services.frameRequests.size() == 2);
    CHECK(guest.services.frameRequests.back() == 59990ms);
}

TEST_CASE("Bundled: git", "[vtextension][bundled][guest]")
{
    auto harness = RunnerHarness {};
    harness.capture.answer = std::string(DirtyMain);
    auto const runner = makeRunner(harness, "git.wasm");
    runner->post(surfaceOf(40, 1));
    runner->post(focused());
    harness.scheduler.runPending();
    // No working directory is known yet: nothing to ask git about.
    CHECK(harness.capture.commands.empty());
    CHECK(textOf(*runner).empty());

    runner->post(CwdChanged { .cwd = "/home/user/repo" });
    harness.scheduler.advance(1s);
    CHECK(textOf(*runner) == "main*");
    REQUIRE(harness.capture.commands.size() == 1);
    // --no-optional-locks: a status taken in the background must never hold the index lock a git command
    // the user runs needs.
    CHECK(harness.capture.commands.back()
          == "'git' '--no-optional-locks' '-C' '/home/user/repo' 'status' '--porcelain=v2' '--branch'");

    SECTION("it does not poll")
    {
        harness.scheduler.advance(10min);
        CHECK(harness.capture.commands.size() == 1);
    }

    SECTION("a command that finishes refreshes it")
    {
        harness.capture.answer = "# branch.oid 0123456789abcdef\n# branch.head main\n";
        runner->post(ForegroundProcessChanged { .name = "zsh" });
        harness.scheduler.advance(1s);
        CHECK(harness.capture.commands.size() == 2);
        CHECK(textOf(*runner) == "main");
    }

    SECTION("gaining the focus refreshes it")
    {
        runner->post(unfocused());
        harness.scheduler.advance(1s);
        CHECK(harness.capture.commands.size() == 1);

        harness.capture.answer = "# branch.oid 0123456789abcdef\n# branch.head topic\n";
        runner->post(focused());
        harness.scheduler.advance(1s);
        REQUIRE(harness.capture.commands.size() == 2);
        CHECK(textOf(*runner) == "topic");
    }

    SECTION("a detached head shows its commit")
    {
        harness.capture.answer = "# branch.oid 0123456789abcdef\n# branch.head (detached)\n";
        runner->post(ForegroundProcessChanged { .name = "zsh" });
        harness.scheduler.advance(1s);
        CHECK(textOf(*runner) == "0123456");
    }

    SECTION("outside a repository it shows nothing")
    {
        harness.capture.answer = "";
        runner->post(CwdChanged { .cwd = "/tmp" });
        harness.scheduler.advance(1s);
        CHECK(textOf(*runner).empty());
    }

    SECTION("a command that fails is logged, and shows nothing")
    {
        harness.capture.answer = std::unexpected(vtpty::CommandCaptureError {
            .kind = vtpty::CommandCaptureError::Kind::SpawnFailed, .cause = {} });
        runner->post(ForegroundProcessChanged { .name = "zsh" });
        harness.scheduler.advance(1s);
        CHECK(textOf(*runner).empty());
        REQUIRE(harness.logged.size() == 1);
        CHECK(harness.logged.back().contains("git status"));

        // The same failure again is not logged again.
        runner->post(ForegroundProcessChanged { .name = "zsh" });
        harness.scheduler.advance(1s);
        CHECK(harness.logged.size() == 1);
    }
}

TEST_CASE("Bundled: git shows the branch in a pane never focused", "[vtextension][bundled][guest]")
{
    // Only recurring work waits for the focus: a pane's first render and the refreshes events ask for run
    // in an unfocused split too.
    auto harness = RunnerHarness {};
    harness.capture.answer = std::string(DirtyMain);
    auto const runner = makeRunner(harness, "git.wasm");
    runner->post(CwdChanged { .cwd = "/home/user/repo" });
    runner->post(surfaceOf(40, 1));
    harness.scheduler.advance(1s);
    CHECK(textOf(*runner) == "main*");
    auto const afterLoad = harness.capture.commands.size();
    CHECK(afterLoad >= 1);

    harness.capture.answer = "# branch.oid 0123456789abcdef\n# branch.head topic\n";
    runner->post(CwdChanged { .cwd = "/home/user/other" });
    harness.scheduler.advance(1s);
    CHECK(textOf(*runner) == "topic");
    REQUIRE_FALSE(harness.capture.commands.empty());
    CHECK(harness.capture.commands.back().contains("'/home/user/other'"));

    runner->post(ForegroundProcessChanged { .name = "zsh" });
    harness.scheduler.advance(1s);
    CHECK(harness.capture.commands.size() == afterLoad + 2);
}

TEST_CASE("Bundled: git polls only when asked to, and only while focused", "[vtextension][bundled][guest]")
{
    auto harness = RunnerHarness {};
    harness.capture.answer = std::string(DirtyMain);
    auto const runner = makeRunner(harness, "git.wasm", { { "refresh-ms", "5000" } });
    runner->post(surfaceOf(40, 1));
    runner->post(focused());
    runner->post(CwdChanged { .cwd = "/home/user/repo" });
    harness.scheduler.advance(1s);
    auto const first = harness.capture.commands.size();
    CHECK(first >= 1);

    harness.scheduler.advance(5s);
    CHECK(harness.capture.commands.size() == first + 1);

    runner->post(unfocused());
    harness.scheduler.advance(1min);
    CHECK(harness.capture.commands.size() == first + 1);
}

TEST_CASE("Bundled: git asks before it runs git", "[vtextension][bundled][guest]")
{
    auto harness = RunnerHarness {};
    auto const runner = makeRunner(harness, "git.wasm", {}, CommandPermission::Ask);
    runner->post(surfaceOf(40, 1));
    runner->post(focused());
    runner->post(CwdChanged { .cwd = "/home/user/repo" });
    harness.scheduler.advance(1s);
    CHECK(harness.capture.commands.empty());
    CHECK(harness.permissionRequests.size() == 1);
    CHECK(textOf(*runner).empty());
    // Waiting for an answer is no failure to log.
    CHECK(harness.logged.empty());
}

TEST_CASE("Bundled: sysload", "[vtextension][bundled][guest]")
{
    auto harness = RunnerHarness {};

    SECTION("the load averages uptime reports, refreshed every five seconds")
    {
        harness.capture.answer = " 10:23:45 up 3 days,  2:01,  2 users,  load average: 0.52, 0.58, 0.59\n";
        auto const runner = makeRunner(harness, "sysload.wasm");
        runner->post(surfaceOf(40, 1));
        runner->post(focused());
        harness.scheduler.runPending();
        CHECK(textOf(*runner) == "load 0.52 0.58 0.59");
        // Read at load, and again on gaining the focus: uptime each time.
        REQUIRE_FALSE(harness.capture.commands.empty());
        CHECK(std::ranges::all_of(harness.capture.commands,
                                  [](std::string const& command) { return command == "'uptime'"; }));

        harness.capture.answer = "10:23  up 3 days,  2:01, 2 users, load averages: 1.23 1.45 1.67\n";
        harness.scheduler.advance(5s);
        CHECK(textOf(*runner) == "load 1.23 1.45 1.67");
    }

    SECTION("without uptime, the processor load")
    {
        harness.capture.answer = std::unexpected(vtpty::CommandCaptureError {
            .kind = vtpty::CommandCaptureError::Kind::SpawnFailed, .cause = {} });
        auto const runner = makeRunner(harness, "sysload.wasm");
        runner->post(surfaceOf(40, 1));
        runner->post(focused());
        harness.scheduler.runPending();
        REQUIRE(harness.capture.commands.size() >= 2);
        CHECK(harness.capture.commands[0] == "'uptime'");
        CHECK(harness.capture.commands[1].starts_with("'powershell'"));

        harness.capture.answer = "17\r\n";
        harness.scheduler.advance(5s);
        CHECK(textOf(*runner) == "cpu 17%");
    }
}

TEST_CASE("Bundled: sysload refreshes only while focused", "[vtextension][bundled][guest]")
{
    // Its first render runs unfocused too; the five-second refresh waits for the focus, which refreshes.
    auto harness = RunnerHarness {};
    harness.capture.answer = " 10:23:45 up 3 days,  2:01,  2 users,  load average: 0.52, 0.58, 0.59\n";
    auto const runner = makeRunner(harness, "sysload.wasm");
    runner->post(surfaceOf(40, 1));
    harness.scheduler.runPending();
    CHECK(textOf(*runner) == "load 0.52 0.58 0.59");
    CHECK(harness.capture.commands.size() == 1);

    harness.scheduler.advance(30s);
    CHECK(harness.capture.commands.size() == 1);

    harness.capture.answer = " 10:24:15 up 3 days,  2:02,  2 users,  load average: 1.00, 0.60, 0.59\n";
    runner->post(focused());
    harness.scheduler.advance(1s);
    CHECK(harness.capture.commands.size() == 2);
    CHECK(textOf(*runner) == "load 1.00 0.60 0.59");

    harness.scheduler.advance(5s);
    CHECK(harness.capture.commands.size() == 3);

    runner->post(unfocused());
    harness.scheduler.advance(1min);
    CHECK(harness.capture.commands.size() <= 4);
}

TEST_CASE("Bundled: agent-status", "[vtextension][bundled][guest]")
{
    // The badge's background, by state: what agent-status draws them in.
    constexpr auto Working = vtbackend::RGBColor { 0xE5, 0xA5, 0x0A };
    constexpr auto Waiting = vtbackend::RGBColor { 0xD7, 0x3A, 0x49 };
    constexpr auto Done = vtbackend::RGBColor { 0x2E, 0xA0, 0x43 };

    auto harness = RunnerHarness {};
    auto const runner = makeRunner(harness, "agent_status.wasm", {}, CommandPermission::Deny);
    runner->post(surfaceOf(40, 1));
    harness.scheduler.runPending();
    CHECK(runner->subscriptions().contains(EventKind::UserVar));
    CHECK(runner->subscriptions().contains(EventKind::Progress));
    CHECK(textOf(*runner).empty());

    auto const post = [&](auto event) {
        runner->post(std::move(event));
        harness.scheduler.advance(1s);
    };
    auto const badgeAfter = [&](std::string name, std::string value) {
        post(UserVarChanged { .name = std::move(name), .value = std::move(value) });
        return topRowOf(*runner).useCellAt(vtbackend::ColumnOffset(0)).backgroundColor();
    };

    SECTION("each state in a colour of its own")
    {
        CHECK(badgeAfter("agent_status", "waiting") == vtbackend::Color(Waiting));
        CHECK(textOf(*runner).contains("waiting"));
        CHECK(badgeAfter("agent_status", "working") == vtbackend::Color(Working));
        CHECK(badgeAfter("agent_status", "done") == vtbackend::Color(Done));
        // Another variable leaves the badge alone.
        CHECK(badgeAfter("other", "waiting") == vtbackend::Color(Done));
        // Unset, the badge goes.
        std::ignore = badgeAfter("agent_status", "");
        CHECK(textOf(*runner).empty());
    }

    SECTION("a value is shown as text, never as sequences, and bounded")
    {
        // Any program in the pane can set a user variable to any bytes.
        std::ignore = badgeAfter("agent_status", "x\033[2Jy\u009b31mz");
        auto row = topRowOf(*runner);
        auto const text = row.toUtf8Trimmed();
        CHECK(text.contains("x[2Jy31mz"));
        CHECK_FALSE(text.contains('\033'));

        std::ignore = badgeAfter("agent_status", std::string(100, 'a'));
        CHECK(textOf(*runner).contains(std::string(32, 'a')));
        CHECK_FALSE(textOf(*runner).contains(std::string(33, 'a')));
    }

    SECTION("the progress an agent reports, beside its state")
    {
        post(ProgressChanged { .progress = Progress { .state = ProgressState::Normal, .percent = 42 } });
        CHECK(textOf(*runner).contains("42%"));
        std::ignore = badgeAfter("agent_status", "working");
        CHECK(textOf(*runner).contains("working"));
        CHECK(textOf(*runner).contains("42%"));

        post(
            ProgressChanged { .progress = Progress { .state = ProgressState::Indeterminate, .percent = 0 } });
        CHECK_FALSE(textOf(*runner).contains('%'));
        CHECK(textOf(*runner).contains("…"));

        post(ProgressChanged { .progress = Progress { .state = ProgressState::Hidden, .percent = 0 } });
        CHECK_FALSE(textOf(*runner).contains("…"));
        CHECK(textOf(*runner).contains("working"));
    }
}

TEST_CASE("Bundled: plasma", "[vtextension][bundled][guest]")
{
    SECTION("its output is a Sixel image")
    {
        auto guest = RunningGuest(readTestModule("plasma.wasm"));
        guest.services.snapshot.focus = FocusState::Focused;
        auto const rendered = guest.loadAndRender({});
        CHECK(rendered.starts_with("\033P"));
        // Animated while focused: it asks for a frame about thirty times a second.
        REQUIRE_FALSE(guest.services.frameRequests.empty());
        CHECK(guest.services.frameRequests.back() == 33ms);

        // 80 cells of the size it assumes, until it has asked for the real one; then of that size.
        CHECK(rendered.contains("\"1;1;640;16"));
        CHECK(rendered.ends_with("\033[16t"));
        auto const reply = std::string_view { "\033[6;20;10t" };
        auto const answered = guest.instance().update(ReplyBytes { .bytes = { reply.begin(), reply.end() } });
        REQUIRE(answered.has_value());
        CHECK(*answered == RenderRequest::Render);
        auto const resized = guest.instance().render(vtextension::testing::surface(80, 1));
        REQUIRE(resized.has_value());
        CHECK(resized->contains("\"1;1;800;20"));
        CHECK_FALSE(resized->ends_with("\033[16t"));
    }

    SECTION("the surface shows the image, the configured text on top, and it animates while focused")
    {
        auto harness = RunnerHarness {};
        auto const runner =
            makeRunner(harness, "plasma.wasm", { { "text", "Contour" } }, CommandPermission::Deny);
        runner->post(surfaceOf(80, 1));
        runner->post(focused());
        harness.scheduler.runPending();
        auto line = topRowOf(*runner);
        CHECK(line.useCellAt(vtbackend::ColumnOffset(0)).imageFragment() != nullptr);
        CHECK(line.useCellAt(vtbackend::ColumnOffset(79)).imageFragment() != nullptr);
        CHECK(textOf(*runner).contains("Contour"));
        // Plain text follows nothing but the focus and the font.
        CHECK(runner->subscriptions()
              == (AlwaysDelivered | EventKinds { EventKind::Focus, EventKind::Font }));

        auto const generation = runner->latest()->generation;
        harness.scheduler.advance(1s);
        CHECK(runner->latest()->generation > generation + 10);
        CHECK(runner->state() == RunnerState::Running);

        // Unfocused, it holds still.
        runner->post(unfocused());
        harness.scheduler.advance(1s);
        auto const still = runner->latest()->generation;
        harness.scheduler.advance(1s);
        CHECK(runner->latest()->generation == still);
    }

    SECTION("a font change draws the image again at the new cell size")
    {
        auto guest = RunningGuest(readTestModule("plasma.wasm"));
        REQUIRE(guest.instance().load({}).value().contains(EventKind::Font));
        REQUIRE(
            guest.instance().render(vtextension::testing::surface(80, 1)).value().contains("\"1;1;640;16"));

        auto const changed = guest.instance().update(
            FontChanged { .metrics = { .pointSize = 15, .cellWidth = 12, .cellHeight = 24 } });
        REQUIRE(changed.has_value());
        CHECK(*changed == RenderRequest::Render);
        CHECK(guest.instance().render(vtextension::testing::surface(80, 1)).value().contains("\"1;1;960;24"));

        // The same metrics again change nothing.
        auto const again = guest.instance().update(
            FontChanged { .metrics = { .pointSize = 15, .cellWidth = 12, .cellHeight = 24 } });
        CHECK(again.value() == RenderRequest::None);
    }

    SECTION("it takes the cell size from the font at load, so it does not need to ask")
    {
        auto guest = RunningGuest(readTestModule("plasma.wasm"));
        guest.services.snapshot.font = FontMetrics { .pointSize = 15, .cellWidth = 12, .cellHeight = 24 };
        auto const rendered = guest.loadAndRender({});
        CHECK(rendered.contains("\"1;1;960;24"));
        CHECK_FALSE(rendered.ends_with("\033[16t"));
    }

    SECTION("never focused, it draws one still frame; focused, it animates")
    {
        auto guest = RunningGuest(readTestModule("plasma.wasm"));
        auto const rendered = guest.loadAndRender({});
        CHECK(rendered.starts_with("\033P"));
        CHECK(guest.services.frameRequests.empty());

        auto const answered = guest.instance().update(FocusChanged { .state = FocusState::Focused });
        REQUIRE(answered.has_value());
        REQUIRE(guest.services.frameRequests.size() == 1);
        CHECK(guest.services.frameRequests.back() == 33ms);
    }

    SECTION("its text shows the mode, the title and the tabs the status line it replaces would")
    {
        auto harness = RunnerHarness {};
        auto const runner = makeRunner(
            harness, "plasma.wasm", { { "text", "{mode} | {title} | {tabs}" } }, CommandPermission::Deny);
        runner->post(surfaceOf(80, 1));
        harness.scheduler.runPending();
        CHECK(runner->subscriptions().contains(EventKind::InputMode));
        CHECK(runner->subscriptions().contains(EventKind::Title));
        CHECK(runner->subscriptions().contains(EventKind::Tabs));

        runner->post(InputModeChanged { .mode = InputMode::Normal });
        runner->post(TitleChanged { .title = "vim" });
        runner->post(TabsChanged {
            .tabs = { Tab { .title = "zsh" }, Tab { .title = "vim", .activity = TabActivity::Active } } });
        harness.scheduler.advance(1s);
        CHECK(textOf(*runner).contains("NORMAL | vim | zsh [vim]"));
    }

    SECTION("its text is centred by the cells it takes, not by its characters")
    {
        // Two wide characters take four cells: at 80 columns the text starts in column 39. Each of these
        // pairs is two wide characters: CJK, the emoji of U+1FA70..1FAFF, U+2600..27BF and U+2B00..2BFF, and
        // U+1F000..1F2FF.
        for (auto const* text: { "日本", "🩰🫠", "☕⚡", "⬛⭐", "🀄🃏" })
        {
            INFO(text);
            auto guest = RunningGuest(readTestModule("plasma.wasm"));
            CHECK(guest.loadAndRender({ { "text", text } }).contains("\033[1;39H"));
        }
    }

    SECTION("a wide take-over's first frame is bounded")
    {
        // A 4K-wide status line of two rows, and far wider and taller ones, each drawn from scratch. The work
        // per frame is capped, which shows in what it encodes: past the cap a larger surface yields no larger
        // image. Measured in bytes, not time -- the deadline is lifted so a slow machine cannot decide it.
        auto limits = ExtensionLimits {};
        limits.callDeadline = std::chrono::seconds { 30 };
        auto sizes = std::vector<std::size_t> {};
        for (auto const [cols, rows]:
             std::array { std::pair { 480, 2 }, std::pair { 1000, 3 }, std::pair { 2000, 4 } })
        {
            INFO(cols << "x" << rows);
            auto guest = RunningGuest(readTestModule("plasma.wasm"), limits);
            REQUIRE(guest.instance().load({}).has_value());
            auto const rendered = guest.instance().render(vtextension::testing::surface(cols, rows));
            REQUIRE(rendered.has_value());
            CHECK(rendered->starts_with("\033P"));
            sizes.push_back(rendered->size());
        }
        // 960 cells to 8000 is more than eight times the surface; capped, the image grows by far less.
        INFO("encoded " << sizes[0] << ", " << sizes[1] << ", " << sizes[2] << " bytes");
        CHECK(sizes[2] <= 2 * sizes[0]);
    }
}

#else
TEST_CASE("Bundled: the bundled modules", "[vtextension][bundled][guest]")
{
    SKIP("the bundled extensions are not built (CONTOUR_EXTENSIONS_BUNDLED=OFF)");
}
#endif
