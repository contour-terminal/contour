// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `interruptibleSleepUntil` — sleep to a deadline, or until a stop token is stopped.
///
/// Ported from fastcached's `Async/InterruptibleSleep.{hpp,cpp}` at `0708dd54`, where it was a
/// POLL and said so at length: a scheduled resumption could not be taken back off the reactor, so
/// a wait that also had to be woken by something else slept in steps of `wakeBound` and re-read
/// the token at each one. **That constraint is gone.** An `EventLoop` park is cancellable by id
/// from any thread (@c EventLoop::requestCancel), so this parks ONCE on the deadline and the stop
/// callback is what wakes it. The cost upstream paid — one wake-up per bound per waiting
/// coroutine — is now zero, and `InterruptibleSleep_test.cpp` asserts it with the clock frozen, so
/// no case there can pass by time elapsing.

#include <core/async/StopToken.hpp>
#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/platform/Clock.hpp>

#include <cstdint>

namespace core::net
{

/// Why an @c interruptibleSleepUntil ended.
enum class WakeReason : std::uint8_t
{
    Deadline = 0, ///< The deadline arrived with the token still unstopped (or there was no loop).
    Cancelled,    ///< The supplied token was stopped first.
};

/// Sleeps until @p deadline, or until @p token is stopped — whichever comes first.
///
/// **Two cancellations reach this wait and they mean different things.** @p token is the
/// interruption the caller asked to be told about, and it comes back as @c WakeReason::Cancelled.
/// The awaiting FLOW's own token — the one its `Task` inherited — is the loop's ordinary
/// cancellation, and every loop awaitable answers that by throwing @c async::OperationCancelled so
/// the frame unwinds and its cleanup runs. Where they are the same token, the reported answer
/// wins: a caller that handed in its own token asked to be told, not to be unwound.
///
/// @param loop The loop whose clock gates the deadline and whose park the stop retires. A null
///        pointer resolves immediately as @c WakeReason::Deadline, mirroring @c sleepUntil's
///        nullable-loop contract for transports with no deadline mechanism. A pointer rather than
///        a reference, because a coroutine's reference parameter is bound before the first
///        suspension and then outlives the expression that produced it.
/// @param token Observed before anything is parked, and again on the way out. Taken **by value**,
///        for the same reason.
/// @param deadline The absolute instant to wake at if nothing stops @p token first.
/// @return Why the wait ended.
/// @throws async::OperationCancelled if the awaiting flow's own token was stopped while parked and
///         @p token was not.
[[nodiscard]] async::Task<WakeReason> interruptibleSleepUntil(EventLoop* loop,
                                                              async::StopToken token,
                                                              platform::SteadyTimePoint deadline);

/// The four-argument form fastcached callers wrote, kept for one release.
///
/// @deprecated Use the three-argument overload. **@p wakeBound is ignored**, and that is
/// deliberate rather than an oversight: it named the longest uninterruptible step of a poll, and
/// there is no poll left to bound — the wait parks once and the stop callback wakes it, which is
/// strictly better than any bound a caller could have chosen. Honouring it would keep a polling
/// path alive inside the one function whose purpose is removing polling, so a caller that migrates
/// and keeps the argument would not get the fix.
///
/// It carries no `[[deprecated]]` attribute, and that is a decision rather than an omission: this
/// overload exists so a fastcached caller **compiles unchanged**, and the attribute under that
/// consumer's own `-Werror` is precisely what would stop it doing so — it defeats the one thing
/// the shim is for. The channel that reports a migration in this project is
/// `tools/migrate/renames.json` and the codemods, not the compiler, and the row is already there;
/// the migration is mechanical, being the removal of one argument. The removal of the overload
/// itself is recorded in `CHANGELOG.md` under Deprecated.
/// @param loop The loop to sleep on, or null.
/// @param token The token that interrupts the sleep.
/// @param deadline The absolute instant to wake at.
/// @param wakeBound Ignored; see above.
/// @return Why the wait ended.
[[nodiscard]] async::Task<WakeReason> interruptibleSleepUntil(EventLoop* loop,
                                                              async::StopToken token,
                                                              platform::SteadyTimePoint deadline,
                                                              platform::SteadyDuration wakeBound);

} // namespace core::net
