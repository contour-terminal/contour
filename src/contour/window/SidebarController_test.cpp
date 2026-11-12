// SPDX-License-Identifier: Apache-2.0
//
// The controller's half of the workspace sidebar: what a window starts with (the configured defaults, or what
// the user last chose), what it declares as horizontal chrome, and what a change writes back to the GUI
// state.

#include <contour/config/GuiStateStore.hpp>
#include <contour/config/TabBarMode.hpp>
#include <contour/geometry/SidebarGeometry.hpp>
#include <contour/test/AppFixture.hpp>
#include <contour/test/CoreFixtures.hpp>
#include <contour/test/GuiTestFixtures.hpp>
#include <contour/window/TitleBandLayout.hpp>
#include <contour/window/WindowController.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>

#include <QtTest/QSignalSpy>

namespace
{

using contour::config::SidebarPrefs;
using contour::config::SidebarVisibility;
using contour::config::TabPlacement;
using contour::window::BarCentre;

/// Gives @p app the sidebar the Native2026 edition ships: shown, with the tabs in it. The test app itself
/// runs the Classic defaults.
void useNativeSidebarDefaults(contour::test::TestApp& app)
{
    app.app().config().sidebar = contour::config::SidebarConfig { .visibility = SidebarVisibility::Shown };
    app.app().config().tabPlacement = TabPlacement::Sidebar;
}

/// @return A sidebar's visibility as the controller publishes it to QML.
[[nodiscard]] int asQml(SidebarVisibility visibility) noexcept
{
    return static_cast<int>(visibility);
}

} // namespace

TEST_CASE("a window starts with the configured sidebar when nothing was chosen",
          "[contour][sidebar][controller]")
{
    auto app = contour::test::TestApp {};
    auto const metrics = app.app().sidebarMetrics();

    SECTION("the Classic defaults hide it and keep the tabs on top")
    {
        auto window = contour::test::ScopedController { app.manager() };
        CHECK(window.controller->sidebarVisibility() == asQml(SidebarVisibility::Hidden));
        CHECK(window.controller->tabPlacement() == static_cast<int>(TabPlacement::Top));
        CHECK(window.controller->sidebarWidth() == 0);
        CHECK(window.controller->titleBand().centre == BarCentre::Tabs);
    }

    useNativeSidebarDefaults(app);
    auto window = contour::test::ScopedController { app.manager() };
    auto& controller = *window.controller;

    // The newest edition's defaults: the sidebar shown, the tabs in it.
    CHECK(controller.sidebarVisibility() == asQml(SidebarVisibility::Shown));
    CHECK(controller.tabPlacement() == static_cast<int>(TabPlacement::Sidebar));
    CHECK(controller.sidebarWidth() == metrics.tokens.width);
    CHECK(controller.titleBand().centre == BarCentre::Breadcrumb);
}

TEST_CASE("GuiState sidebar prefs win over the config default", "[contour][sidebar][controller]")
{
    auto app = contour::test::TestApp {};
    useNativeSidebarDefaults(app);
    auto const metrics = app.app().sidebarMetrics();
    auto const remembered = metrics.tokens.minWidth + 11;
    app.guiState().state.sidebar = SidebarPrefs { .visibility = SidebarVisibility::Hidden,
                                                  .widthPx = remembered,
                                                  .widthCells = std::nullopt };
    auto window = contour::test::ScopedController { app.manager() };
    auto& controller = *window.controller;

    CHECK(controller.sidebarVisibility() == asQml(SidebarVisibility::Hidden));
    // Placement is configuration only: what was remembered does not touch it.
    CHECK(controller.tabPlacement() == static_cast<int>(TabPlacement::Sidebar));
    // Hidden, the sidebar takes no width...
    CHECK(controller.sidebarWidth() == 0);
    // ...and shown again it takes the width the user left it at, not the row's own.
    controller.toggleSidebar();
    CHECK(controller.sidebarWidth() == remembered);
}

