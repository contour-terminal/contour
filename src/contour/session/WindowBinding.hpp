// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/session/SessionFactory.hpp>

#include <string_view>

namespace contour::session
{

/// What a window's remote sessions are bound to: the daemon connection that hosts them.
///
/// Implemented by remote::DaemonLink and unknown to session/ beyond this interface, so that session/ names
/// the connection a window belongs to without including remote/, which sits above it.
class WindowBinding
{
  public:
    virtual ~WindowBinding() = default;

    WindowBinding() = default;
    WindowBinding(WindowBinding const&) = delete;
    WindowBinding& operator=(WindowBinding const&) = delete;
    WindowBinding(WindowBinding&&) = delete;
    WindowBinding& operator=(WindowBinding&&) = delete;

    /// @return The factory every session of a window bound here is created through. GUI thread.
    [[nodiscard]] virtual SessionFactory& sessions() noexcept = 0;

    /// @return The remote host the binding is attached to, as `contour client --host` named it; empty for a
    ///         local daemon, `--connect-tcp`, and until the attach succeeded.
    [[nodiscard]] virtual std::string_view hostLabel() const noexcept = 0;

    /// Tells the binding that the last window it served is gone: closed, or bound elsewhere
    /// (TerminalSessionManager::bindWindow, TerminalSessionManager::removeWindowController). Not told when
    /// the binding releases its windows itself (TerminalSessionManager::releaseBinding). Which windows it
    /// serves is the manager's to know (TerminalSessionManager::windowsBoundTo). GUI thread.
    virtual void lastWindowReleased() = 0;
};

} // namespace contour::session
