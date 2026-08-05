// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Derivation of the daemon's control-socket path, following tmux's shape:
/// a per-user runtime directory holding one socket file per label.

#include <crispy/environment.h>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#ifndef _WIN32
    #include <unistd.h>
#endif

namespace vthost
{

/// The pure derivation core: resolves the control-socket path for @p label from
/// explicitly passed inputs (fully deterministic, so tests need no environment
/// mutation — which crispy::environment's snapshot semantics forbid anyway).
///
/// Precedence mirrors tmux's socket discovery, adapted to XDG conventions:
///  1. @p explicitPath (the `--socket PATH` flag) verbatim, if non-empty;
///  2. @p contourMuxEnv verbatim, if set and non-empty (the `$CONTOUR_MUX`
///     override, analogous to tmux's `$TMUX` naming the active server's socket);
///  3. `<xdgRuntimeDir>/contour/<label>`;
///  4. `<temp>/contour-<uid>/<label>` as the fallback when no runtime dir exists
///     (tmux's `/tmp/tmux-<uid>/<label>` shape).
///
/// The parent directory is NOT created here; the listener's bind path hardens
/// and creates it (see net::ensureOwnedPrivateDirectory).
/// @param label The socket label (tmux's `-L`); "default" for the unnamed one.
/// @param explicitPath A user-supplied path override, or empty.
/// @param contourMuxEnv The value of `$CONTOUR_MUX`, if set.
/// @param xdgRuntimeDir The value of `$XDG_RUNTIME_DIR`, if set.
/// @return The resolved socket file path.
[[nodiscard]] inline std::filesystem::path muxSocketPath(std::string_view label,
                                                         std::string_view explicitPath,
                                                         std::optional<std::string_view> contourMuxEnv,
                                                         std::optional<std::string_view> xdgRuntimeDir)
{
    namespace fs = std::filesystem;

    if (!explicitPath.empty())
        return fs::path { explicitPath };

    if (contourMuxEnv && !contourMuxEnv->empty())
        return fs::path { *contourMuxEnv };

    if (xdgRuntimeDir && !xdgRuntimeDir->empty())
        return fs::path { *xdgRuntimeDir } / "contour" / label;

#ifndef _WIN32
    auto const user = std::to_string(::getuid());
#else
    auto const user = crispy::defaultEnvironment().get("USERNAME").value_or("user");
#endif
    return fs::temp_directory_path() / ("contour-" + user) / label;
}

/// Production entry point: derives the path from a process environment.
/// @param label The socket label; "default" for the unnamed one.
/// @param explicitPath A user-supplied path override, or empty.
/// @param env The environment to read `$CONTOUR_MUX` and `$XDG_RUNTIME_DIR` from.
/// @return The resolved socket file path.
[[nodiscard]] inline std::filesystem::path muxSocketPath(
    std::string_view label = "default",
    std::string_view explicitPath = {},
    crispy::environment const& env = crispy::defaultEnvironment())
{
    // Held in named locals, because the pure core above views them rather than owning them.
    auto const contourMux = env.get("CONTOUR_MUX");
    auto const xdgRuntimeDir = env.get("XDG_RUNTIME_DIR");
    auto const viewOf = [](std::optional<std::string> const& value) -> std::optional<std::string_view> {
        if (!value)
            return std::nullopt;
        return std::string_view { *value };
    };

    return muxSocketPath(label, explicitPath, viewOf(contourMux), viewOf(xdgRuntimeDir));
}

// The daemon's sibling endpoints derive from the control-socket path by
// suffix. These helpers are the ONLY place the convention lives; every
// consumer (GUI, POSIX daemon, Win32 daemon, attach flows) derives through
// them.

/// @return The native cells+deltas endpoint beside @p controlSocket.
[[nodiscard]] inline std::filesystem::path nativeSocketPath(std::filesystem::path controlSocket)
{
    controlSocket += "-native";
    return controlSocket;
}

/// @return The binary-tmux (imsg) endpoint beside @p controlSocket.
[[nodiscard]] inline std::filesystem::path tmuxSocketPath(std::filesystem::path controlSocket)
{
    controlSocket += "-tmux";
    return controlSocket;
}

/// The default log file for a daemon that has no console to write to.
///
/// A backgrounded or service-hosted daemon is detached from any terminal, so its standard
/// error goes nowhere and an unconfigured one becomes the instance nobody can diagnose. It
/// lands beside the socket for the same reason the socket is where it is: one place per
/// label, derived rather than remembered.
/// @param controlSocket The daemon's control-socket path.
/// @return The log file path beside @p controlSocket.
[[nodiscard]] inline std::filesystem::path daemonLogPath(std::filesystem::path controlSocket)
{
    controlSocket += ".log";
    return controlSocket;
}

} // namespace vthost
