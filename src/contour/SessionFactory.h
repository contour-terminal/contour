// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/Pty.h>
#if defined(VTPTY_LIBSSH2)
    #include <vtpty/SshSession.h>
#endif

#include <memory>
#include <optional>
#include <string>

namespace contour
{

class ContourGuiApp;

/// Creates the PTY backing a terminal session.
///
/// This is the part of session creation that is independent of how sessions are organized into
/// tabs/windows: it consults the active profile and produces either a local process PTY or an SSH
/// session PTY, optionally rooted at a given working directory. It was extracted from
/// TerminalSessionManager so that every code path that needs a new session (a new tab, a new window,
/// or a new split pane) goes through one place, and so the tab/window model can evolve without
/// touching PTY spawning.
class SessionFactory
{
  public:
    explicit SessionFactory(ContourGuiApp& app): _app { app } {}

    /// Creates a PTY for a new session, optionally inheriting @p cwd as its working directory.
    ///
    /// @param cwd If set (and not an SSH profile), the new shell starts in this directory. Callers
    ///            pass the current session's working directory here so a new tab/split opens beside
    ///            the one it was spawned from.
    [[nodiscard]] std::unique_ptr<vtpty::Pty> createPty(std::optional<std::string> cwd);

  private:
#if defined(VTPTY_LIBSSH2)
    void requestSshHostkeyVerification(vtpty::SshHostkeyVerificationRequest const& request,
                                       vtpty::SshHostkeyVerificationResponseCallback const& response);
#endif

    ContourGuiApp& _app;
};

} // namespace contour
