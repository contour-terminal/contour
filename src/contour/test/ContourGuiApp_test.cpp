// SPDX-License-Identifier: Apache-2.0
//
// Tests for the headless-reachable surface of ContourGuiApp: the parameter/config accessors the
// GUI boot path and TerminalSession read (profile resolution, early-exit threshold, dump-state
// path, resource resolution). The event-loop-driven paths (run/terminalGuiAction) are covered by
// the offscreen e2e app runs, not here.

#include <contour/ContourGuiApp.hpp>
#include <contour/session/SessionFactory.hpp>
#include <contour/session/TerminalSession.hpp>
#include <contour/test/GuiTestFixtures.hpp>

#include <QtCore/QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
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

// The QML override seam moved: the loader used to probe `<configHome>/ui/<Name>.qml` file by file,
// and the module that replaced it is overridden by a `<configHome>/Contour/Ui` module instead. A user
// who had customized a component would otherwise watch it silently stop applying, so the boot path
// detects the stranded layout and says so — which only means anything if it detects it correctly.
TEST_CASE("hasStrandedQmlOverrides only fires on a customization that has actually been lost",
          "[contour][app]")
{
    QTemporaryDir const home;
    REQUIRE(home.isValid());
    auto const configHome = std::filesystem::path(home.path().toStdString());

    SECTION("a config directory with no overrides at all")
    {
        CHECK_FALSE(contour::hasStrandedQmlOverrides(configHome));
    }

    SECTION("an empty legacy directory is not a lost customization")
    {
        // Left behind by an uninstall, or created and never used. Warning here would train the user
        // to ignore the message.
        std::filesystem::create_directories(configHome / "ui");
        CHECK_FALSE(contour::hasStrandedQmlOverrides(configHome));

        // A non-QML file there is not one either.
        std::ofstream(configHome / "ui" / "notes.txt") << "hello";
        CHECK_FALSE(contour::hasStrandedQmlOverrides(configHome));
    }

    SECTION("a legacy .qml file with no module override IS the case worth warning about")
    {
        std::filesystem::create_directories(configHome / "ui");
        std::ofstream(configHome / "ui" / "TabItem.qml") << "import QtQuick\nItem {}\n";
        CHECK(contour::hasStrandedQmlOverrides(configHome));
    }

    SECTION("a migrated module silences it, whatever is left in the old directory")
    {
        std::filesystem::create_directories(configHome / "ui");
        std::ofstream(configHome / "ui" / "TabItem.qml") << "import QtQuick\nItem {}\n";

        auto const moduleDirectory = contour::uiModuleOverrideDirectory(configHome);
        std::filesystem::create_directories(moduleDirectory);
        std::ofstream(moduleDirectory / "qmldir") << "module Contour.Ui\n";
        CHECK_FALSE(contour::hasStrandedQmlOverrides(configHome));
    }
}

TEST_CASE("the attach boot window adopts the primary daemon window", "[contour][app][attach]")
{
    using contour::primaryDaemonWindowToAdopt;

    // The boot window (no OS window mapped yet) adopts the primary — lowest-id — daemon window,
    // so Main.qml adopts it instead of authoring a spurious fresh tab on the daemon.
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
    auto session = std::make_unique<contour::session::TerminalSession>(
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
    CHECK(contour::session::childPtyPageSize(PageSize { LineCount(25), ColumnCount(80) },
                                             StatusDisplayType::Indicator)
          == PageSize { LineCount(24), ColumnCount(80) });
    CHECK(contour::session::childPtyPageSize(PageSize { LineCount(25), ColumnCount(80) },
                                             StatusDisplayType::HostWritable)
          == PageSize { LineCount(24), ColumnCount(80) });
    // No status line: the child uses the full total, unchanged.
    CHECK(contour::session::childPtyPageSize(PageSize { LineCount(25), ColumnCount(80) },
                                             StatusDisplayType::None)
          == PageSize { LineCount(25), ColumnCount(80) });
    // Columns are never touched by the status line.
    CHECK(contour::session::childPtyPageSize(PageSize { LineCount(40), ColumnCount(120) },
                                             StatusDisplayType::Indicator)
              .columns
          == ColumnCount(120));
    // A degenerate 1-line total must clamp, not underflow to 0.
    CHECK(contour::session::childPtyPageSize(PageSize { LineCount(1), ColumnCount(80) },
                                             StatusDisplayType::Indicator)
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

    CHECK_FALSE(contour::session::overridesShellProgram(std::nullopt));

    // Directory-only layout pane: engaged override, empty program -> NOT a program override.
    auto directoryOnly = Process::ExecInfo {};
    directoryOnly.workingDirectory = "/tmp";
    CHECK_FALSE(contour::session::overridesShellProgram(directoryOnly));

    auto command = Process::ExecInfo {};
    command.program = "htop";
    CHECK(contour::session::overridesShellProgram(command));
}
