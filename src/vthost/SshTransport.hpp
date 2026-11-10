// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The ssh transport: `ssh <target> contour daemon --stdio`, read up to the stdio preamble and handed out
/// as an ordinary connection. Authentication is ssh's own. With an IAskpassBroker and an OpenSSH that
/// honours `SSH_ASKPASS_REQUIRE=force` (8.4+), ssh's prompts (password, passphrase, second factor,
/// host-key confirmation) are answered by Contour and the connect deadline does not run meanwhile.
/// Otherwise every invocation runs in batch mode, and a login that wants a prompt fails with the command
/// to run once in a terminal.

#include <core/async/Task.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
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

class IAskpassBroker;

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

/// Who answers ssh's prompts (password, passphrase, second factor, host-key confirmation).
enum class SshPromptMode : std::uint8_t
{
    Batch = 0, ///< Nobody: `BatchMode=yes`, so a login that needs a prompt fails.
    Askpass,   ///< Contour, through the askpass helper ssh is told to run.
};

/// An OpenSSH release, as `ssh -V` reports it.
struct OpenSshVersion
{
    int major; ///< The major number.
    int minor; ///< The minor number.

    /// Orders by major, then minor.
    [[nodiscard]] auto operator<=>(OpenSshVersion const&) const = default;
};

/// The first OpenSSH that honours `SSH_ASKPASS_REQUIRE=force`; older ones only fall back to batch mode.
inline constexpr auto AskpassMinimumOpenSsh = OpenSshVersion { .major = 8, .minor = 4 };

/// @param sshDashV The text `ssh -V` prints, e.g. `OpenSSH_9.6p1 Ubuntu-3ubuntu13, OpenSSL 3.0.13` or
///        `OpenSSH_for_Windows_8.1p1, LibreSSL 3.0.2`.
/// @return The version it names; nullopt when the text is no OpenSSH banner.
[[nodiscard]] std::optional<OpenSshVersion> parseOpenSshVersion(std::string_view sshDashV);

/// @param found The version of the local ssh; nullopt when unknown.
/// @return Askpass when @p found reaches AskpassMinimumOpenSsh, else Batch.
[[nodiscard]] SshPromptMode promptModeFor(std::optional<OpenSshVersion> found);

/// Finds out, once, which OpenSSH the local ssh is: `ssh -V`, whose banner OpenSSH writes to stderr.
/// Shared by the factories of a process; an answer ssh gave, including "no version we know", is kept, and a
/// probe that got none is repeated by the next connect.
class SshVersionProbe
{
  public:
    /// @param loop The loop the probe's child runs on.
    /// @param spawner Starts `ssh -V`.
    /// @return The local ssh's version; nullopt when it could not be run or its banner did not parse.
    [[nodiscard]] core::async::Task<std::optional<OpenSshVersion>> version(core::net::EventLoop* loop,
                                                                           IStdioSpawner* spawner);

  private:
    /// Whether a probe's answer is worth keeping.
    enum class ProbeConclusion : std::uint8_t
    {
        Inconclusive = 0, ///< No banner came: ask again next time.
        Conclusive,       ///< ssh said what it is, or that it is none we know.
    };

    /// What one `ssh -V` made of the local ssh.
    struct Probed
    {
        std::optional<OpenSshVersion> version; ///< Nullopt when no version could be read.
        ProbeConclusion conclusion = ProbeConclusion::Inconclusive; ///< Whether that is to be remembered.
    };

    /// Runs `ssh -V` and reads its banner, without blocking the loop.
    [[nodiscard]] core::async::Task<Probed> probeOnce(core::net::EventLoop* loop, IStdioSpawner* spawner);

    std::mutex _mutex;
    bool _isProbing = false;                             ///< A caller is running the probe.
    std::optional<std::optional<OpenSshVersion>> _found; ///< Empty until the probe finished.
};

/// The argv of one attempt:
/// `ssh -T [-o BatchMode=yes | -o NumberOfPasswordPrompts=3] -o ConnectTimeout=10 -o ServerAliveInterval=15
/// -o ServerAliveCountMax=3 [-p PORT] -- [USER@]HOST REMOTE_COMMAND`. The `--` keeps a destination that
/// begins with `-` from ever being read as an option.
/// @param target Where to attach.
/// @param mode Batch forbids every prompt; Askpass lets ssh ask, a few tries for a password.
/// @return The program and its arguments.
[[nodiscard]] std::vector<std::string> sshArguments(SshTarget const& target, SshPromptMode mode);

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
    Cancelled,              ///< The user dismissed a prompt, or the attempt was cancelled.
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

/// What the words of an ssh failure are made from.
struct SshFailureContext
{
    SshTarget target;             ///< Where the attempt was going.
    std::string_view detail = {}; ///< The error preamble's text, or the stderr tail's last line.
    std::optional<int> exitCode =
        std::nullopt;                          ///< ssh's exit code, which NoPreamble names; nullopt: unknown.
    SshPromptMode mode = SshPromptMode::Batch; ///< How the attempt ran.
    std::optional<OpenSshVersion> foundVersion =
        std::nullopt; ///< The local ssh's version, when it was probed.
};

/// The user-facing error for @p failure.
/// @param failure Why the attempt failed.
/// @param context The target, the detail and how the attempt ran.
/// @return The transport error: its kind decides whether a reconnect retries (retryOf).
[[nodiscard]] TransportError sshTransportError(SshFailure failure, SshFailureContext const& context);

/// Opens connections to a daemon over ssh: one fresh ssh child per attempt, reaped with the connection
/// it carries.
class SshTransportFactory final: public ITransportFactory
{
  public:
    /// @param target Where to attach.
    /// @param spawner Starts ssh; shared, as a reconnect loop and its caller both hold it.
    /// @param broker Answers ssh's prompts; nullptr: batch mode, no prompt is ever answered.
    /// @param connectTimeout Spawn-to-preamble bound, which stands still while a prompt is open; a
    ///        parameter so a test can make it short.
    /// @param probe The process's ssh version probe, shared by every factory so that ssh -V runs once; a null
    ///        one is replaced by a probe of this factory's own.
    SshTransportFactory(SshTarget target,
                        std::shared_ptr<IStdioSpawner> spawner,
                        std::shared_ptr<IAskpassBroker> broker,
                        std::chrono::milliseconds connectTimeout,
                        std::shared_ptr<SshVersionProbe> probe);

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
    std::shared_ptr<IAskpassBroker> _broker;
    std::chrono::milliseconds _connectTimeout;
    std::shared_ptr<SshVersionProbe> _probe;
};

} // namespace vthost
