// SPDX-License-Identifier: Apache-2.0
//
// Offscreen manager-layer regression pins for the "new tab / split inherits the live window size"
// fix. The bug: after the user resized the window, a NEW tab spawned its child PTY at the profile's
// configured terminalSize (default 80x25) instead of the currently-running window size, so the shell
// saw a stale grid.
//
// The size a new session's child PTY is spawned at is decided in
// TerminalSessionManager::createSessionInBackground / splitActivePane (via
// contour::geometry::initialPageSize): a tab/split in an EXISTING window inherits the active pane's
// running page size; a brand-new window (no active pane to inherit from) uses the profile default.
// These tests drive the REAL manager headlessly (TestApp + MockPtySessionFactory records the page
// size each createPty() was asked for) and assert exactly that decision — no display, no PTY spawn.
// The pure decision itself is unit-tested in WindowGeometry_test; the end-to-end grid refit on a live
// display is pinned by the [display] case in DisplayRendering_test.

#include <contour/ContourGuiApp.h>
#include <contour/TerminalSession.h>
#include <contour/TerminalSessionManager.h>
#include <contour/WindowController.h>
#include <contour/test/GuiTestFixtures.h>

#include <catch2/catch_test_macros.hpp>

#include <mutex>

#include <vtmux/SessionModel.h>

namespace
{

using contour::test::MockPtySessionFactory;
using contour::test::ScopedController;
using contour::test::TestApp;

/// Builds a TestApp whose sessions are backed by a recording MockPtySessionFactory, keeping a raw
/// observation pointer to the factory so a test can read the requested page sizes.
struct ManagerFixture
{
    MockPtySessionFactory* factory = nullptr;
    TestApp app;

    ManagerFixture(): app(makeFactory()) {}

    /// Grows (or shrinks) a running session's grid to simulate the user having resized the window,
    /// so the NEXT tab created in that window inherits this size.
    static void resizeSessionGrid(contour::TerminalSession& session, vtbackend::PageSize to)
    {
        auto const _ = std::scoped_lock { session.terminal() };
        session.terminal().resizeScreen(to, std::nullopt);
    }

  private:
    std::unique_ptr<contour::SessionFactory> makeFactory()
    {
        auto owned = std::make_unique<MockPtySessionFactory>();
        factory = owned.get();
        return owned;
    }
};

} // namespace

TEST_CASE("a new tab inherits the live window's resized grid, not the profile default",
          "[contour][sizing][tabinherit]")
{
    ManagerFixture fixture;
    auto& manager = fixture.app.manager();
    ScopedController window { manager };

    // First tab of a brand-new window: no running pane to inherit from -> the profile default. (The
    // manager always resolves a concrete size via geometry::initialPageSize, so the request carries the
    // profile default rather than nullopt; a nullopt request is only what a non-inheriting caller sends.)
    auto* first = manager.createSession(window.id);
    REQUIRE(first != nullptr);
    REQUIRE(fixture.factory->requestedPageSizes.size() == 1);
    REQUIRE(fixture.factory->requestedPageSizes.front().has_value());
    CHECK(*fixture.factory->requestedPageSizes.front() == MockPtySessionFactory::DefaultPageSize);
    // The first session's grid is the profile default.
    CHECK(first->terminal().totalPageSize() == MockPtySessionFactory::DefaultPageSize);

    // The user resizes the window: the running session's grid grows to 40 lines x 200 columns.
    auto const resized = vtbackend::PageSize { vtbackend::LineCount(40), vtbackend::ColumnCount(200) };
    ManagerFixture::resizeSessionGrid(*first, resized);
    REQUIRE(first->terminal().totalPageSize() == resized);

    // A NEW tab must adopt the resized grid, not the 80x25 profile default (this is the bug).
    auto* second = manager.createSession(window.id);
    REQUIRE(second != nullptr);
    REQUIRE(fixture.factory->requestedPageSizes.size() == 2);
    REQUIRE(fixture.factory->requestedPageSizes.back().has_value());
    CHECK(*fixture.factory->requestedPageSizes.back() == resized);
    // The terminal grid is BORN at the inherited size (not just corrected once a display attaches — a
    // background tab that never attaches must still hold the right size).
    CHECK(second->terminal().totalPageSize() == resized);
    // The child PTY tracks the terminal's PTY-facing MAIN-page size (total minus the status line, the
    // rows the shell actually draws into) — i.e. it followed the inherited grid, not the 80x25 default.
    REQUIRE(fixture.factory->createdPtys.size() == 2);
    CHECK(fixture.factory->createdPtys.back()->pageSize() == second->terminal().pageSize());
    CHECK(second->terminal().pageSize().columns == resized.columns);
}

TEST_CASE("a new split pane inherits the active pane's running grid", "[contour][sizing][tabinherit]")
{
    ManagerFixture fixture;
    auto& manager = fixture.app.manager();
    ScopedController window { manager };

    auto* base = manager.createSession(window.id);
    REQUIRE(base != nullptr);

    auto const resized = vtbackend::PageSize { vtbackend::LineCount(50), vtbackend::ColumnCount(160) };
    ManagerFixture::resizeSessionGrid(*base, resized);

    // Splitting always happens inside a live window, so the new pane inherits the running grid. (The
    // split then subdivides that area in the display layer; the point here is the SPAWN size is the
    // live window size, never the profile default.)
    manager.splitActivePane(/*vertical*/ true, base);
    REQUIRE(fixture.factory->requestedPageSizes.size() == 2);
    REQUIRE(fixture.factory->requestedPageSizes.back().has_value());
    CHECK(*fixture.factory->requestedPageSizes.back() == resized);
}

TEST_CASE("the first tab of each fresh window uses the profile default", "[contour][sizing][tabinherit]")
{
    // The constraint's other half: the profile's configured terminalSize is enforced ONLY for a
    // newly-spawned window. Two independent windows each spawn their first tab with no running pane to
    // inherit from, so both are born at the profile default even though window A's grid could be
    // grown afterwards.
    ManagerFixture fixture;
    auto& manager = fixture.app.manager();
    ScopedController windowA { manager };
    ScopedController windowB { manager };

    auto* a = manager.createSession(windowA.id);
    auto* b = manager.createSession(windowB.id);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);

    REQUIRE(fixture.factory->requestedPageSizes.size() == 2);
    REQUIRE(fixture.factory->requestedPageSizes[0].has_value());
    REQUIRE(fixture.factory->requestedPageSizes[1].has_value());
    CHECK(*fixture.factory->requestedPageSizes[0] == MockPtySessionFactory::DefaultPageSize);
    CHECK(*fixture.factory->requestedPageSizes[1] == MockPtySessionFactory::DefaultPageSize);
    CHECK(a->terminal().totalPageSize() == MockPtySessionFactory::DefaultPageSize);
    CHECK(b->terminal().totalPageSize() == MockPtySessionFactory::DefaultPageSize);
}