TEST_CASE("chrome width is the sidebar width when shown, 0 when hidden (window)",
          "[contour][sidebar][controller]")
{
    auto app = contour::test::TestApp {};
    useNativeSidebarDefaults(app);
    auto window = contour::test::ScopedController { app.manager() };
    auto& controller = *window.controller;
    auto widthChanges = QSignalSpy { &controller, &contour::window::WindowController::sidebarWidthChanged };
    auto const shown = controller.sidebarWidth();
    REQUIRE(shown > 0);

    controller.toggleSidebar();
    CHECK(controller.sidebarVisibility() == asQml(SidebarVisibility::Hidden));
    CHECK(controller.sidebarWidth() == 0);
    CHECK(controller.titleBand().centre == BarCentre::ChipAndTabs);

    controller.toggleSidebar();
    CHECK(controller.sidebarWidth() == shown);
    CHECK(widthChanges.count() == 2);
}

TEST_CASE("a sidebar change is remembered", "[contour][sidebar][controller]")
{
    auto app = contour::test::TestApp {};
    useNativeSidebarDefaults(app);
    auto const metrics = app.app().sidebarMetrics();
    auto window = contour::test::ScopedController { app.manager() };
    auto& controller = *window.controller;
    REQUIRE_FALSE(app.guiState().state.sidebar.has_value());

    SECTION("a width is clamped to the row's bounds, then written")
    {
        controller.setSidebarWidth(100000);
        CHECK(controller.sidebarWidth() == metrics.tokens.maxWidth);
        REQUIRE(app.guiState().state.sidebar.has_value());
        CHECK(app.guiState().state.sidebar->widthPx == metrics.tokens.maxWidth);
        CHECK(app.guiState().state.sidebar->visibility == SidebarVisibility::Shown);

        controller.setSidebarWidth(1);
        CHECK(controller.sidebarWidth() == metrics.tokens.minWidth);
        CHECK(app.guiState().state.sidebar->widthPx == metrics.tokens.minWidth);
    }

    SECTION("a width that changes nothing writes nothing")
    {
        auto const saves = app.guiState().saves;
        controller.setSidebarWidth(controller.sidebarWidth());
        CHECK(app.guiState().saves == saves);
    }

    SECTION("a visibility is written, and the next window starts with it")
    {
        controller.toggleSidebar();
        REQUIRE(app.guiState().state.sidebar.has_value());
        CHECK(app.guiState().state.sidebar->visibility == SidebarVisibility::Hidden);

        auto next = contour::test::ScopedController { app.manager() };
        CHECK(next.controller->sidebarVisibility() == asQml(SidebarVisibility::Hidden));
    }
}

TEST_CASE("a config reload changes an open window's tab placement", "[contour][sidebar][controller]")
{
    using contour::test::ConfiguredTestApp;
    auto app = ConfiguredTestApp {};
    auto window = contour::test::ScopedController { app.manager() };
    auto& controller = *window.controller;
    REQUIRE(controller.tabPlacement() == static_cast<int>(TabPlacement::Top));
    auto changes = QSignalSpy { &controller, &contour::window::WindowController::tabPlacementChanged };

    contour::test::writeConfigFile(
        app.configFile(),
        contour::test::profileConfigYaml(app.app().profileName(), "tab_placement: sidebar\n"));
    REQUIRE(app.manager().reloadAllSessions());
    CHECK(controller.tabPlacement() == static_cast<int>(TabPlacement::Sidebar));
    CHECK(changes.count() == 1);

    // The same placement again says nothing, and nothing of placement was remembered.
    REQUIRE(app.manager().reloadAllSessions());
    CHECK(changes.count() == 1);
}

TEST_CASE("a Terminal-row sidebar width is remembered in cells, not pixels", "[contour][sidebar][geometry]")
{
    using contour::config::LengthUnit;
    auto prefs = SidebarPrefs { .visibility = SidebarVisibility::Shown, .widthPx = 260 };
    contour::geometry::rememberSidebarUnits(prefs, LengthUnit::Cell, 30);
    CHECK(prefs.widthCells == 30);
    CHECK(prefs.widthPx == 260);
    CHECK(contour::geometry::rememberedSidebarUnits(prefs, LengthUnit::Cell) == 30);
    CHECK(contour::geometry::rememberedSidebarUnits(prefs, LengthUnit::Pixel) == 260);

    // The same on a pixel row leaves the cells alone.
    contour::geometry::rememberSidebarUnits(prefs, LengthUnit::Pixel, 300);
    CHECK(prefs.widthPx == 300);
    CHECK(prefs.widthCells == 30);
}
