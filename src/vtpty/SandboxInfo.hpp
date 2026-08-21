// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What the application sandbox this process runs in permits — read from the sandbox's own
/// description of itself rather than guessed from how a call happened to fail.

#include <cstdint>
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
