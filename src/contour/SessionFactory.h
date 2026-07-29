// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/Terminal.h>
#include <vtbackend/primitives.h>

#include <vtpty/Process.h>
#include <vtpty/Pty.h>
#ifdef VTPTY_LIBSSH2
    #include <vtpty/SshSession.h>
#endif

#include <memory>
#include <optional>
#include <string>

namespace contour
{

class ContourGuiApp;

/// The winsize a session's child PTY must be born with, given the terminal's TOTAL page size and its
/// status-line type: the status line reserves the bottom row(s), so the child's usable area is the total
/// minus the status line (clamped to at least one line). Birthing the child at the full total leaves a
/// display-less pane (e.g. a background layout tab) reading one row too many — see SessionFactory.cpp.
[[nodiscard]] vtbackend::PageSize childPtyPageSize(vtbackend::PageSize total,
                                                   vtbackend::StatusDisplayType statusLine) noexcept;

/// Creates the PTY backing a terminal session.
///
/// This is the part of session creation that is independent of how sessions are organized into
/// tabs/windows: every code path that needs a new session (a new tab, a new window, or a new split
/// pane) goes through one factory, so the tab/window model can evolve without touching PTY
/// spawning. It is an interface (per the project's dependency-injection principle: PTY creation is
/// process/network I/O), so tests inject an in-memory PTY factory and drive the manager's
/// session-creation paths headlessly.
/// Whether @p commandOverride overrides the shell PROGRAM a session runs. A directory-only layout
/// pane engages the override with an EMPTY program (only the working directory is set), which must
/// keep every program-dependent profile behavior — most critically the "an SSH-configured profile
/// opens an SshSession" invariant (see AppSessionFactory::createPty). Pure and dependency-free so
/// the SSH gate is testable without libssh2.
/// @param commandOverride A session's command override, if any.
/// @return true only when an override carries a non-empty program.
[[nodiscard]] inline bool overridesShellProgram(
    std::optional<vtpty::Process::ExecInfo> const& commandOverride) noexcept
{
    return commandOverride.has_value() && !commandOverride->program.empty();
}

class SessionFactory
{
  public:
    virtual ~SessionFactory() = default;

    /// Whether this factory can back a new session right now. Local factories
    /// always can; an attach-mode factory can only hand out PTYs for remote
    /// sessions that exist but have no local tab yet — the manager's creation
    /// entry points (new tab, split, layout) no-op while this is false, so a
    /// "+" click inside a mirror window cannot spawn a stray local shell.
    [[nodiscard]] virtual bool canCreateSession() const noexcept { return true; }

    /// Creates a PTY for a new session, optionally inheriting @p cwd as its working directory and
    /// @p pageSize as its initial grid size.
    ///
    /// @param cwd      If set (and applicable), the new shell starts in this directory. Callers pass the
    ///                 current session's working directory here so a new tab/split opens beside the one
    ///                 it was spawned from.
    /// @param pageSize If set, the child PTY is spawned at this total page size instead of the profile's
    ///                 configured @c terminalSize. Callers pass the currently-running window's page size
    ///                 (see contour::geometry::initialPageSize) so a new tab/split adopts the live window
    ///                 size rather than the profile default; a brand-new window passes @c std::nullopt.
    /// @return The PTY device backing the new session.
    [[nodiscard]] virtual std::unique_ptr<vtpty::Pty> createPty(
        std::optional<std::string> cwd,
        std::optional<vtbackend::PageSize> pageSize = std::nullopt,
        std::optional<vtpty::Process::ExecInfo> commandOverride = std::nullopt,
        std::optional<std::string> profileName = std::nullopt) = 0;

    /// Attach mode: authors a new tab on the DAEMON rather than creating one
    /// locally — the daemon honors it and re-pushes its layout, which reconciles
    /// into a local tab (B3-Qt). A local factory does nothing and returns false, so
    /// the manager creates the tab itself.
    ///
    /// The target WINDOW travels as one of its ptys, matching `requestRemoteSplit` and the
    /// session-keyed verbs underneath: a remote model mints its own window ids, so the only thing
    /// both ends can name a window by is a session it hosts. Taking no window at all is what made
    /// a "+" clicked in a second attach-mode window open its tab in the first.
    /// @param actingPty A pty backing any pane of the target window; null means no preference.
    /// @return true if the request was routed to the daemon (the manager must NOT
    ///         also create a local tab); false for a local factory.
    [[nodiscard]] virtual bool requestRemoteTab(vtpty::Pty const* /*actingPty*/) { return false; }

