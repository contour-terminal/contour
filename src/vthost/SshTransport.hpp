// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The ssh transport: `ssh <target> contour daemon --stdio`, read up to the stdio preamble and handed out
/// as an ordinary connection. Authentication is ssh's own, and only what needs no prompt: every
/// invocation runs in batch mode, and a login that wants a password, a passphrase, a second factor or a
/// host-key confirmation fails with the command to run once in a terminal.

#include <core/async/Task.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <vthost/StdioChild.hpp>
#include <vthost/Transport.hpp>

namespace core::net
{
class EventLoop;
}

namespace vthost
{

/// What a remote host runs when its saved entry names no `remote_command:`.
inline constexpr std::string_view DefaultRemoteCommand = "contour daemon --stdio";

/// How long an attempt may take from spawning ssh to the remote relay's preamble.
inline constexpr auto SshConnectTimeout = std::chrono::seconds { 30 };

/// Where an ssh attach goes.
struct SshTarget
{
    std::string user;                                                 ///< Empty: ssh's own default.
    std::string host;                                                 ///< A name or an address, bare.
    std::optional<std::uint16_t> port;                                ///< Empty: ssh's own default.
    std::string remoteCommand = std::string { DefaultRemoteCommand }; ///< Run by the remote login.
};

/// @param target Where to attach.
/// @return `ssh://[user@]host[:port]`, an IPv6 address in brackets; what logs and messages name.
[[nodiscard]] std::string describeSshTarget(SshTarget const& target);

/// The argv of one attempt:
/// `ssh -T -o BatchMode=yes -o ConnectTimeout=10 -o ServerAliveInterval=15 -o ServerAliveCountMax=3
/// [-p PORT] -- [USER@]HOST REMOTE_COMMAND`. The `--` keeps a destination that begins with `-` from ever
/// being read as an option.
/// @param target Where to attach.
/// @return The program and its arguments.
[[nodiscard]] std::vector<std::string> sshArguments(SshTarget const& target);

/// What the user runs once in a terminal to answer what batch mode cannot: `ssh [-p PORT] [USER@]HOST`.
/// @param target Where to attach.
/// @return The command line, without batch mode.
[[nodiscard]] std::string interactiveSshCommand(SshTarget const& target);

/// Why an ssh attempt ended before the remote relay answered.
enum class SshFailure : std::uint8_t
{
    HostKeyUnknown,         ///< The host's key is not in known_hosts yet.
    HostKeyChanged,         ///< The host's key differs from the one known_hosts holds.
    AuthenticationRequired, ///< No key or agent identity was accepted.
    HostUnreachable,        ///< The name did not resolve, or nothing answered on the port.
    RemoteContourMissing,   ///< The remote login found no `contour` on its PATH.
    RemoteContourTooOld,    ///< The remote `contour` has no `daemon --stdio`.
    RemoteRelayError,       ///< The relay answered with an error preamble.
    RemoteShellNoise,       ///< The remote login printed too much before the preamble.
    NoPreamble,             ///< ssh ended without the relay answering, for no reason recognized.
};

/// One row of the stderr classification: a text ssh or the remote shell prints, and what it means.
struct SshFailureRule
{
    std::string_view needle; ///< Matched anywhere in the stderr tail, case-sensitively.
    SshFailure failure;      ///< What the match means.
};

/// What ssh's stderr says, in match order: the first rule whose needle occurs wins. A new message is a
/// new row here.
inline constexpr auto SshFailureRules = std::to_array<SshFailureRule>({
    { .needle = "REMOTE HOST IDENTIFICATION HAS CHANGED", .failure = SshFailure::HostKeyChanged },
    { .needle = "Host key verification failed", .failure = SshFailure::HostKeyUnknown },
    // sshd's own phrasings only: a remote shell's "PATH: Permission denied" is no login failure.
    { .needle = "Permission denied (", .failure = SshFailure::AuthenticationRequired },
    { .needle = "Permission denied, please try again", .failure = SshFailure::AuthenticationRequired },
    { .needle = "Too many authentication failures", .failure = SshFailure::AuthenticationRequired },
    { .needle = "Could not resolve hostname", .failure = SshFailure::HostUnreachable },
    { .needle = "Connection refused", .failure = SshFailure::HostUnreachable },
    { .needle = "Connection timed out", .failure = SshFailure::HostUnreachable },
    { .needle = "Operation timed out", .failure = SshFailure::HostUnreachable },
    { .needle = "No route to host", .failure = SshFailure::HostUnreachable },
    { .needle = "Unexpected argument \"--stdio\"", .failure = SshFailure::RemoteContourTooOld },
    { .needle = "command not found", .failure = SshFailure::RemoteContourMissing },
    { .needle = "is not recognized as an internal or external command",
      .failure = SshFailure::RemoteContourMissing },
});

/// The exit code a POSIX shell reports for a command it could not find. A child whose status was lost
/// reads as ExitStatusLost instead, never as this.
inline constexpr int ShellCommandNotFound = 127;

/// Classifies an attempt that ended before the preamble.
/// @param stderrTail What ssh (and the remote login through it) wrote to stderr.
/// @param exitCode ssh's exit code; nullopt while it still runs.
/// @return The first rule whose needle occurs in @p stderrTail; else RemoteContourMissing for exit code
///         127; else NoPreamble.
[[nodiscard]] SshFailure classifySshFailure(std::string_view stderrTail, std::optional<int> exitCode);

/// The user-facing error for @p failure.
/// @param failure Why the attempt failed.
/// @param target Where it was going.
/// @param detail The error preamble's text, or the stderr tail's last non-empty line.
/// @param exitCode ssh's exit code, which NoPreamble's message names; nullopt when unknown.
/// @return The transport error: its kind decides whether a reconnect retries (retryOf).
[[nodiscard]] TransportError sshTransportError(SshFailure failure,
                                               SshTarget const& target,
                                               std::string_view detail,
                                               std::optional<int> exitCode = std::nullopt);

/// Opens connections to a daemon over ssh: one fresh ssh child per attempt, reaped with the connection
/// it carries.
class SshTransportFactory final: public ITransportFactory
{
  public:
    /// @param target Where to attach.
    /// @param spawner Starts ssh; shared, as a reconnect loop and its caller both hold it.
    /// @param connectTimeout Spawn-to-preamble bound; a parameter so a test can make it short.
    SshTransportFactory(SshTarget target,
                        std::shared_ptr<IStdioSpawner> spawner,
                        std::chrono::milliseconds connectTimeout = SshConnectTimeout);

    /// Spawns ssh and reads up to the relay's preamble. Every failure ends the child before it returns.
    /// @param loop The loop the connection runs on; the spawn happens on its thread.
    /// @return The connection, its ssh child as its lifetime and an empty token, or why there is none.
    [[nodiscard]] core::async::Task<std::expected<std::unique_ptr<Connection>, TransportError>> connect(
        core::net::EventLoop& loop) override;

    /// @return `ssh://[user@]host[:port]`.
    [[nodiscard]] std::string describe() const override;

    /// @return Verified: ssh checks the host's key against known_hosts, and batch mode refuses a key it
    ///         does not know rather than asking, so no connection reaches an unauthenticated peer.
    [[nodiscard]] PeerTrust peerTrust() const noexcept override { return PeerTrust::Verified; }

  private:
    /// The connect itself: a coroutine takes the loop by pointer, as its reference would outlive the call.
    [[nodiscard]] core::async::Task<std::expected<std::unique_ptr<Connection>, TransportError>> connectOn(
        core::net::EventLoop* loop);

    SshTarget _target;
    std::shared_ptr<IStdioSpawner> _spawner;
    std::chrono::milliseconds _connectTimeout;
};

} // namespace vthost
