// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The decisions behind installing Contour's terminfo entry on an SSH host before the shell starts.
/// Extracted from SshSession so every one of them is testable without libssh2, a socket or a host.

#include <vtpty/SshSessionRules.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>

namespace vtpty
{

/// Whether a built-in SSH session installs Contour's terminfo entry on the remote host.
enum class TerminfoInstall : uint8_t
{
    Off = 0, ///< Install nothing; the session keeps the TERM it would have had.
    Install, ///< Install the entry before the shell starts.
};

/// How a terminfo install attempt ended.
enum class TerminfoInstallOutcome : uint8_t
{
    NotAttempted = 0, ///< No install was tried.
    Installed,        ///< The remote host printed the success marker.
    Failed,           ///< The remote command ran but did not print the marker, or could not be run.
    TimedOut,         ///< The install budget ran out.
};

/// The line the remote command prints, last, once the entry is in place.
inline constexpr std::string_view TerminfoInstalledMarker = "CONTOUR-TERMINFO-INSTALLED";

/// How long the whole install may take.
inline constexpr std::chrono::milliseconds TerminfoInstallTimeout { 3000 };

/// The longest install command, below libssh2's request limit (34,744 bytes) with headroom.
inline constexpr size_t MaxTerminfoCommandSize = 32000;

/// Why an install command could not be built.
enum class TerminfoCommandError : uint8_t
{
    EmptySource = 1,  ///< The terminfo source is empty.
    NonAsciiSource,   ///< The terminfo source holds a byte of 0x80 or above.
    InvalidEntryName, ///< The entry name is not `[a-z0-9-]+`.
    TooLong,          ///< The command would exceed MaxTerminfoCommandSize.
};

/// Applies the pinned-TERM rule: Off if @p option is Off, or @p profileTerm is non-empty and differs from
/// @p entryName (the profile pins a TERM the install would not serve).
/// @param option      The configured install option.
/// @param profileTerm The TERM the profile pins, if any.
/// @param entryName   The terminfo entry that would be installed.
/// @return The install decision to act on.
[[nodiscard]] TerminfoInstall effectiveTerminfoInstall(TerminfoInstall option,
                                                       std::optional<std::string_view> profileTerm,
                                                       std::string_view entryName);

/// Builds the remote command: `sh -c '<body>'`, on one line, with no `'` or `\` inside the body.
/// @param entryName      The entry to install; must match `[a-z0-9-]+`.
/// @param terminfoSource The terminfo source (ASCII), carried hex-encoded inside the command.
/// @return The command, or why it cannot be built.
[[nodiscard]] std::expected<std::string, TerminfoCommandError> buildTerminfoInstallCommand(
    std::string_view entryName, std::string_view terminfoSource);

/// Judges the remote command's standard output.
/// @param stdoutText What the command printed.
/// @return Installed iff a line equals TerminfoInstalledMarker (a trailing '\r' tolerated); else Failed.
[[nodiscard]] TerminfoInstallOutcome judgeTerminfoInstallOutput(std::string_view stdoutText);

/// Chooses the TERM value for the session.
/// @param outcome     How the install ended.
/// @param entryName   The installed entry's name.
/// @param fallbackTerm The TERM to use when the entry is not known to be installed.
/// @return @p entryName if installed, otherwise @p fallbackTerm.
[[nodiscard]] std::string chooseSshTerm(TerminfoInstallOutcome outcome,
                                        std::string_view entryName,
                                        std::string_view fallbackTerm);

/// A short name for @p error, for a log line.
/// @param error What went wrong building the command.
/// @return Its description.
[[nodiscard]] constexpr std::string_view describeTerminfoCommandError(TerminfoCommandError error) noexcept
{
    switch (error)
    {
        case TerminfoCommandError::EmptySource: return "the terminfo source is empty";
        case TerminfoCommandError::NonAsciiSource: return "the terminfo source is not ASCII";
        case TerminfoCommandError::InvalidEntryName: return "the entry name is not [a-z0-9-]+";
        case TerminfoCommandError::TooLong: return "the install command is too long";
    }
    return "unknown error";
}

/// A short name for @p outcome, for a log line.
/// @param outcome How an install attempt ended.
/// @return Its name.
[[nodiscard]] constexpr std::string_view terminfoOutcomeName(TerminfoInstallOutcome outcome) noexcept
{
    switch (outcome)
    {
        case TerminfoInstallOutcome::NotAttempted: return "not attempted";
        case TerminfoInstallOutcome::Installed: return "installed";
        case TerminfoInstallOutcome::Failed: return "failed";
        case TerminfoInstallOutcome::TimedOut: return "timed out";
    }
    return "unknown";
}

/// Judges an install whose budget ran out while its output was still being read. The marker is the
/// command's last line, so a host that printed it before the budget ended did install the entry.
/// @param stdoutText What the command printed so far.
/// @return Installed if the marker is there, else TimedOut.
[[nodiscard]] TerminfoInstallOutcome judgeExpiredTerminfoRead(std::string_view stdoutText);

/// The outcome of an install step that failed as @p failure says.
/// @param failure How the step was judged. @see classifySshStepFailure()
/// @return TimedOut or Failed.
[[nodiscard]] constexpr TerminfoInstallOutcome terminfoOutcomeOf(SshStepFailure failure) noexcept
{
    return failure == SshStepFailure::TimedOut ? TerminfoInstallOutcome::TimedOut
                                               : TerminfoInstallOutcome::Failed;
}

/// Judges an install whose output could not be read to its end. The marker is the command's last line, so a
/// host that printed it before the read failed did install the entry, whatever the failure.
/// @param stdoutText What the command printed so far.
/// @param failure    How the failed read was judged. @see classifySshStepFailure()
/// @return Installed if the marker is there, else TimedOut or Failed as @p failure says.
[[nodiscard]] TerminfoInstallOutcome judgeFailedTerminfoRead(std::string_view stdoutText,
                                                             SshStepFailure failure);

/// The step of the install an outcome was reached in.
enum class TerminfoInstallStage : uint8_t
{
    OpenChannel = 0, ///< Opening the install channel.
    Exec,            ///< Running the command on it.
    Read,            ///< Reading its output.
};

/// What the session does once the install step is left.
enum class AfterTerminfoInstall : uint8_t
{
    StartShell = 0,  ///< Carry on with the shell's own channel.
    EndSession,      ///< End the session: the connection cannot open another channel.
    ConnectTimedOut, ///< End the session: the install used up the connection's time (ssh.connect_timeout).
};

/// Whether libssh2 still holds an outbound packet it could not finish sending. Read from
/// libssh2_session_block_directions(): libssh2 sets LIBSSH2_SESSION_BLOCK_OUTBOUND exactly when it keeps a
/// packet's unsent remainder and clears it once that remainder is out (transport.c, send_existing and
/// _libssh2_transport_send), so the bit is meaningful whatever the call that left it.
enum class SshOutbound : uint8_t
{
    Idle = 0, ///< Nothing is waiting to be sent.
    Pending,  ///< A packet is half-sent; libssh2 retries it before it sends anything else.
};

/// Decides how the session goes on after the install. Two states leave the connection unusable:
/// - a channel OPEN that timed out leaves libssh2's open state half-way (the blocking wrapper gives up
///   without re-entering the open call), so the shell's own open would resume that dead request;
/// - a packet still pending, however the install ended -- libssh2's timeout counts whole seconds
///   (session.c, _libssh2_wait_socket), so it can fire before our own deadline and leave an outcome other
///   than TimedOut. libssh2 refuses to send any other packet until that one is out (transport.c,
///   send_existing), so the shell's own open, which runs without a timeout, would spin on it forever.
/// A usable session whose connect budget the install spent cannot go on either: the shell's channel would
/// find it spent before sending a byte, so the session ends naming the install, the step that used the time.
/// Every other state leaves the session usable.
/// @param stage         Where the install ended.
/// @param outcome       How it ended.
/// @param outbound      Whether a packet was still pending once the install's channel was cleaned up.
/// @param connectBudget Whether the connection has time left (ssh.connect_timeout).
/// @return Whether to start the shell or end the session, and why.
[[nodiscard]] constexpr AfterTerminfoInstall afterTerminfoInstall(TerminfoInstallStage stage,
                                                                  TerminfoInstallOutcome outcome,
                                                                  SshOutbound outbound,
                                                                  SshBudget connectBudget) noexcept
{
    if (outbound == SshOutbound::Pending)
        return AfterTerminfoInstall::EndSession;
    if (stage == TerminfoInstallStage::OpenChannel && outcome == TerminfoInstallOutcome::TimedOut)
        return AfterTerminfoInstall::EndSession;
    if (connectBudget == SshBudget::Spent)
        return AfterTerminfoInstall::ConnectTimedOut;
    return AfterTerminfoInstall::StartShell;
}

/// How SshSession's destructor runs libssh2's teardown calls (channel close, disconnect, session free).
enum class SshTeardown : uint8_t
{
    AsConfigured = 0, ///< In whatever blocking mode the session is in. The socket is shut down by then, so
                      ///< every send fails at once and every read sees the end of the stream: nothing waits.
    NonBlocking,      ///< Switched to non-blocking first, so no call waits; one that cannot finish returns
                      ///< EAGAIN, and a session libssh2_session_free() could not finish is leaked.
};

/// Decides how the libssh2 session is torn down. A pending packet makes libssh2 answer every other send
/// with EAGAIN without touching the socket (transport.c, send_existing), and a blocking call loops on EAGAIN
/// (session.h, BLOCK_ADJUST) waiting for a socket that, shut down, is always ready -- with no timeout, for
/// ever. libssh2_session_free() would do the same, closing every channel still open (session.c,
/// session_free; channel.c, _libssh2_channel_close). Non-blocking turns that loop into one EAGAIN; the price
/// is the dead session's memory, once, for a connection that was already broken.
/// @param outbound Whether a packet is still pending when the session is torn down.
/// @return How to run the teardown.
[[nodiscard]] constexpr SshTeardown sshTeardownFor(SshOutbound outbound) noexcept
{
    return outbound == SshOutbound::Pending ? SshTeardown::NonBlocking : SshTeardown::AsConfigured;
}

/// The end of the install budget: TerminfoInstallTimeout from @p now, but never past the connection's own
/// deadline -- the install is one step of the connection coming up.
/// @param now             When the install starts.
/// @param connectDeadline The connection's deadline; nullopt when it has no limit.
/// @return The install's deadline.
[[nodiscard]] constexpr std::chrono::steady_clock::time_point terminfoInstallDeadline(
    std::chrono::steady_clock::time_point now,
    std::optional<std::chrono::steady_clock::time_point> connectDeadline) noexcept
{
    auto const own = now + TerminfoInstallTimeout;
    return connectDeadline && *connectDeadline < own ? *connectDeadline : own;
}

/// Appends @p chunk to @p buffer, keeping only the last @p cap bytes (the marker is the last line).
/// @param buffer The accumulated output.
/// @param chunk  Newly read bytes.
/// @param cap    The most bytes to keep.
void appendTail(std::string& buffer, std::string_view chunk, size_t cap);

/// Process-lifetime record of hosts whose install failed, injected into SshSession.
class SshTerminfoHostMemo
{
  public:
    virtual ~SshTerminfoHostMemo() = default;
    SshTerminfoHostMemo() = default;
    SshTerminfoHostMemo(SshTerminfoHostMemo const&) = delete;
    SshTerminfoHostMemo& operator=(SshTerminfoHostMemo const&) = delete;
    SshTerminfoHostMemo(SshTerminfoHostMemo&&) = delete;
    SshTerminfoHostMemo& operator=(SshTerminfoHostMemo&&) = delete;

