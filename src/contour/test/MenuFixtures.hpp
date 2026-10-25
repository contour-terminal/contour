// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Test doubles for the native menu presenter and the menu bar integration, and the helpers that pick a
/// menu bar row by its title.

#include <contour/command/MenuTree.hpp>
#include <contour/platform/MenuBarIntegration.hpp>
#include <contour/platform/NativeMenuPresenter.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <expected>
#include <ranges>
#include <span>
#include <string_view>
#include <vector>

#include <vtworkspace/Primitives.hpp>

class QMenu;

namespace contour::test
{

/// A presenter that records what it is asked to show and answers with a result the test chooses.
class RecordingMenuPresenter final: public platform::INativeMenuPresenter
{
  public:
    std::expected<void, platform::MenuPresentError> result {}; ///< What present() returns.
    std::vector<command::MenuTree> trees {};                   ///< Every tree present() was handed.
    std::vector<platform::MenuAnchor> anchors {};              ///< Every anchor present() was handed.
    platform::MenuPicked lastPicked {};                        ///< The callback of the last call.

    /// Records the call.
    /// @return @ref result.
    [[nodiscard]] std::expected<void, platform::MenuPresentError> present(
        command::MenuTree const& tree,
        platform::MenuAnchor const& anchor,
        platform::MenuPicked const& onPicked) override
    {
        trees.push_back(tree);
        anchors.push_back(anchor);
        lastPicked = onPicked;
        return result;
    }

    /// Picks a row as the user would, by calling the last callback.
    /// @param index The action index to report.
    void pick(std::size_t index) const { lastPicked(index); }
};

/// A menu bar integration that records the menus it is handed and hands them to nobody.
class RecordingMenuBarIntegration final: public platform::IMenuBarIntegration
{
  public:
    /// The menus of one adopt() call.
    struct Adoption
    {
        QMenu* windows = nullptr; ///< The Window menu handed over; not owned.
        QMenu* help = nullptr;    ///< The Help menu handed over; not owned.
    };

    std::vector<Adoption> adoptions {}; ///< Every adopt() call, in order.

    /// Records the call.
    void adopt(QMenu& windows, QMenu& help) override
    {
        adoptions.push_back({ .windows = &windows, .help = &help });
    }
};

/// Tabs that each have a real (mock-PTY) session: their ids and session ids in row order.
struct SessionTabs
{
    std::vector<vtworkspace::TabId> tabs {};
    std::vector<vtworkspace::SessionId> sessions {};
};

/// Opens @p n tabs in @p controller's window, each with a session of its own.
///
/// A template over the manager and the controller, as runMenuBarRow() is over the app, so that this header
/// does not bring the frontend's headers to the menu tests that build neither.
/// @param manager    The session manager owning @p controller (a session::TerminalSessionManager).
/// @param controller The window to open the tabs in (a window::WindowController).
/// @param n          How many tabs to open.
/// @return The tabs and their sessions, in row order.
template <typename Manager, typename Controller>
[[nodiscard]] SessionTabs createSessionTabs(Manager& manager, Controller& controller, int n)
{
    auto result = SessionTabs {};
    for (auto const _: std::views::iota(0, n))
    {
        controller.createNewTab();
        auto const* tab = manager.model().window(controller.windowId())->activeTab();
        REQUIRE(tab != nullptr);
        REQUIRE(controller.activeSession() != nullptr);
        result.tabs.push_back(tab->id());
        result.sessions.push_back(controller.activeSession()->modelSessionId());
    }
    return result;
}

/// The node titled @p title in @p nodes, searching submenus too.
/// @param nodes The nodes to search.
/// @param title The title to find.
/// @return The node, or nullptr if there is none.
[[nodiscard]] inline command::MenuNode const* nodeTitled(std::span<command::MenuNode const> nodes,
                                                         std::string_view title)
{
    for (auto const& node: nodes)
    {
        if (node.title == title)
            return &node;
        if (auto const* nested = nodeTitled(node.children, title))
            return nested;
    }
    return nullptr;
}

/// Runs the menu bar row titled @p title of @p tree through the app's own menu bar path, at the row's scope.
/// @param app   The test app (a test::TestApp) whose app() runs the row.
/// @param tree  The menu bar's tree.
/// @param title The row's title; the row must exist.
template <typename App>
void runMenuBarRow(App& app, command::MenuTree const& tree, std::string_view title)
{
    auto const* row = nodeTitled(tree.nodes, title);
    REQUIRE(row != nullptr);
    app.app().runMenuBarAction(tree.actions[row->actionIndex], row->scope);
}

} // namespace contour::test
