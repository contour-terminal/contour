// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What an accept loop does about an accept that failed: one answer for every accept loop, so that
/// none of them decides it by accident.
///
/// **A failed accept is almost never a failed LISTENER.** `AcceptEx` completes with `WSAECONNRESET`
/// when a client reset its connection while it was still queued, and POSIX `accept(2)` answers
/// `EMFILE` and `ENOBUFS` when the process or the system has run out of something. A loop that ends
/// on any of them leaves its listening socket open: the backlog fills, the kernel refuses every
/// later connect, and the port goes on showing `LISTENING` for as long as the process runs.
/// fastcached's compile node served nothing on its port for nine hours that way, while its health
/// endpoint answered `200`, and a flood of client resets reproduced it after 149 connections.
///
/// So only a listener that is closed or dead ends a loop. Everything else is accepted past:
/// silently when it is a listener's own poll deadline, with a rate-limited warning when one
/// connection failed, and after a bounded backoff when something is exhausted -- where accepting
/// again at once would only spin. **A failure nothing classifies (`NetErrorCode::SystemError`) is
/// backed off on as well, without end**, because what lands there is by definition what nobody
/// anticipated, and ending the loop on it would turn a transient condition into a permanent outage
/// -- the very defect this policy exists to prevent. A long run of them is REPORTED instead: the
/// loop is degraded (@c AcceptConditionChange::Degraded), once per run, and recovers when an accept
/// or a failed connection shows the listener is dequeuing again.
///
/// The policy is pure with respect to time and I/O: it decides, and the loop waits, logs and
/// accepts. `serve` (`HttpServer.hpp`) is one such loop.
///
/// Origin: fastcached `src/FastCache/Transport/AcceptPolicy.hpp`.

