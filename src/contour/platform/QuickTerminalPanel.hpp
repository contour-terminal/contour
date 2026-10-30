// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The native side of the quick terminal's window: how it takes the keyboard without taking the
/// application's activation with it. Behind an interface so the controller is tested with a recorder and
/// the Cocoa panel is the only translation unit that talks to AppKit.

#include <memory>
#include <string>

class QWindow;

namespace contour::platform
{

/// What makes a window a quick-terminal panel, and shows and hides it.
///
/// Every call is made on the GUI thread.
class IQuickTerminalPanel
{
  public:
    IQuickTerminalPanel() = default;
    IQuickTerminalPanel(IQuickTerminalPanel const&) = delete;
    IQuickTerminalPanel& operator=(IQuickTerminalPanel const&) = delete;
    IQuickTerminalPanel(IQuickTerminalPanel&&) = delete;
    IQuickTerminalPanel& operator=(IQuickTerminalPanel&&) = delete;
    virtual ~IQuickTerminalPanel() = default;

    /// Prepares a freshly created window to be a panel: on macOS, the non-activating style.
    /// @param window The quick terminal's window, not yet shown.
    /// @pre GUI thread.
    virtual void adopt(QWindow& window) = 0;

    /// Shows @p window and gives it the keyboard.
    /// @param window The adopted window.
    /// @param activationToken The desktop's focus-stealing token (the Linux portal's), empty when there is
    /// none.
    /// @pre GUI thread.
    virtual void present(QWindow& window, std::string const& activationToken) = 0;

    /// Hides @p window.
    /// @param window The adopted window.
    /// @pre GUI thread.
    virtual void dismiss(QWindow& window) = 0;
};

/// The panel of a host with nothing to adopt: every call does nothing.
class NullQuickTerminalPanel final: public IQuickTerminalPanel
{
  public:
    /// Does nothing.
    void adopt(QWindow& /*window*/) override {}

    /// Does nothing.
    void present(QWindow& /*window*/, std::string const& /*activationToken*/) override {}

    /// Does nothing.
    void dismiss(QWindow& /*window*/) override {}
};

/// Creates the operating system's quick-terminal panel.
/// @return Qt's own show, raise and activate off macOS, the AppKit non-activating panel on it.
[[nodiscard]] std::unique_ptr<IQuickTerminalPanel> makeQuickTerminalPanel();

} // namespace contour::platform
