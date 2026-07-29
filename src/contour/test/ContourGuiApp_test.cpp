// SPDX-License-Identifier: Apache-2.0
//
// Tests for the headless-reachable surface of ContourGuiApp: the parameter/config accessors the
// GUI boot path and TerminalSession read (profile resolution, early-exit threshold, dump-state
// path, resource resolution). The event-loop-driven paths (run/terminalGuiAction) are covered by
// the offscreen e2e app runs, not here.

#include <contour/ContourGuiApp.h>
#include <contour/SessionFactory.h>
#include <contour/TerminalSession.h>
#include <contour/test/GuiTestFixtures.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>

using contour::test::TestApp;

TEST_CASE("ContourGuiApp resolves the default profile and its config", "[contour][app]")
{
    TestApp app;

    // The default-constructed config seeds a "main" profile; profileName() resolves to it via the
    // default_profile key.
    CHECK(app.app().profileName() == "main");
    CHECK(app.app().config().profile("main") != nullptr);
}

TEST_CASE("ContourGuiApp early-exit threshold falls back to the documented default", "[contour][app]")
{
    TestApp app;

    // With no --early-exit-threshold override (parameter defaults to -1) and the config at its
    // default, the accessor returns the documented default threshold.
    auto const threshold = app.app().earlyExitThreshold();
    CHECK(threshold == std::chrono::seconds(contour::config::documentation::DefaultEarlyExitThreshold));
}

TEST_CASE("ContourGuiApp reports no dump-state path by default", "[contour][app]")
{
    TestApp app;
    CHECK_FALSE(app.app().dumpStateAtExit().has_value());
}

TEST_CASE("ContourGuiApp::resolveResource falls back to the qrc scheme for an unknown path", "[contour][app]")
{
    // A resource that exists neither under the config home nor the dev source dir resolves to the
    // bundled qrc: URL (the production fallback for shipped assets).
    auto const url = contour::ContourGuiApp::resolveResource("definitely/not/a/real/asset.xyz");
    CHECK(url.scheme() == "qrc");
    CHECK(url.toString().contains("contour"));
}

TEST_CASE("the attach boot window adopts the primary daemon window", "[contour][app][attach]")
{
    using contour::primaryDaemonWindowToAdopt;

    // The boot window (no OS window mapped yet) adopts the primary — lowest-id — daemon window,
    // so main.qml adopts it instead of authoring a spurious fresh tab on the daemon.
    CHECK(primaryDaemonWindowToAdopt(/*anyWindowMapped=*/false, { 7, 12, 30 }) == 7);

    // A later OS window (one already mapped) is NOT the boot window — it adopts nothing here (it is
    // handled by the staged _pendingAttachWindow path instead).
    CHECK_FALSE(primaryDaemonWindowToAdopt(/*anyWindowMapped=*/true, { 7, 12 }).has_value());

    // No daemon window reported yet: nothing to bind now — the caller still claims the boot window
    // and reconcileAttachWindows binds it once the first layout arrives.
    CHECK_FALSE(primaryDaemonWindowToAdopt(/*anyWindowMapped=*/false, {}).has_value());
}

TEST_CASE("onExit records no exit status for a non-process session", "[contour][app]")
{
    // A MockPty-backed session is neither a local vtpty::Process nor an SSH session: onExit must
    // take the fallthrough (no exit status recorded) without crashing — the path every display-less
    // test session hits at teardown.
    auto factoryOwned = std::make_unique<contour::test::MockPtySessionFactory>();
    auto* factory = factoryOwned.get();
    TestApp app(std::move(factoryOwned));
    auto session = std::make_unique<contour::TerminalSession>(
        &app.manager(), factory->createPty(std::nullopt), app.app());
    CHECK_NOTHROW(app.app().onExit(*session));
}

// Regression (background layout pane aborts at startup): the status line reserves the bottom row, so a
// session's child PTY must be born at the terminal's MAIN-display size (total minus the status line),
// never the full total. A display-less pane (e.g. a background layout tab) never gets the display-attach
// resizeScreen() that would correct a too-tall winsize, and the construction-time reconcile
// (setStatusDisplay -> resizeScreen) early-returns because the PTY master fd is not open yet. Left at the
// total, the child reads one row too many, sets a full-height DECSTBM scroll region, and trips the
// margin<=main-display invariant in the backend. childPtyPageSize() (used by AppSessionFactory::createPty)
// computes the correct birth size.
TEST_CASE("childPtyPageSize reserves the status-line row(s) from the total page size", "[contour][session]")
{
    using vtbackend::ColumnCount;
    using vtbackend::LineCount;
    using vtbackend::PageSize;
    using vtbackend::StatusDisplayType;

    // With a status line the child PTY is born one row shorter than the total (the reserved status row).
    CHECK(contour::childPtyPageSize(PageSize { LineCount(25), ColumnCount(80) }, StatusDisplayType::Indicator)
          == PageSize { LineCount(24), ColumnCount(80) });
    CHECK(contour::childPtyPageSize(PageSize { LineCount(25), ColumnCount(80) },
                                    StatusDisplayType::HostWritable)
          == PageSize { LineCount(24), ColumnCount(80) });
    // No status line: the child uses the full total, unchanged.
    CHECK(contour::childPtyPageSize(PageSize { LineCount(25), ColumnCount(80) }, StatusDisplayType::None)
          == PageSize { LineCount(25), ColumnCount(80) });
    // Columns are never touched by the status line.
    CHECK(
        contour::childPtyPageSize(PageSize { LineCount(40), ColumnCount(120) }, StatusDisplayType::Indicator)
            .columns
        == ColumnCount(120));
    // A degenerate 1-line total must clamp, not underflow to 0.
    CHECK(contour::childPtyPageSize(PageSize { LineCount(1), ColumnCount(80) }, StatusDisplayType::Indicator)
          == PageSize { LineCount(1), ColumnCount(80) });
}

// Regression (SSH profile silently opening a LOCAL shell): AppSessionFactory::createPty skips the
// profile's SshSession only when the session genuinely overrides the shell PROGRAM. A layout
// pane that sets only `directory:` engages the command override with an EMPTY program — it still
// runs the profile's shell and must keep the SSH invariant, or the user ends up typing on the
// wrong host. The gate is the pure overridesShellProgram() so it is testable without libssh2.
TEST_CASE("overridesShellProgram only counts a real program override", "[contour][session]")
{
    using vtpty::Process;

    CHECK_FALSE(contour::overridesShellProgram(std::nullopt));

    // Directory-only layout pane: engaged override, empty program -> NOT a program override.
    auto directoryOnly = Process::ExecInfo {};
    directoryOnly.workingDirectory = "/tmp";
    CHECK_FALSE(contour::overridesShellProgram(directoryOnly));

    auto command = Process::ExecInfo {};
    command.program = "htop";
    CHECK(contour::overridesShellProgram(command));
}
