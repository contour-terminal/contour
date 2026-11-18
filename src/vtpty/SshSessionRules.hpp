// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The decisions SshSession makes about time limits and about the end of a session. Extracted so every
/// one of them is testable without libssh2, a socket or a host.

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace vtpty
{

/// The longest ssh.connect_timeout there is; a longer one is taken as this. Far beyond any real handshake,
/// and far below where a deadline or libssh2's millisecond timeout could overflow.
inline constexpr auto MaxSshConnectTimeout = std::chrono::hours { 24 };

/// The least time authentication through the SSH agent may take, all identities together, in place of the
/// connect budget, which stands still meanwhile; a longer ssh.connect_timeout gives it that instead.
/// @see sshAgentDeadline()
///
/// An agent may ask its user before it signs -- 1Password or Secretive asking for approval, `ssh-add -c`, a
/// FIDO key waiting for a touch, gpg-agent asking for its PIN -- and that happens inside
/// libssh2_agent_userauth(), where libssh2 1.11.1 gives no way to stop a timeout around it. Its own agent
/// signing cannot be called from a libssh2_userauth_publickey() sign callback instead: libssh2_agent_sign()
/// picks its identity only while the authentication is idle and frees the method name the authentication
/// reads right after the callback (agent.c:932-957, userauth.c:1801-1806). So the agent step gets a generous
/// allowance of its own. No limit applies when ssh.connect_timeout is 0.
///
/// What it bounds: libssh2's waits on the host during the agent step, and -- on Linux, macOS and the other
/// POSIX systems -- the agent's own socket, which libssh2 reads blocking through the session's I/O functions
/// (agent.c, agent_transact_unix): listing the identities and each signature, approval included. An agent
/// that never answers fails the session once the allowance is over. On Windows libssh2 reaches the agent
/// through a named pipe (agent_win.c), outside those functions, so there a pending approval or a silent
/// agent is not cut off; the allowance applies once the agent has answered.
inline constexpr auto SshAgentAuthAllowance = std::chrono::seconds { 120 };

/// Which time limit governs a step of the connection coming up.
enum class SshStepBudget : uint8_t
{
    None = 0, ///< None: the session waits for the user, or is not coming up at all.
    Connect,  ///< The connect budget, ssh.connect_timeout.
    Agent,    ///< The agent's own allowance, SshAgentAuthAllowance. @see sshAgentDeadline()
};

/// @p start + @p limit, or the clock's end where that would overflow.
/// @param start When the limit starts.
/// @param limit How long it lasts.
/// @return The deadline.
[[nodiscard]] std::chrono::steady_clock::time_point saturatingDeadline(
    std::chrono::steady_clock::time_point start, std::chrono::milliseconds limit) noexcept;

/// How long the agent authentication step may take.
/// @param connectLimit The configured connect limit; nullopt when there is none.
/// @return The longer of SshAgentAuthAllowance and @p connectLimit; nullopt for no limit.
[[nodiscard]] constexpr std::optional<std::chrono::milliseconds> sshAgentAllowance(
    std::optional<std::chrono::milliseconds> connectLimit) noexcept
{
    if (!connectLimit)
        return std::nullopt;
    return std::max<std::chrono::milliseconds>(SshAgentAuthAllowance, *connectLimit);
}

/// The deadline of the agent authentication step.
/// @param connectLimit The configured connect limit; nullopt when there is none, and then the agent step has
///                     none either.
/// @param start        When the agent step started.
/// @return @p start + the longer of SshAgentAuthAllowance and @p connectLimit (saturating), or nullopt for no
///         limit.
[[nodiscard]] std::optional<std::chrono::steady_clock::time_point> sshAgentDeadline(
    std::optional<std::chrono::milliseconds> connectLimit,
    std::chrono::steady_clock::time_point start) noexcept;

/// Whether a time limit has anything left; once it is spent, the step it guards is not started.
enum class SshBudget : uint8_t
{
    Spent = 0, ///< Nothing is left.
    Left,      ///< libssh2's timeout was armed with what is left.
};

/// Milliseconds left before @p deadline, for libssh2_session_set_timeout().
/// Never returns 0 (libssh2 reads 0 as "no timeout"): a positive remainder below 1 ms rounds up to 1.
/// @param deadline The end of the budget.
/// @param now      The current time.
/// @return The remaining budget, or nullopt once it is spent.
[[nodiscard]] std::optional<std::chrono::milliseconds> remainingSshBudget(
    std::chrono::steady_clock::time_point deadline, std::chrono::steady_clock::time_point now);

/// The timeout to arm libssh2 with for one blocking call that must end by @p deadline.
/// @param deadline The end of the budget; nullopt when no limit applies.
/// @param now      The current time.
/// @return nullopt once the budget is spent; 0 ms, libssh2's own spelling of "no timeout", when no limit
///         applies; otherwise what is left, never 0. @see remainingSshBudget()
[[nodiscard]] std::optional<std::chrono::milliseconds> sshCallTimeout(
    std::optional<std::chrono::steady_clock::time_point> deadline, std::chrono::steady_clock::time_point now);

/// @p timeout as libssh2_session_set_timeout() takes it: a long, which is 32 bits on Windows. Saturates
/// rather than wrapping round to a negative number.
/// @param timeout The timeout to arm.
/// @return It in milliseconds, at most the largest long.
[[nodiscard]] constexpr long sshTimeoutArgument(std::chrono::milliseconds timeout) noexcept
{
    constexpr auto Largest = std::chrono::milliseconds::rep { std::numeric_limits<long>::max() };
    return static_cast<long>(timeout.count() < Largest ? timeout.count() : Largest);
}

/// The least time one resolved address is given to connect while others are still to try: a share too short
/// for a real connect would just pass the time on.
inline constexpr auto MinSshAddressConnectShare = std::chrono::seconds { 2 };

/// How long one resolved address may take to connect, so that a host whose first address never answers still
/// leaves time for the others: what is left, shared evenly among the addresses still to try, but at least
/// MinSshAddressConnectShare -- and never more than what is left.
/// @param remaining     What is left of the budget; nullopt when no limit applies.
/// @param addressesLeft How many addresses are still to try, this one included.
/// @return This address's limit; nullopt when no limit applies. Never 0 while one does.
[[nodiscard]] constexpr std::optional<std::chrono::milliseconds> sshAddressConnectLimit(
    std::optional<std::chrono::milliseconds> remaining, std::size_t addressesLeft) noexcept
{
    if (!remaining || addressesLeft <= 1)
        return remaining;
    auto const share = *remaining / static_cast<std::chrono::milliseconds::rep>(addressesLeft);
    auto const floored = std::max<std::chrono::milliseconds>(share, MinSshAddressConnectShare);
    return std::min(*remaining, floored);
}

/// The time limit on an SSH connection coming up, from the TCP connect to the started shell. Its clock stops
/// while the session waits for the user (host key confirmation, passphrase, password): how long a person
/// takes to answer is not the host being slow.
class SshConnectBudget
{
  public:
    using Clock = std::chrono::steady_clock;

    /// Starts the clock.
    /// @param limit How long the connection may take; nullopt for no limit. A limit beyond the clock's range
    ///              saturates at its end.
    /// @param now   When the connection attempt starts.
    SshConnectBudget(std::optional<std::chrono::milliseconds> limit, Clock::time_point now) noexcept;

    /// Stops the clock. Does nothing while it is stopped already.
    /// @param now The current time.
    void pause(Clock::time_point now) noexcept;

    /// Starts the clock again, moving the deadline on by the time it stood still. Does nothing while it runs.
    /// @param now The current time.
    void resume(Clock::time_point now) noexcept;

    /// @return The time by which the connection must be up; nullopt for no limit. While the clock stands
    ///         still, the deadline as it was when it stopped.
    [[nodiscard]] std::optional<Clock::time_point> deadline() const noexcept { return _deadline; }

  private:
    std::optional<Clock::time_point> _deadline;
    std::optional<Clock::time_point> _pausedAt;
};

/// What a failed libssh2 call said about why it failed, as far as a time limit is concerned.
enum class SshCallError : uint8_t
{
    Other = 0, ///< Anything but libssh2's own timeout.
    Timeout,   ///< LIBSSH2_ERROR_TIMEOUT: libssh2's timeout fired.
};

/// How a failed step that ran under a time limit is judged.
enum class SshStepFailure : uint8_t
{
    Failed = 0, ///< The step failed for a reason of its own.
    TimedOut,   ///< The time limit ran out.
};

/// Judges a failed step that ran under @p deadline. libssh2 measures its timeout with time(), in whole
/// seconds (session.c, _libssh2_wait_socket), so its timeout can fire before our deadline:
/// LIBSSH2_ERROR_TIMEOUT is a timeout whatever our clock says, and any failure once the deadline has passed
/// is one as well.
/// @param error    What libssh2 reported.
/// @param deadline The step's deadline; nullopt when no limit applies.
/// @param now      The current time.
/// @return TimedOut or Failed.
[[nodiscard]] constexpr SshStepFailure classifySshStepFailure(
    SshCallError error,
    std::optional<std::chrono::steady_clock::time_point> deadline,
    std::chrono::steady_clock::time_point now) noexcept
{
    if (error == SshCallError::Timeout || (deadline && now >= *deadline))
        return SshStepFailure::TimedOut;
    return SshStepFailure::Failed;
}

/// What the agent's socket last did, as the session's I/O functions saw it. @see sshAgentBoundedRecv()
enum class SshAgentIo : uint8_t
{
    Fine = 0, ///< Nothing went wrong on it.
    Closed,   ///< The agent closed the connection, or went away, in the middle of a request.
    TimedOut, ///< The agent's deadline passed while waiting for it.
};

/// How a failed agent request is judged.
enum class SshAgentFailure : uint8_t
{
    Failed = 0,  ///< The agent or the host refused; the next identity, or the next way to log in, may work.
    AgentClosed, ///< The agent went away: nothing more can be asked of it.
    TimedOut,    ///< The agent's allowance ran out.
};

/// Judges a failed agent request. A closed agent says so first, whatever the clock: the user must not be told
/// that an approval may be pending when the agent is simply gone. Without a deadline nothing is a timeout.
/// @param agentIo  What the agent's socket last did.
/// @param error    What libssh2 reported.
/// @param deadline The agent's deadline; nullopt when no limit applies.
/// @param now      The current time.
/// @return How the request failed.
[[nodiscard]] constexpr SshAgentFailure classifySshAgentFailure(
    SshAgentIo agentIo,
    SshCallError error,
    std::optional<std::chrono::steady_clock::time_point> deadline,
    std::chrono::steady_clock::time_point now) noexcept
{
    if (agentIo == SshAgentIo::Closed)
        return SshAgentFailure::AgentClosed;
    if (!deadline)
        return SshAgentFailure::Failed;
    if (agentIo == SshAgentIo::TimedOut
        || classifySshStepFailure(error, deadline, now) == SshStepFailure::TimedOut)
        return SshAgentFailure::TimedOut;
    return SshAgentFailure::Failed;
}

/// Whether text the session injected for the screen is still waiting to be read.
enum class InjectedRead : uint8_t
{
    Empty = 0, ///< Nothing is waiting.
    Pending,   ///< Text is waiting, e.g. the message of the failure that ended the session.
};

/// The errno SshSession::read() reports once the session is over, here or on another thread. A message the
/// end injected is still to be shown, and read() hands out injected text before it reports the end, so the
/// reader is asked to read again (EINTR) while there is some; EPIPE, the end, once there is none.
/// @param injected Whether injected text is waiting.
/// @return EINTR or EPIPE.
[[nodiscard]] constexpr int sshClosedReadErrno(InjectedRead injected) noexcept
{
    return injected == InjectedRead::Pending ? EINTR : EPIPE;
}

} // namespace vtpty
