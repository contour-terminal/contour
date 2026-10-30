// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/session/EndReason.hpp>

#include <vtpty/Process.hpp>
#include <vtpty/Pty.hpp>
#ifdef VTPTY_LIBSSH2
    #include <vtpty/SshSession.hpp>
#endif

#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <variant>

namespace contour::session
{

/// The terminal's session exit status: the process (or SSH) exit variant, or nullopt when the app
/// exited without a session ever terminating (e.g. the window was closed while the shell still ran).
using SessionExitStatus = std::optional<std::variant<vtpty::Process::ExitStatus
#ifdef VTPTY_LIBSSH2
                                                     ,
                                                     vtpty::SshSession::ExitStatus
#endif
                                                     >>;

/// Derives the process exit code the terminal should return from its session's exit status.
///
/// This is the pure policy behind ContourGuiApp::run()'s return value, extracted so it can be
/// unit-tested without launching the Qt event loop: a normal exit propagates the child's exit code;
/// a signal exit maps to EXIT_FAILURE; and when no session terminated (nullopt) the caller's
/// @p fallback (Qt's own QApplication::exec() return) is preserved unchanged.
/// @param status   The session's recorded exit status (Process or SSH), or nullopt.
/// @param fallback The exit code to keep when no session status was recorded.
/// @return The exit code the process should return.
[[nodiscard]] inline int exitCodeFor(SessionExitStatus const& status, int fallback) noexcept
{
    if (!status.has_value())
        return fallback;

    auto const mapProcess = [](vtpty::Process::ExitStatus const& s) {
        if (auto const* normal = std::get_if<vtpty::Process::NormalExit>(&s))
            return normal->exitCode;
        return EXIT_FAILURE; // SignalExit
    };

#ifdef VTPTY_LIBSSH2
    if (auto const* ssh = std::get_if<vtpty::SshSession::ExitStatus>(&*status))
    {
        if (auto const* normal = std::get_if<vtpty::SshSession::NormalExit>(ssh))
            return normal->exitCode;
        return EXIT_FAILURE; // SignalExit
    }
    if (auto const* proc = std::get_if<vtpty::Process::ExitStatus>(&*status))
        return mapProcess(*proc);
    return fallback;
#else
    return mapProcess(std::get<vtpty::Process::ExitStatus>(*status));
#endif
}

/// Reads how the program behind @p device ended.
/// @param device The session's terminal device.
/// @return Its exit status; nullopt for a device that runs no program of its own, or not yet ended.
[[nodiscard]] inline SessionExitStatus sessionExitStatusOf(vtpty::Pty const& device)
{
    if (auto const* localProcess = dynamic_cast<vtpty::Process const*>(&device))
        if (auto const status = localProcess->checkStatus())
            return *status;
#ifdef VTPTY_LIBSSH2
    if (auto const* sshSession = dynamic_cast<vtpty::SshSession const*>(&device))
        if (auto const status = sshSession->exitStatus())
            return *status;
#endif
    return std::nullopt;
}

/// Whether a shell that ended within `early_exit_threshold` keeps its window open to say so.
enum class QuickExitNotice : uint8_t
{
    Skip, ///< It said it succeeded -- `exit`, Ctrl+D -- so the window closes as at any other time.
    Show, ///< It may have failed to start, and what it printed must stay readable.
};

/// Decides whether a shell's quick end is reported as a failed start.
///
/// Only an exit status of success clears it: a failure, a signal, and a device that cannot tell how its
/// program ended are all taken for what the notice exists for.
/// @param status How the shell ended.
/// @return Whether the notice is shown.
[[nodiscard]] inline QuickExitNotice quickExitNoticeFor(SessionExitStatus const& status) noexcept
{
    return exitCodeFor(status, EXIT_FAILURE) == EXIT_SUCCESS ? QuickExitNotice::Skip : QuickExitNotice::Show;
}

/// How a session's program ended, as its pane's footer names it.
struct SessionEndReport
{
    EndReason reason = EndReason::None; ///< Exited or Signalled.
    std::string detail {};              ///< The exit code or the signal; empty when unknown.

    bool operator==(SessionEndReport const&) const = default;
};

/// Names how the program behind a session ended.
/// @param status How it ended; nullopt for a device that cannot tell (a mirror, a mock).
/// @return Exited with the code, or Signalled with the signal; Exited with no detail when unknown.
[[nodiscard]] inline SessionEndReport endOf(SessionExitStatus const& status)
{
    if (!status.has_value())
        return SessionEndReport { .reason = EndReason::Exited };

    auto const ofProcess = [](vtpty::Process::ExitStatus const& s) {
        if (auto const* normal = std::get_if<vtpty::Process::NormalExit>(&s))
            return SessionEndReport { .reason = EndReason::Exited,
                                      .detail = std::to_string(normal->exitCode) };
        return SessionEndReport { .reason = EndReason::Signalled,
                                  .detail = std::to_string(std::get<vtpty::Process::SignalExit>(s).signum) };
    };

#ifdef VTPTY_LIBSSH2
    if (auto const* ssh = std::get_if<vtpty::SshSession::ExitStatus>(&*status))
    {
        if (auto const* normal = std::get_if<vtpty::SshSession::NormalExit>(ssh))
            return SessionEndReport { .reason = EndReason::Exited,
                                      .detail = std::to_string(normal->exitCode) };
        return SessionEndReport { .reason = EndReason::Signalled,
                                  .detail = std::get<vtpty::SshSession::SignalExit>(*ssh).signal };
    }
#endif
    return ofProcess(std::get<vtpty::Process::ExitStatus>(*status));
}

} // namespace contour::session
