// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/platform/Notifier.hpp>
#include <contour/session/HostedOperation.hpp>

#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/screen/Terminal.hpp>
#include <vtbackend/shell/CommandLineText.hpp>

#include <vtpty/Process.hpp>
#include <vtpty/Pty.hpp>
#ifdef VTPTY_LIBSSH2
    #include <vtpty/SshSession.hpp>
#endif

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <vthost/client/TabPresentation.hpp>

class QObject;

namespace contour
{
// The composition root, which owns the configuration this factory reads. Named rather than
// included: session/ sits below it, and phase 3's AppServices seam is what removes the name.
class ContourGuiApp;
} // namespace contour

namespace contour::session
{

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

/// Where the sessions a SessionFactory makes run their programs.
enum class SessionHosting : uint8_t
{
    Local = 0, ///< On this machine: a new session runs its profile's shell, or the command it is given.
    Remote,    ///< On a daemon or a tmux server (attach mode): a new pane is bound to a session the
               ///< remote end hosts, and a command given for it is dropped.
};

/// What a session authored on the remote end runs, as this client asks for it (SessionFactory's
/// requestRemote* verbs). A struct rather than a bare name so that a later field -- a working directory,
/// say -- is one more member, not one more parameter on every override.
struct RemoteSpawn
{
    /// The host profile to run, by name: the action's own (`CreateNewTab`/`NewTerminal`), else empty for
    /// the one this client named in its hello. The daemon resolves it in its own configuration and runs
    /// its default profile for a name it does not have. A remote end without profiles ignores it.
    std::string profileName {};
};

/// How a tab is presented here, as SessionFactory::reportTabPresentation is told. Its panes are named by the
/// ptys behind them: only the factory knows which remote session a pty is bound to.
struct LocalTabPresentation
{
    vtpty::Pty const* activePty = nullptr; ///< The active pane's pty, which also names the tab; never null.
    vtpty::Pty const* zoomedPty = nullptr; ///< The zoomed pane's pty; null when the tab is not zoomed.
    std::optional<std::string> title {};   ///< The user's title; std::nullopt for the automatic one.
    /// The colour the user chose (vtworkspace::TabColorSource::User) as 0xRRGGBB; std::nullopt for none. Not
    /// the effective colour: a profile's or an application's is this client's own, not the user's choice.
    std::optional<uint32_t> color {};
    vthost::client::TabActivity activity = vthost::client::TabActivity::Inactive;
};

/// Where a factory that mirrors a daemon's sessions tells one what a restart stopped in its shell: the
/// command line, or nullopt once it stands no more.
///
/// The factory learns it on its own reactor thread; @ref deliver runs on @ref context's thread (the GUI
/// thread, the session's), posted there, and is dropped if @ref context is gone first.
struct InterruptionSink
{
    QObject* context = nullptr; ///< The object whose thread @ref deliver runs on; null drops everything.
    std::function<void(std::optional<vtbackend::CommandLineText>)> deliver {}; ///< What to run there.
};

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

    /// Where the sessions this factory makes run. canCreateSession() cannot answer whether a pane may
    /// run a given program: an attach-mode factory that can make a session at all makes it for a pane
    /// the remote end announced, so a pager asked for in that moment would take that pane's place.
    /// @return Local for a factory that spawns here (the default); Remote for an attach-mode one.
    [[nodiscard]] virtual SessionHosting hosting() const noexcept { return SessionHosting::Local; }

    /// The notifier a new session raises its notifications through.
    ///
    /// A seam rather than a choice: production takes the one the platform offers, and a test hands in
    /// one that records what was raised and plays the desktop's events back.
    /// @return The notifier, or null for the platform's own (the default).
    [[nodiscard]] virtual std::unique_ptr<platform::Notifier> createNotifier() { return nullptr; }

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
    /// @param spawn What the new tab's session runs, as the host is asked for it.
    /// @return true if the request was routed to the daemon (the manager must NOT
    ///         also create a local tab); false for a local factory.
    [[nodiscard]] virtual bool requestRemoteTab(vtpty::Pty const* /*actingPty*/, RemoteSpawn const& /*spawn*/)
    {
        return false;
    }

    /// Attach mode: authors a split of the pane backed by @p actingPty on the
    /// DAEMON (@p vertical orientation) rather than splitting locally — the daemon's
    /// layout re-push reconciles the new pane in (B3-Qt). A local factory returns
    /// false so the manager performs the split itself.
    /// @param actingPty The pty of the pane to split.
    /// @param vertical  The split orientation.
    /// @param spawn What the new pane's session runs, as the host is asked for it.
    /// @return true if routed to the daemon; false for a local factory.
    [[nodiscard]] virtual bool requestRemoteSplit(vtpty::Pty const* /*actingPty*/,
                                                  bool /*vertical*/,
                                                  RemoteSpawn const& /*spawn*/)
    {
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

    /// Attach mode: reports how the user now presents a tab -- its active and zoomed pane, title, colour, and
    /// whether it is its window's active tab -- so the DAEMON's model, and every other client attached to it,
    /// follows.
    ///
    /// Like reportSplitRatio it does not route anything: the local model already changed. The factory
    /// compares with what the daemon last said and sends only what differs, so a report the daemon's own
    /// layout caused costs nothing. A local factory does nothing.
    /// @param presentation How the tab is presented here.
    virtual void reportTabPresentation(LocalTabPresentation const& /*presentation*/) {}

    /// Attach mode: authors a new window on the DAEMON rather than opening a purely
    /// local one — the daemon honors it and pushes the new window's layout, which the
    /// GUI maps onto a fresh OS window (B4). A local factory returns false so the app
    /// opens an ordinary window.
    /// @param spawn What the new window's first session runs, as the host is asked for it.
    /// @return true if routed to the daemon; false for a local factory.
    [[nodiscard]] virtual bool requestRemoteWindow(RemoteSpawn const& /*spawn*/) { return false; }

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

    /// Decides where an operation on the engine of the session backed by @p pty runs, and sends it to the
    /// remote end when that is where it runs.
    ///
    /// An attach-mode pane's terminal mirrors one the remote end hosts, so an action that changes engine
    /// state has to change THAT terminal, or the next update undoes it (@see HostedOperation.hpp). A local
    /// factory runs every operation here.
    /// @param pty The pty backing the acting session.
    /// @param request The operation, with its text.
    /// @return Where it runs: RunHere for a local factory and a pty not of this factory's making.
    [[nodiscard]] virtual HostedOperationRoute requestHostedOperation(
        vtpty::Pty const* /*pty*/, HostedOperationRequest const& /*request*/)
    {
        return HostedOperationRoute::RunHere;
    }

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
    /// @param interruptions Where the session is told what a restart stopped in its shell.
    virtual void bindTerminal(vtpty::Pty const* /*pty*/,
                              vtbackend::Terminal& /*terminal*/,
                              InterruptionSink const& /*interruptions*/)
    {
    }
};

/// The production SessionFactory: consults the app's active profile and produces either a local
/// process PTY or an SSH session PTY.
class AppSessionFactory final: public SessionFactory
{
  public:
    /// @param app       The application, for profile lookup (and SSH host-key verification routing).
    /// @param placement Moves each spawned shell into its own resource domain. @see vtpty::ProcessPlacement.
    AppSessionFactory(ContourGuiApp& app, std::shared_ptr<vtpty::ProcessPlacement> placement):
        _app { app }, _placement { std::move(placement) }
    {
    }

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
    std::shared_ptr<vtpty::ProcessPlacement> _placement;
};

} // namespace contour::session