#include <core/net/NetError.hpp>
#include <core/platform/Clock.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace core::net
{

/// What an accept loop does about one code a failed accept answered.
enum class AcceptDisposition : std::uint8_t
{
    Unclassified, ///< Nothing says whether it passes: back off, and report the loop degraded if
                  ///< it persists (@c AcceptErrorPolicy::UnclassifiedBeforeDegraded).
    Closed,       ///< The listener was closed: the loop ends, quietly.
    Dead,         ///< The listening handle is gone or was never valid: the loop ends, and says so.
    PollTick,     ///< The listener's own poll deadline: accept again, and say nothing.
    PeerFailed,   ///< One connection failed before it was handed over: accept again, and warn.
    Exhausted,    ///< The process or the system ran out of something: back off, then accept again.
};

/// One error code and what an accept loop does about it.
struct AcceptErrorRow
{
    NetErrorCode code;             ///< What the accept answered.
    AcceptDisposition disposition; ///< What the loop does.
};

/// Every @c NetErrorCode, classified for an accept loop, in enumerator order.
///
/// **A table over the whole enumeration rather than a list of the codes that end a loop**, because
/// the failure it exists to prevent is a code nobody thought about ending the loop by default. Its
/// extent is `NetErrorCode::Last`, and the `static_assert` below it checks every row's position, so
/// a code added to the enumeration fails the build here until its row is a decision.
///
/// Two rows end a loop. `Cancelled` is how a listener's `close()` resolves a parked accept, and
/// `BadHandle` is a listening handle that is closed or was never valid -- `EBADF`, `ENOTSOCK`, and
/// `EINVAL` from `accept` (a socket nobody called `listen()` on), which `posix/AcceptLoop.cpp`
/// reports as `BadHandle` -- where accepting again would fail the same way forever. Every other row
/// accepts again, and none of them spins: a backoff waits at least @c AcceptErrorPolicy::FirstBackoff,
/// and a run of the rows that wait for nothing yields every @c AcceptErrorPolicy::FailuresBeforeYield.
inline constexpr auto AcceptErrorTable =
    std::array<AcceptErrorRow, static_cast<std::size_t>(NetErrorCode::Last)> { {
        // Never a failure: a value-initialized code. Nothing says what it means here.
        { .code = NetErrorCode::Ok, .disposition = AcceptDisposition::Unclassified },
        { .code = NetErrorCode::Eof, .disposition = AcceptDisposition::PeerFailed },
        { .code = NetErrorCode::Cancelled, .disposition = AcceptDisposition::Closed },
        // A listener armed with a poll timeout wakes this way to re-check its stop flag: Winsock says
        // `WSAETIMEDOUT` and POSIX says `EAGAIN`, one event under two names (`isDeadlineExpiry`). A
        // pending `ETIMEDOUT` on one new connection is NOT this: `posix/AcceptLoop.cpp` reports it as
        // `HostUnreach`.
        { .code = NetErrorCode::Timeout, .disposition = AcceptDisposition::PollTick },
        { .code = NetErrorCode::WouldBlock, .disposition = AcceptDisposition::PollTick },
        { .code = NetErrorCode::BadHandle, .disposition = AcceptDisposition::Dead },
        // The peer reset a queued connection: `WSAECONNRESET` from `AcceptEx`, the case that stopped
        // fastcached's node.
        { .code = NetErrorCode::ConnReset, .disposition = AcceptDisposition::PeerFailed },
        { .code = NetErrorCode::ConnRefused, .disposition = AcceptDisposition::PeerFailed },
        // Bind-time answers that an accept should never give; nothing says they pass.
        { .code = NetErrorCode::AddressInUse, .disposition = AcceptDisposition::Unclassified },
        { .code = NetErrorCode::AddressNotAvail, .disposition = AcceptDisposition::Unclassified },
        { .code = NetErrorCode::AddressError, .disposition = AcceptDisposition::Unclassified },
        // accept(2)'s network errors pending on ONE new connection -- `EHOSTUNREACH`, `ENETUNREACH`,
        // `EHOSTDOWN`, `ENETDOWN`, `ENONET`, and a pending `ETIMEDOUT` -- and Winsock's
        // `WSAEHOSTUNREACH`, `WSAENETUNREACH` and `WSAEHOSTDOWN`. Not `WSAENETDOWN`, which is
        // `SystemError`: see `windows/SocketErrors.cpp`.
        { .code = NetErrorCode::HostUnreach, .disposition = AcceptDisposition::PeerFailed },
        // Linux answers `EPERM` from `accept` when a packet filter refused that one connection.
        { .code = NetErrorCode::PermissionDenied, .disposition = AcceptDisposition::PeerFailed },
        // From an accept, only `EOPNOTSUPP`, which means two things: an error pending on one new
        // connection, and -- on every call, forever -- a listener that is not `SOCK_STREAM`. Every
        // listener this library makes is a stream, so what reaches a loop is the first. Still bounded
        // were it the second: a run of `PeerFailed` yields every `FailuresBeforeYield` and warns at most
        // once per `WarnInterval`.
        { .code = NetErrorCode::Unsupported, .disposition = AcceptDisposition::PeerFailed },
        { .code = NetErrorCode::MessageTooLarge, .disposition = AcceptDisposition::PeerFailed },
        // An OS error nothing classified further. Among it `WSAENETDOWN`, which Winsock answers when
        // the network subsystem has failed, for every call on the machine: perhaps transient, perhaps
        // not, which is exactly what `Unclassified` is for.
        { .code = NetErrorCode::SystemError, .disposition = AcceptDisposition::Unclassified },
        // `EMFILE`, `ENFILE`, `ENOBUFS`, `ENOMEM`, `ENOSR`, `WSAEMFILE`, `WSAENOBUFS`,
        // `WSA_NOT_ENOUGH_MEMORY`: out of descriptors, buffer space or memory. Accepting again at once
        // would spin a core; nothing else about it is wrong.
        { .code = NetErrorCode::ResourceExhausted, .disposition = AcceptDisposition::Exhausted },
    } };

namespace detail
{

    /// @return Whether every row of @c AcceptErrorTable sits at its code's position.
    [[nodiscard]] constexpr bool acceptRowsInEnumeratorOrder() noexcept
    {
        auto index = std::size_t { 0 };
        for (auto const& row: AcceptErrorTable)
        {
            if (static_cast<std::size_t>(row.code) != index)
                return false;
            ++index;
        }
        return true;
    }

} // namespace detail

static_assert(detail::acceptRowsInEnumeratorOrder(),
              "AcceptErrorTable holds exactly one row per NetErrorCode, in enumerator order: a code "
              "added to NetErrorCode needs its row, and the row is a decision");

/// @param code What an accept answered.
/// @return What an accept loop does about it. A value that is no code is @c Unclassified.
[[nodiscard]] constexpr AcceptDisposition acceptDispositionOf(NetErrorCode code) noexcept
{
    auto const index = static_cast<std::size_t>(code);
    if (index >= AcceptErrorTable.size())
        return AcceptDisposition::Unclassified;
    return AcceptErrorTable[index].disposition;
}

/// What an accept loop does next, after one failed accept.
enum class AcceptAction : std::uint8_t
{
    AcceptAgain, ///< Accept again, after @c AcceptVerdict::delay.
    Stop,        ///< The listener was closed: end the loop. Nothing went wrong.
    GiveUp,      ///< The listener is dead: end the loop, and say so -- whatever was served there is
                 ///< no longer accepting (@c describeAcceptLoopEnded).
};

/// Whether the loop's condition changed with this failure or accept.
enum class AcceptConditionChange : std::uint8_t
{
    None,      ///< No change worth a report.
    Degraded,  ///< A run of unclassified failures reached @c AcceptErrorPolicy::UnclassifiedBeforeDegraded:
               ///< the loop is backing off on something nobody classified. Reported once per run.
    Recovered, ///< A run that was reported degraded ended: an accept, or a failed connection, shows
               ///< the listener is dequeuing again.
};

/// A run of unclassified failures, as a degraded or recovered report carries it.
struct AcceptStreak
{
    platform::SteadyDuration lasted {}; ///< From the run's first failure to the one reported on.
    std::uint32_t failures {};          ///< How many failures the run holds.
    NetErrorCode code {};               ///< What the last of them answered.
};

/// A warning the rate limit let through.
struct AcceptWarning
{
    std::uint64_t unreported {}; ///< Failures since the previous warning that were not said.
};

/// The answer to one failed accept.
struct AcceptVerdict
{
    std::chrono::milliseconds delay {};   ///< How long to wait before accepting again; zero is at once.
    std::optional<AcceptWarning> warning; ///< Set when a warning is due; the rate limit is applied.
    AcceptStreak streak;                  ///< The run @c change reports on; empty when it is @c None.
    AcceptAction action {};               ///< What the loop does.
    AcceptConditionChange change {};      ///< Whether this failure made the loop degraded or recovered.
};

/// The per-loop state behind @c AcceptVerdict: the backoff, the yield, the run of unclassified
/// failures and the warning rate limit. One per accept loop.
///
/// Pure with respect to time: the caller hands in the instant from its own clock -- a loop on an
/// @c EventLoop reads the loop's -- so a test drives it with a @c platform::ManualClock.
class AcceptErrorPolicy
{
  public:
    /// The first backoff after an exhaustion, doubled on each one that follows.
    static constexpr std::chrono::milliseconds FirstBackoff { 10 };
    /// The longest backoff: what bounds how late a recovered system is noticed.
    static constexpr std::chrono::milliseconds MaxBackoff { 1000 };
    /// At most one warning per interval, carrying how many it stands for.
    static constexpr std::chrono::seconds WarnInterval { 10 };
    /// Answers in a row that wait for nothing -- failed connections and poll ticks, with no accept
    /// between them -- before the loop YIELDS for @c FirstBackoff. A listener whose accept fails
    /// without suspending would otherwise never yield its loop, and no other flow on it would run.
    /// A yield rather than a backoff, and never growing, because each failed connection consumed a
    /// queued one: the loop is draining a backlog, and slowing it down keeps the port refusing
    /// (measured in fastcached: a doubling backoff held a port refusing for as long as a flood's
    /// backlog of dead connections took to drain at one a second).
    static constexpr std::uint32_t FailuresBeforeYield { 16 };
    /// Unclassified failures in a row, with no accept and no failed connection between them, before
    /// the loop is reported degraded: about 25 seconds of nothing else under the backoff above. The
    /// loop goes on backing off at @c MaxBackoff -- it never ends on one -- and the report is made
    /// once per run.
    static constexpr std::uint32_t UnclassifiedBeforeDegraded { 32 };

    /// Decides what the loop does about one failed accept.
    /// @param code What the accept answered.
    /// @param now The loop's clock.
    /// @return The verdict.
    [[nodiscard]] AcceptVerdict onError(NetErrorCode code, platform::SteadyTimePoint now) noexcept;

    /// An accept succeeded: the backoff, the yield count and the run start again.
    /// @param now The loop's clock.
    /// @return @c AcceptConditionChange::Recovered with the run it ended when that run had been
    ///         reported degraded, and @c AcceptConditionChange::None with an empty run otherwise.
    [[nodiscard]] AcceptVerdict onAccepted(platform::SteadyTimePoint now) noexcept;

    /// @return Whether the loop is degraded now: a run was reported and nothing has ended it. A
    ///         loop that stops in that state says so (@c AcceptLoopEventKind::Stopped), or a
    ///         liveness registry would go on showing a surface that was shut down as degraded.
    [[nodiscard]] bool degraded() const noexcept { return _degraded; }

  private:
    /// @param now The loop's clock.
    /// @return The warning, if the rate limit lets one through now.
    [[nodiscard]] std::optional<AcceptWarning> warningDue(platform::SteadyTimePoint now) noexcept;

    /// @return The next backoff: @c FirstBackoff, then doubled up to @c MaxBackoff.
    [[nodiscard]] std::chrono::milliseconds nextBackoff() noexcept;

    /// @return A delay of @c FirstBackoff once every @c FailuresBeforeYield calls, and zero otherwise.
    [[nodiscard]] std::chrono::milliseconds yieldDue() noexcept;

    /// Ends the run of unclassified failures.
    /// @param now The loop's clock.
    /// @param verdict Given @c AcceptConditionChange::Recovered and the run, when it had been reported.
    void endRun(platform::SteadyTimePoint now, AcceptVerdict& verdict) noexcept;

    std::optional<platform::SteadyTimePoint> _warnedAt; ///< When the last warning went out.
    std::optional<platform::SteadyTimePoint> _runSince; ///< When the run's first failure came.
    std::chrono::milliseconds _backoff {};              ///< The last backoff; zero when none is running.
    std::uint64_t _unreported {};                       ///< Failures since the last warning, not said.
    std::uint32_t _waitlessInARow {};                   ///< Answers that waited for nothing, in a row.
    std::uint32_t _runLength {};                        ///< Unclassified failures in the run.
    NetErrorCode _runCode {};                           ///< What the run's last failure answered.
    bool _degraded {};                                  ///< Whether the run was reported degraded.
};

/// The warning line for a failed accept the loop goes on past.
/// @param surface What the loop serves, as its other log lines name it.
/// @param error What the accept answered.
/// @param verdict The policy's answer to it, carrying a warning.
/// @return The line, for example `http: an accept failed (resource exhausted (accept) [errno 24]);
///         accepting again in 10 ms (3 more since the last warning)`.
[[nodiscard]] std::string describeAcceptFailure(std::string_view surface,
                                                NetError const& error,
                                                AcceptVerdict const& verdict);

/// The line for an accept loop reported degraded (@c AcceptConditionChange::Degraded).
/// @param surface What the loop serves.
/// @param error What the last accept answered.
/// @param streak The run.
/// @return The line.
[[nodiscard]] std::string describeAcceptDegraded(std::string_view surface,
                                                 NetError const& error,
                                                 AcceptStreak const& streak);

/// The line for an accept loop that recovered (@c AcceptConditionChange::Recovered).
/// @param surface What the loop serves.
/// @param streak The run that ended.
/// @return The line.
[[nodiscard]] std::string describeAcceptRecovered(std::string_view surface, AcceptStreak const& streak);

/// The line for an accept loop that stopped -- its listener closed, or its flow cancelled -- while
/// it was degraded.
/// @param surface What the loop serves.
/// @return The line.
[[nodiscard]] std::string describeAcceptLoopStopped(std::string_view surface);

/// The line for an accept loop that gave up (@c AcceptAction::GiveUp).
/// @param surface What the loop serves.
/// @param error What the last accept answered.
/// @return The line.
[[nodiscard]] std::string describeAcceptLoopEnded(std::string_view surface, NetError const& error);

/// What an accept loop reports, beside what it decides: the event a log line, a liveness registry
/// (@c AcceptLoopHealth::record) or a test receives.
enum class AcceptLoopEventKind : std::uint8_t
{
    Warning,   ///< A failed accept the loop went on past; rate-limited.
    Degraded,  ///< The loop is backing off on a long run of unclassified failures.
    Recovered, ///< A degraded loop is accepting again.
    GaveUp,    ///< The loop ended on a dead listener, which it then closed.
    Stopped,   ///< A degraded loop ended because its listener was closed or its flow cancelled:
               ///< it is no longer degraded, it is no longer serving, and nothing went wrong.
};

/// One report from an accept loop.
struct AcceptLoopEvent
{
    std::string surface;      ///< What the loop serves.
    std::string line;         ///< The event in words, ready for a log.
    NetError error;           ///< What the accept answered; empty for @c AcceptLoopEventKind::Recovered
                              ///< and @c AcceptLoopEventKind::Stopped.
    AcceptLoopEventKind kind; ///< What happened.
};

} // namespace core::net
