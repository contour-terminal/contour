// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What a failed SSH connection attempt tells the user. Extracted from SshSession so the decision is
/// testable: the session itself needs libssh2, a socket and a host that is not there.

#include <vtpty/SandboxInfo.hpp>

#include <chrono>
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

/// Whether a terminfo install ran before the shell's channel was opened.
enum class TerminfoInstallAttempt : uint8_t
{
    None = 0, ///< No install was tried on this connection.
    Attempted ///< An install channel was opened (and has since been left).
};

/// The message a failed opening of the shell's SSH channel should carry.
///
/// After an install attempt the failure may be the install's doing -- a server that allows one session
/// channel per connection, say -- so the message names the opt-out.
/// @param detail  The libssh2 error text.
/// @param attempt Whether a terminfo install ran first.
/// @return The message, ready to show.
[[nodiscard]] std::string describeSshChannelOpenFailure(std::string_view detail,
                                                        TerminfoInstallAttempt attempt);

/// The message an SSH connection that took longer than `ssh.connect_timeout` to come up should carry.
/// @param stage  What the connection was doing when the limit ran out, e.g. "the SSH handshake".
/// @param host   The host being connected to.
/// @param limit  The configured limit.
/// @return The message, ready to show.
[[nodiscard]] std::string describeSshConnectTimeout(std::string_view stage,
                                                    std::string_view host,
                                                    std::chrono::seconds limit);

/// The message an SSH agent authentication that took longer than its own allowance should carry.
/// @param host      The host being connected to.
/// @param allowance How long the agent step may take. @see SshAgentAuthAllowance
/// @return The message, ready to show.
[[nodiscard]] std::string describeSshAgentTimeout(std::string_view host, std::chrono::seconds allowance);

/// The message an SSH agent that closed its connection in the middle of authentication should carry. It makes
/// no claim about an approval: the agent is gone, not waiting.
/// @param host The host being connected to.
/// @return The message, ready to show.
[[nodiscard]] std::string describeSshAgentClosed(std::string_view host);

} // namespace vtpty
