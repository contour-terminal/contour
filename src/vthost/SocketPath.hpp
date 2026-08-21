// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Derivation of the daemon's control-socket path, following tmux's shape:
/// a per-user runtime directory holding one socket file per label.

#include <vtpty/SandboxInfo.hpp>

#include <crispy/Environment.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#ifndef _WIN32
    #include <unistd.h>
#endif

namespace vthost
{

/// The inputs a control-socket path is derived from.
///
/// A struct rather than six positional parameters: `sandboxAppId` and `user` are both
/// std::string_view and adjacent, so nothing but argument order tells them apart — the same
/// silently-exchangeable-parameters problem AGENT.md names for bools, and one the compiler cannot
/// diagnose. Designated initializers put the name back at the call site.
///
/// Every field carries a default, so a call site names only what it is actually varying: this tree
/// builds with -Wmissing-designated-field-initializers under -Werror, which would otherwise make a
/// partial designated initializer a build error and cost the form its whole point.
struct MuxSocketPathInputs
{
    /// The socket label (tmux's `-L`); "default" for the unnamed one.
    std::string_view label = "default";

    /// A user-supplied path override (the `--socket PATH` flag), or empty.
    std::string_view explicitPath = {};

    /// The value of `$CONTOUR_MUX`, if set.
    std::optional<std::string_view> contourMuxEnv = {};

    /// The value of `$XDG_RUNTIME_DIR`, if set.
    std::optional<std::string_view> xdgRuntimeDir = {};

    /// The sandbox's own name for this application, or empty when not sandboxed.
    std::string_view sandboxAppId = {};

    /// What identifies this user in the fallback directory's name: the numeric uid on POSIX,
    /// `$USERNAME` on Windows.
    std::string_view user = {};
};

/// The pure derivation core: resolves the control-socket path from explicitly passed inputs. Every
/// input is a field — the user identity of the fallback included — so this is fully deterministic
/// and a test drives it without touching the environment or the host it happens to run on.
///
/// Precedence mirrors tmux's socket discovery, adapted to XDG conventions:
///  1. @c explicitPath (the `--socket PATH` flag) verbatim, if non-empty;
///  2. @c contourMuxEnv verbatim, if set and non-empty (the `$CONTOUR_MUX`
///     override, analogous to tmux's `$TMUX` naming the active server's socket);
///  3. `<xdgRuntimeDir>/app/<sandboxAppId>/contour/<label>` when sandboxed;
///  4. `<xdgRuntimeDir>/contour/<label>`;
///  5. `<temp>/contour-<user>/<label>` as the fallback when no runtime dir exists
///     (tmux's `/tmp/tmux-<uid>/<label>` shape).
///
/// Rung 3 exists because inside a Flatpak sandbox `XDG_RUNTIME_DIR` is `/run/user/<uid>` — a
/// directory created inside the sandbox's own mount namespace, NOT the host's. A socket bound at
/// rung 4 from inside the sandbox is therefore not merely at a different path than the host would
/// derive; it is on a private mount the host cannot reach at all, so `contour client` run from a
/// shell that escaped the sandbox finds nothing and can never find anything.
/// `$XDG_RUNTIME_DIR/app/<app-id>` is the one directory flatpak documents as both writable from
/// inside and shared with the host, and since `XDG_RUNTIME_DIR` is `/run/user/<uid>` on both sides,
/// it is the same absolute path in both worlds.
/// @see https://github.com/contour-terminal/contour/issues/2075
///
/// Note that it only makes the two SANDBOXED derivations agree. A `contour` installed on the host
/// is not sandboxed and still derives rung 4, which is why the sandboxed daemon also exports
/// `$CONTOUR_MUX` — rung 2 — into the shells it hosts.
///
/// The parent directory is NOT created here; the listener's bind path hardens
/// and creates it (see net::ensureOwnedPrivateDirectory).
/// @param inputs What the path is derived from.
/// @return The resolved socket file path.
[[nodiscard]] inline std::filesystem::path muxSocketPath(MuxSocketPathInputs const& inputs)
{
    namespace fs = std::filesystem;

    if (!inputs.explicitPath.empty())
        return fs::path { inputs.explicitPath };

    if (inputs.contourMuxEnv && !inputs.contourMuxEnv->empty())
        return fs::path { *inputs.contourMuxEnv };

    if (inputs.xdgRuntimeDir && !inputs.xdgRuntimeDir->empty())
    {
        auto base = fs::path { *inputs.xdgRuntimeDir };
        if (!inputs.sandboxAppId.empty())
            base /= fs::path { "app" } / inputs.sandboxAppId;
        return base / "contour" / inputs.label;
    }

    return fs::temp_directory_path() / ("contour-" + std::string { inputs.user }) / inputs.label;
}

/// Production entry point: derives the path from a process environment and the sandbox this process
/// runs in.
/// @param label The socket label; "default" for the unnamed one.
/// @param explicitPath A user-supplied path override, or empty.
/// @param env The environment to read `$CONTOUR_MUX`, `$XDG_RUNTIME_DIR` and (on
///            Windows) `$USERNAME` from.
/// @param sandbox Where this process runs. Injected for the same reason @p env is, and separate
///                from it because the sandbox describes itself in `/.flatpak-info` — a fact no
///                environment variable can be trusted to reflect.
/// @return The resolved socket file path.
[[nodiscard]] inline std::filesystem::path muxSocketPath(
    std::string_view label = "default",
    std::string_view explicitPath = {},
    crispy::Environment const& env = crispy::defaultEnvironment(),
    vtpty::SandboxInfo const& sandbox = vtpty::currentSandbox())
{
    // Held in named locals, because the pure core above views them rather than owning them.
    auto const contourMux = env.get("CONTOUR_MUX");
    auto const xdgRuntimeDir = env.get("XDG_RUNTIME_DIR");
    auto const asView = [](std::string const& value) {
        return std::string_view { value };
    };

#ifndef _WIN32
    auto const user = std::to_string(::getuid());
#else
    auto const user = env.get("USERNAME").value_or("user");
#endif

    return muxSocketPath(MuxSocketPathInputs { .label = label,
                                               .explicitPath = explicitPath,
                                               .contourMuxEnv = contourMux.transform(asView),
                                               .xdgRuntimeDir = xdgRuntimeDir.transform(asView),
                                               .sandboxAppId = sandbox.applicationId,
                                               .user = user });
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
