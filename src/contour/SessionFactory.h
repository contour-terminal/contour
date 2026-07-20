// SPDX-License-Identifier: Apache-2.0
#pragma once

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
