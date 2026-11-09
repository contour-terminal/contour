// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/session/SessionFactory.hpp>
#include <contour/session/WindowSwitching.hpp>

#include <optional>
#include <string_view>

#include <vtworkspace/Primitives.hpp>

namespace contour::window
{
class DaemonDriftModel;
}

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

    /// @return What a window bound here says about the daemon it is on, in its title and its chip: the remote
    ///         host, or the label of a local daemon that is not `default`; empty when there is nothing to say
    ///         (the default local daemon, a binding with no daemon). Unlike hostLabel() it names local
    ///         daemons too, so it is for the chrome only, never for deciding where a session runs.
    [[nodiscard]] virtual std::string_view chromeLabel() const noexcept { return hostLabel(); }

    /// @return The daemon the binding is attached to, as a switch names it: a local label or a saved host;
    ///         SwitchDaemonKind::Same (with no name) for a daemon no switch can name (`--connect-tcp`), whose
    ///         own workspaces a window can still switch between. None for a binding with no daemon behind it
    ///         -- a factory binding, as a tmux window's -- which offers no switching at all.
    [[nodiscard]] virtual std::optional<SwitchTarget> daemonTarget() const { return std::nullopt; }

    /// @return What the drift badge of a window bound here shows: whether its daemon runs another release
    /// than
    ///         this client; null for a binding with no daemon to drift. GUI thread.
    [[nodiscard]] virtual window::DaemonDriftModel* daemonDrift() noexcept = 0;

    /// Tells the binding that @p window, which it served, was closed: what it showed is no longer shown here
    /// (a daemon is told so, and keeps the workspace as left). Told before lastWindowReleased() when it was
    /// the last; not told for a window bound elsewhere, whose rebinding says what became of it. GUI thread.
    /// @param window The closed window.
    virtual void windowClosed(vtworkspace::WorkspaceId window) = 0;

    /// Tells the binding that the last window it served is gone: closed, or bound elsewhere
    /// (TerminalSessionManager::bindWindow, TerminalSessionManager::removeWindowController). Not told when
    /// the binding releases its windows itself (TerminalSessionManager::releaseBinding). Which windows it
    /// serves is the manager's to know (TerminalSessionManager::windowsBoundTo). GUI thread.
    virtual void lastWindowReleased() = 0;
};

} // namespace contour::session
