// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/Process.h>
#if defined(VTPTY_LIBSSH2)
    #include <vtpty/SshSession.h>
#endif

#include <cstdlib>
#include <optional>
#include <variant>

namespace contour
{

/// The terminal's session exit status: the process (or SSH) exit variant, or nullopt when the app
/// exited without a session ever terminating (e.g. the window was closed while the shell still ran).
using SessionExitStatus = std::optional<std::variant<vtpty::Process::ExitStatus
#if defined(VTPTY_LIBSSH2)
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

#if defined(VTPTY_LIBSSH2)
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

} // namespace contour
