// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Test doubles for the native menu presenter.

#include <contour/command/MenuTree.hpp>
#include <contour/platform/NativeMenuPresenter.hpp>

#include <cstddef>
#include <expected>
#include <vector>

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

} // namespace contour::test
