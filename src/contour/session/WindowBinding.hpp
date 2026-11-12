// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/session/SessionFactory.hpp>
#include <contour/session/WindowSwitching.hpp>
#include <contour/window/DaemonDriftChoice.hpp>
#include <contour/window/WorkspaceDirectorySource.hpp>

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include <vthost/proto/Pdu.hpp>
#include <vtworkspace/Primitives.hpp>

class QScreen;

namespace contour::window
{
class DaemonDriftModel;
}

namespace contour::session
{

/// Why UpgradeDaemon does nothing in a window that has no daemon behind it.
inline constexpr std::string_view NoDaemonToUpgrade = "this window has no daemon to upgrade";

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

    /// @return Whether the daemon behind the binding runs on this machine. Local for a binding with no
    /// daemon.
    [[nodiscard]] virtual window::DaemonLocality daemonLocality() const noexcept
    {
        return window::DaemonLocality::Local;
    }

    /// @return What the drift badge of a window bound here shows: whether its daemon runs another release
    /// than
    ///         this client; null for a binding with no daemon to drift. GUI thread.
    [[nodiscard]] virtual window::DaemonDriftModel* daemonDrift() noexcept = 0;

    /// Upgrades the daemon behind the windows bound here in place, because the user commanded it (the
    /// `UpgradeDaemon` action), whether or not its release drifts. Returns at once: the upgrade runs on, and
    /// its outcome is told to the windows. GUI thread.
    /// @return The reason it did not start, in words for the user -- a daemon that is not this client's to
    ///         upgrade, an upgrade already under way -- or none. A binding with no daemon refuses.
    [[nodiscard]] virtual std::expected<void, std::string> upgradeDaemonOnCommand()
    {
        return std::unexpected(std::string { NoDaemonToUpgrade });
    }

    /// Tells the binding that @p window, which it served, was closed: what it showed is no longer shown here
    /// (a daemon's workspace runs on in the daemon). Told before lastWindowReleased() when it was
    /// the last; not told for a window bound elsewhere, whose rebinding says what became of it. GUI thread.
    /// @param window The closed window.
    virtual void windowClosed(vtworkspace::WorkspaceId window) = 0;

    /// Tells the binding that the last window it served is gone: closed, or bound elsewhere
    /// (TerminalSessionManager::bindWindow, TerminalSessionManager::removeWindowController). Not told when
    /// the binding releases its windows itself (TerminalSessionManager::releaseBinding). Which windows it
    /// serves is the manager's to know (TerminalSessionManager::windowsBoundTo). GUI thread.
    virtual void lastWindowReleased() = 0;

    /// Tells the daemon behind the binding whether the window bound here has the user's focus: of every
    /// window on a daemon, the one focused most recently raises (raiseRole()). Nothing for a binding with no
    /// daemon. GUI thread.
    /// @param focus Whether the window gained or lost the focus.
    virtual void reportFocus(vthost::proto::WindowFocus /*focus*/) {}

    /// @return Whether a window bound here rings, alerts and notifies for its panes, or only marks them: the
    ///         daemon names one window of all those on it, so that a bell sounds once. Raise for a binding
    ///         with no daemon, whose window is the only one that sees its panes. GUI thread.
    [[nodiscard]] virtual vthost::proto::RaiseRole raiseRole() const noexcept
    {
        return vthost::proto::RaiseRole::Raise;
    }

    /// @return How many tabs wait for the user in the daemon's workspaces the window bound here does not
    ///         display; none for a binding with no daemon, whose window displays all it has. GUI thread.
    [[nodiscard]] virtual unsigned tabsWaitingElsewhere() const { return 0; }

    /// @return Where a window bound here lists the workspaces it can show, which a binding with a daemon (or
    ///         other host of workspaces) states; null for a binding with none, whose window lists its own
    ///         process's (InProcessDirectory). The binding owns it. GUI thread.
    [[nodiscard]] virtual window::IWorkspaceDirectorySource* directorySource() noexcept { return nullptr; }

    /// @return What every binding to the same daemon shares, so that a count over windows takes that daemon
    ///         once; none for a binding with no daemon. GUI thread.
    [[nodiscard]] virtual std::optional<uint64_t> daemonKey() const { return std::nullopt; }

    /// Opens another window on the daemon behind the binding, selecting the workspace @p window displays:
    /// New Window, or a tab torn off @p window, which the new window shows as its active tab. Nothing moves:
    /// both windows show the daemon's workspace. GUI thread.
    /// @param window The window bound here that asks.
    /// @param activeTab The tab of @p window the new window activates once it shows the workspace; none for
    ///        the daemon's own choice.
    /// @param screen Where the window opens; null for the default placement.
    /// @return Nothing once the window's connection is under way; why not, for a binding with no daemon.
    [[nodiscard]] virtual std::expected<void, std::string> openWindowLike(
        vtworkspace::WorkspaceId /*window*/,
        std::optional<vtworkspace::TabId> /*activeTab*/,
        QScreen* /*screen*/)
    {
        return std::unexpected(std::string { "this window has no daemon to open another window on" });
    }
};

} // namespace contour::session
