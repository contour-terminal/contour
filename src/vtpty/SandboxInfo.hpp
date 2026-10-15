// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What the application sandbox this process runs in permits — read from the sandbox's own
/// description of itself rather than guessed from how a call happened to fail.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace vtpty
{

/// Whether this process runs inside an application sandbox, and which.
enum class SandboxState : uint8_t
{
    Host = 0,    ///< No sandbox.
    Flatpak = 1, ///< A Flatpak sandbox.
};

/// Whether the sandbox lets this process reach the network.
///
/// A Flatpak without `--share=network` is placed in a network namespace holding nothing but
/// loopback, so both DNS resolution and connect() fail there. This is the difference between
/// reporting "could not resolve host" and reporting why no host can ever be resolved.
enum class NetworkAccess : uint8_t
{
    Denied = 0,
    Permitted = 1,
};

/// What the sandbox permits, and what it calls this application.
struct SandboxInfo
{
    SandboxState state = SandboxState::Host;

    /// Permitted off the sandbox: there is nothing there to deny it.
    NetworkAccess network = NetworkAccess::Permitted;

    /// The sandbox's own name for this application, e.g. `org.contourterminal.Contour`. Empty off
    /// the sandbox. It is what names the one runtime directory both sides of the sandbox can see.
    /// @see vthost::muxSocketPath.
    std::string applicationId;
};

/// Parses the contents of `/.flatpak-info`, the sandbox's description of itself.
///
/// A minimal walk of the key-file rather than a general parser: only two of its keys are read, and
/// pulling GLib in for `g_key_file_*` is not worth it. Flatpak writes it with
/// `flatpak_context_save_metadata()`, so the two groups and keys below are fixed by its source:
///
///     [Application]
///     name=org.contourterminal.Contour
///     [Context]
///     shared=network;ipc;
///
/// `shared` is a semicolon-separated list whose only defined members are `network` and `ipc`
/// (flatpak's `flatpak_context_shares[]`), so the absence of `network` IS the denial — there is no
/// separate "denied" spelling to look for.
///
/// The returned state is always Flatpak: possessing this file is what being sandboxed means, so a
/// caller with its contents already knows the answer to that.
///
/// @param flatpakInfo The file's contents.
/// @return What it says the sandbox permits.
[[nodiscard]] SandboxInfo parseFlatpakInfo(std::string_view flatpakInfo);

/// The per-user runtime directory this process shares with the rest of the user's session: where a
/// file or a socket placed by this process is found at the same path by a program outside the sandbox.
///
/// Off a sandbox that is `$XDG_RUNTIME_DIR` itself. Inside a Flatpak it is
/// `$XDG_RUNTIME_DIR/app/<application id>`: the sandbox's `$XDG_RUNTIME_DIR` is a private
/// `/run/user/<uid>` in its own mount namespace, and flatpak documents this one subdirectory as the
/// host directory the sandbox may reach ("No access to any host files except the runtime, the app,
/// `~/.var/app/$FLATPAK_ID`, and `$XDG_RUNTIME_DIR/app/$FLATPAK_ID`",
/// https://docs.flatpak.org/en/latest/sandbox-permissions.html). Its source bind-mounts the host's
/// `<real XDG_RUNTIME_DIR>/app/<app-id>` at `/run/flatpak/app/<app-id>` and makes the sandbox's
/// `$XDG_RUNTIME_DIR/app` a link to `/run/flatpak/app` (flatpak `common/flatpak-context.c`, the
/// `run_user_app_src`/`run_user_app_dst` bind), so the path names the same directory on both sides
/// wherever the host's `$XDG_RUNTIME_DIR` is `/run/user/<uid>`, which flatpak always uses inside.
///
/// A relative `$XDG_RUNTIME_DIR` is ignored, as the XDG base directory specification asks: it would
/// name a directory relative to whatever the working directory happens to be.
///
/// Pure, so that both of its readers -- the daemon's socket path and the pager's private files -- are
/// tested without the environment or the sandbox they run in.
/// @param xdgRuntimeDir The value of `$XDG_RUNTIME_DIR`, if set.
/// @param applicationId The sandbox's name for this application (SandboxInfo::applicationId); empty
///                      off a sandbox.
/// @return The directory; std::nullopt where there is none.
[[nodiscard]] std::optional<std::filesystem::path> sharedRuntimeDirectory(
    std::optional<std::string_view> xdgRuntimeDir, std::string_view applicationId);

/// The sandbox this process actually runs in.
///
/// Read once and memoized: neither the file nor the answer changes during the life of a process.
/// This is the process's ONLY reader of /.flatpak-info — Process::isFlatpak() is the narrow
/// predicate over it, not a second look at the same file.
///
/// Off a sandbox this is a default SandboxInfo — Host, network permitted, no application id — so
/// callers never have to ask whether they are sandboxed before asking what they may do.
///
/// @return The description; a reference to process-wide storage.
[[nodiscard]] SandboxInfo const& currentSandbox();

} // namespace vtpty
