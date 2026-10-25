// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/command/MenuTree.hpp>

#include <QtCore/QPointF>
#include <QtGui/QWindow>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>

namespace contour::platform
{

/// Why a menu was not shown natively; the caller then shows it in the scene.
enum class MenuPresentError : uint8_t
{
    Unsupported = 0, ///< This platform has no native menu presenter.
    NoWindow,        ///< The anchor names a position but no window to place it in.
};

/// Where a menu opens: @ref position in @ref window's logical coordinates, or at the pointer when absent.
struct MenuAnchor
{
    QWindow* window = nullptr;          ///< The window the menu belongs to; not owned.
    std::optional<QPointF> position {}; ///< Where in @ref window the menu opens; the pointer when absent.
};

/// Called with the picked command row's MenuNode::actionIndex; not called when the menu is dismissed.
using MenuPicked = std::function<void(std::size_t actionIndex)>;

/// Shows a resolved menu with the operating system's own menu machinery.
class INativeMenuPresenter
{
  public:
    virtual ~INativeMenuPresenter() = default;

    /// Shows @p tree as a native menu at @p anchor.
    /// @pre GUI (main) thread. On macOS this blocks in AppKit's menu tracking loop until the menu closes,
    ///      and calls @p onPicked after that loop returns, never inside it.
    /// @param tree The menu to show.
    /// @param anchor Where to open it.
    /// @param onPicked Called with the picked row's action index; not called on dismissal.
    /// @return Nothing when the menu was shown, otherwise why it was not.
    [[nodiscard]] virtual std::expected<void, MenuPresentError> present(command::MenuTree const& tree,
                                                                        MenuAnchor const& anchor,
                                                                        MenuPicked const& onPicked) = 0;
};

/// The presenter for platforms without native menus: every request is Unsupported.
class NullNativeMenuPresenter final: public INativeMenuPresenter
{
  public:
    /// @return Always MenuPresentError::Unsupported; @p onPicked is never called.
    [[nodiscard]] std::expected<void, MenuPresentError> present(command::MenuTree const& /*tree*/,
                                                                MenuAnchor const& /*anchor*/,
                                                                MenuPicked const& /*onPicked*/) override
    {
        return std::unexpected(MenuPresentError::Unsupported);
    }
};

/// Creates the native menu presenter, or the null presenter where there is none.
/// @return The presenter.
[[nodiscard]] std::unique_ptr<INativeMenuPresenter> makeNativeMenuPresenter();

} // namespace contour::platform
