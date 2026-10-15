// SPDX-License-Identifier: Apache-2.0
#include <core/net/AcceptPolicy.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <string>
#include <utility>

namespace core::net
{

AcceptVerdict AcceptErrorPolicy::onError(NetErrorCode code, platform::SteadyTimePoint now) noexcept
{
    auto verdict = AcceptVerdict {};
    switch (acceptDispositionOf(code))
    {
        case AcceptDisposition::Closed: verdict.action = AcceptAction::Stop; return verdict;
        case AcceptDisposition::Dead: verdict.action = AcceptAction::GiveUp; return verdict;
        case AcceptDisposition::PollTick:
            // Says nothing, but counts toward the yield: a listener that answers its poll deadline
            // without suspending would otherwise hold the loop for as long as it keeps answering.
            verdict.delay = yieldDue();
            return verdict;
        case AcceptDisposition::Exhausted:
            verdict.delay = nextBackoff();
            verdict.warning = warningDue(now);
            return verdict;
        case AcceptDisposition::Unclassified:
            if (!_runSince.has_value())
                _runSince = now;
            ++_runLength;
            _runCode = code;
            // Never an end: what nobody classified may well pass, and a loop that ended on it
            // would make a transient condition permanent. A long run is REPORTED, once.
            if (_runLength >= UnclassifiedBeforeDegraded && !_degraded)
            {
                _degraded = true;
                verdict.change = AcceptConditionChange::Degraded;
                verdict.streak =
                    AcceptStreak { .lasted = now - *_runSince, .failures = _runLength, .code = code };
            }
            verdict.delay = nextBackoff();
            verdict.warning = warningDue(now);
            return verdict;
        case AcceptDisposition::PeerFailed:
            // The listener just dequeued a connection, so it is alive whatever came before.
            endRun(now, verdict);
            verdict.delay = yieldDue();
            verdict.warning = warningDue(now);
            return verdict;
    }
    // Not reached: every disposition returns above, and a disposition no row names cannot be made.
    verdict.delay = nextBackoff();
    return verdict;
}

AcceptVerdict AcceptErrorPolicy::onAccepted(platform::SteadyTimePoint now) noexcept
{
    auto verdict = AcceptVerdict {};
    endRun(now, verdict);
    _backoff = {};
    _waitlessInARow = 0;
    return verdict;
}

void AcceptErrorPolicy::endRun(platform::SteadyTimePoint now, AcceptVerdict& verdict) noexcept
{
    if (_degraded)
    {
        verdict.change = AcceptConditionChange::Recovered;
        verdict.streak = AcceptStreak { .lasted = now - _runSince.value_or(now),
                                        .failures = _runLength,
                                        .code = _runCode };
    }
    _degraded = false;
    _runSince.reset();
    _runLength = 0;
    _runCode = {};
}

std::optional<AcceptWarning> AcceptErrorPolicy::warningDue(platform::SteadyTimePoint now) noexcept
{
    // Rate-limited rather than said once: a loop that keeps meeting failures is a fact an operator
    // must be able to see NOW, and a line per failure is a line per client of a loaded machine,
    // which buries everything else in the log.
    if (_warnedAt.has_value() && now - *_warnedAt < WarnInterval)
    {
        ++_unreported;
        return std::nullopt;
    }
    _warnedAt = now;
    return AcceptWarning { .unreported = std::exchange(_unreported, 0) };
}

std::chrono::milliseconds AcceptErrorPolicy::nextBackoff() noexcept
{
    _backoff = _backoff == std::chrono::milliseconds {} ? FirstBackoff : std::min(_backoff * 2, MaxBackoff);
    return _backoff;
}

std::chrono::milliseconds AcceptErrorPolicy::yieldDue() noexcept
{
    // A YIELD, constant and never growing: what came back waited for nothing, and the loop must let
    // the others on its thread run -- but a failed connection consumed a queued one, so the loop is
    // making progress through a backlog, and slowing it down further would keep the port refusing.
    if (++_waitlessInARow < FailuresBeforeYield)
        return {};
    _waitlessInARow = 0;
    return FirstBackoff;
}

namespace
{

    /// @param duration A duration.
    /// @return It in whole milliseconds, for a line.
    [[nodiscard]] std::int64_t millisecondsOf(platform::SteadyDuration duration) noexcept
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
    }

} // namespace

std::string describeAcceptFailure(std::string_view surface,
                                  NetError const& error,
                                  AcceptVerdict const& verdict)
{
    auto const unreported = verdict.warning.has_value() ? verdict.warning->unreported : 0;
    return std::format("{}: an accept failed ({}); {}{}",
                       surface,
                       error.toString(),
                       verdict.delay == std::chrono::milliseconds {}
                           ? std::string { "accepting again" }
                           : std::format("accepting again in {} ms", verdict.delay.count()),
                       unreported == 0 ? std::string {}
                                       : std::format(" ({} more since the last warning)", unreported));
}

std::string describeAcceptDegraded(std::string_view surface,
                                   NetError const& error,
                                   AcceptStreak const& streak)
{
    return std::format(
        "{}: accept loop degraded: {} accepts in a row failed over {} ms with an error nothing "
        "classifies, the last {}; backing off and accepting again until it clears",
        surface,
        streak.failures,
        millisecondsOf(streak.lasted),
        error.toString());
}

std::string describeAcceptRecovered(std::string_view surface, AcceptStreak const& streak)
{
    return std::format("{}: accept loop recovered after {} failed accepts over {} ms (the last {})",
                       surface,
                       streak.failures,
                       millisecondsOf(streak.lasted),
                       toString(streak.code));
}

std::string describeAcceptLoopStopped(std::string_view surface)
{
    return std::format("{}: accept loop stopped while degraded: its listener was closed or it was cancelled",
                       surface);
}

std::string describeAcceptLoopEnded(std::string_view surface, NetError const& error)
{
    return std::format(
        "{}: accept loop ended ({}) on a listener that was not closed; it accepts no more connections",
        surface,
        error.toString());
}

} // namespace core::net
