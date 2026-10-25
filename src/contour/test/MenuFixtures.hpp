// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Test doubles for the native menu presenter and the menu bar integration.

#include <contour/command/MenuTree.hpp>
#include <contour/platform/MenuBarIntegration.hpp>
#include <contour/platform/NativeMenuPresenter.hpp>

#include <cstddef>
#include <expected>
#include <vector>

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

} // namespace contour::test
