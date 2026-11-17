// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The decisions SshSession makes about time limits and about the end of a session. Extracted so every
/// one of them is testable without libssh2, a socket or a host.

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <optional>

namespace vtpty
{

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
