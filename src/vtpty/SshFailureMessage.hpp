// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What a failed SSH connection attempt tells the user. Extracted from SshSession so the decision is
/// testable: the session itself needs libssh2, a socket and a host that is not there.

#include <vtpty/SandboxInfo.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace vtpty
{

/// Where an SSH connection attempt failed.
///
/// The two stages fail for different reasons and read differently, but a sandbox with no network
/// breaks BOTH -- and breaks resolution first, which is why the sandbox case cannot be attached to
/// connect() alone.
enum class SshConnectStage : uint8_t
{
    Resolve = 0, ///< The host name could not be resolved.
    Connect,     ///< No resolved address could be reached.
};

/// The message a failed SSH connection attempt should carry.
///
/// Contour's SSH is in-process libssh2 rather than a spawned `ssh`, so it runs INSIDE the sandbox
/// and is subject to its network permission. Without `--share=network` a Flatpak gets a network
/// namespace holding only loopback, so every host is unresolvable and every address unreachable --
/// and "Failed to resolve host" is then a true statement that sends the user looking in entirely
/// the wrong place. @see https://github.com/contour-terminal/contour/issues/2075
///
/// The sandbox sentence is driven by what the sandbox SAYS it permits (`[Context] shared=` in
/// /.flatpak-info), not by which errno came back, because no errno distinguishes a misspelt host
/// name from a host name that could never have been looked up.
///
/// @param stage Where the attempt failed.
/// @param network Whether this process may reach the network at all. @see currentSandbox().
/// @param target What was being reached: the host name, or `address:port`.
/// @param detail The platform's own message -- gai_strerror() or strerror() text.
/// @return The message, ready to show.
[[nodiscard]] std::string describeSshConnectFailure(SshConnectStage stage,
                                                    NetworkAccess network,
                                                    std::string_view target,
                                                    std::string_view detail);

} // namespace vtpty
