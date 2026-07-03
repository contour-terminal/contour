// SPDX-License-Identifier: Apache-2.0
//
// Tests for the headless-reachable surface of ContourGuiApp: the parameter/config accessors the
// GUI boot path and TerminalSession read (profile resolution, early-exit threshold, dump-state
// path, resource resolution). The event-loop-driven paths (run/terminalGuiAction) are covered by
// the offscreen e2e app runs, not here.

#include <contour/ContourGuiApp.h>
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