    /// Attach mode: authors a split of the pane backed by @p actingPty on the
    /// DAEMON (@p vertical orientation) rather than splitting locally — the daemon's
    /// layout re-push reconciles the new pane in (B3-Qt). A local factory returns
    /// false so the manager performs the split itself.
    /// @param actingPty The pty of the pane to split.
    /// @param vertical  The split orientation.
    /// @return true if routed to the daemon; false for a local factory.
    [[nodiscard]] virtual bool requestRemoteSplit(vtpty::Pty const* actingPty, bool vertical)
    {
        (void) actingPty;
        (void) vertical;
        return false;
    }

    /// Attach mode: reports that the user moved the divider of the split separating the panes backed
    /// by @p firstPty and @p secondPty, so the DAEMON's model records the new ratio.
    ///
    /// Unlike the request* verbs this does not ROUTE the operation — the local tree has already been
    /// changed, and the daemon is being told. It has to be told: a re-attaching client rebuilds every
    /// tab from the daemon's layout, so a ratio only it knows about is a ratio the user loses.
    ///
    /// The split is named by one pty from each side rather than by a pane id, because pane ids are
    /// minted per model and the two ends have their own (@see vtworkspace::Pane::lowestCommonAncestor).
    /// A local factory does nothing — its model is the only one there is.
    /// @param firstPty  A pty backing a leaf of the split's FIRST child.
    /// @param secondPty A pty backing a leaf of the split's SECOND child.
    /// @param ratio     The first child's new space share.
    virtual void reportSplitRatio(vtpty::Pty const* /*firstPty*/,
                                  vtpty::Pty const* /*secondPty*/,
                                  double /*ratio*/)
    {
    }

    /// Attach mode: authors a new window on the DAEMON rather than opening a purely
    /// local one — the daemon honors it and pushes the new window's layout, which the
    /// GUI maps onto a fresh OS window (B4). A local factory returns false so the app
    /// opens an ordinary window.
    /// @return true if routed to the daemon; false for a local factory.
    [[nodiscard]] virtual bool requestRemoteWindow() { return false; }

    /// Attach mode: authors the CLOSE of the pane backed by @p pty on the DAEMON, so a session the
    /// user really ended does not linger there headless.
    ///
    /// Called ONLY from the paths that mean "end this session" — a pane close, a tab close, or an
    /// orphaned session with no pane — and never from a window close, an app quit or a lost
    /// connection, where the local view goes away but a daemon-hosted session must keep running.
    /// That distinction cannot be recovered downstream: a PTY is destroyed on all of those paths
    /// alike, so the intent has to be stated where it is known (@see TerminalSessionManager::
    /// SessionEnd). A local factory does nothing — destroying the pty ends its process.
    /// @param pty The pty backing the session to end.
    virtual void requestRemoteClose(vtpty::Pty const* /*pty*/) {}

    /// Announces the terminal that was built around a pty this factory handed out.
    ///
    /// One post-construction call, because the dependency genuinely is cyclic: a `Terminal` cannot
    /// be constructed without its pty, and attach mode needs the `Terminal` in order to populate its
    /// grid from the daemon's deltas. @see the "cyclic wiring" exception in AGENT.md — it is a single
    /// `attach`-style call, not a sequence of them, and the object is fully usable before it.
    ///
    /// A local factory ignores it: a local session's output arrives through its own pty and the
    /// parser puts it on the screen, so nothing outside needs a handle on the grid.
    /// @param pty The pty this factory produced.
    /// @param terminal The terminal now driving it.
    virtual void bindTerminal(vtpty::Pty const* /*pty*/, vtbackend::Terminal& /*terminal*/) {}
};

/// The production SessionFactory: consults the app's active profile and produces either a local
/// process PTY or an SSH session PTY.
class AppSessionFactory final: public SessionFactory
{
  public:
    /// @param app The application, for profile lookup (and SSH host-key verification routing).
    explicit AppSessionFactory(ContourGuiApp& app): _app { app } {}

    [[nodiscard]] std::unique_ptr<vtpty::Pty> createPty(
        std::optional<std::string> cwd,
        std::optional<vtbackend::PageSize> pageSize = std::nullopt,
        std::optional<vtpty::Process::ExecInfo> commandOverride = std::nullopt,
        std::optional<std::string> profileName = std::nullopt) override;

  private:
#ifdef VTPTY_LIBSSH2
    void requestSshHostkeyVerification(vtpty::SshHostkeyVerificationRequest const& request,
                                       vtpty::SshHostkeyVerificationResponseCallback const& response);
#endif

    ContourGuiApp& _app;
};

} // namespace contour
