// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <memory>

class QMenu;

namespace contour::platform
{

/// Hands the menu bar's Window and Help menus to the operating system.
class IMenuBarIntegration
{
  public:
    virtual ~IMenuBarIntegration() = default;

    /// Makes @p windows the menu the OS lists its windows in, and @p help the one it adds its help
    /// search to. Called once per menu bar, with menus that outlive this call.
    /// @pre GUI (main) thread; on macOS, a native QMenuBar holds both menus.
    /// @param windows The menu bar's Window menu.
    /// @param help The menu bar's Help menu.
    virtual void adopt(QMenu& windows, QMenu& help) = 0;
};

/// The integration for platforms whose OS adopts no menus: adopt() does nothing.
class NullMenuBarIntegration final: public IMenuBarIntegration
{
  public:
    /// Does nothing.
    void adopt(QMenu& /*windows*/, QMenu& /*help*/) override {}
};

/// Creates the operating system's menu bar integration, or the null one where there is none.
/// @return The integration.
[[nodiscard]] std::unique_ptr<IMenuBarIntegration> makeMenuBarIntegration();

} // namespace contour::platform