    /// @param hostKey A key from terminfoHostKey().
    /// @return Whether an install on that host failed before.
    [[nodiscard]] virtual bool hasFailed(std::string const& hostKey) const = 0;

    /// Records that an install on @p hostKey failed.
    /// @param hostKey A key from terminfoHostKey().
    virtual void recordFailure(std::string const& hostKey) = 0;
};

/// Thread-safe in-memory SshTerminfoHostMemo.
class InMemorySshTerminfoHostMemo final: public SshTerminfoHostMemo
{
  public:
    /// @copydoc SshTerminfoHostMemo::hasFailed
    [[nodiscard]] bool hasFailed(std::string const& hostKey) const override;

    /// @copydoc SshTerminfoHostMemo::recordFailure
    void recordFailure(std::string const& hostKey) override;

  private:
    mutable std::mutex _mutex;
    std::set<std::string> _failed;
};

/// @param user The remote user.
/// @param host The remote host.
/// @param port The remote port.
/// @return The memo key, "user@host:port".
[[nodiscard]] std::string terminfoHostKey(std::string_view user, std::string_view host, int port);

/// Everything SshSession needs to install; absent means never install.
struct SshTerminfoInstaller
{
    std::string entryName;                     ///< The entry to install, e.g. "contour".
    std::string command;                       ///< Prebuilt by buildTerminfoInstallCommand().
    std::shared_ptr<SshTerminfoHostMemo> memo; ///< Never null.
};

} // namespace vtpty
