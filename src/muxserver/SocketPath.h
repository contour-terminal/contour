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

namespace muxserver
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
    auto const user = std::string { crispy::environment::get("USERNAME").value_or("user") };
#endif
    return fs::temp_directory_path() / ("contour-" + user) / label;
}

/// Production entry point: derives the path from the process environment.
/// @param label The socket label; "default" for the unnamed one.
/// @param explicitPath A user-supplied path override, or empty.
/// @return The resolved socket file path.
[[nodiscard]] inline std::filesystem::path muxSocketPath(std::string_view label = "default",
                                                         std::string_view explicitPath = {})
{
    return muxSocketPath(label,
                         explicitPath,
                         crispy::environment::get("CONTOUR_MUX"),
                         crispy::environment::get("XDG_RUNTIME_DIR"));
}

} // namespace muxserver
