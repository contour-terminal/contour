// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `armSocketDeadline` — one place that decides how long a socket may be held, and what a caller
/// learns when that runs out.
///
/// Imported from fastcached's `Net/SocketDeadline.hpp` at
/// `0708dd54dc7ee72622c8c0783c2bd4a06f0e9b21`, where the same policy existed twice — once in a
/// server component and once inline in the compile launcher — with only one of the two covered by
/// a regression test.

#include <core/net/DeadlineTimer.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/ISocket.hpp>

#include <chrono>
#include <optional>
#include <utility>

namespace core::net
{

/// What a deadline closes, and where it records that it fired.
///
/// **The flag is the whole reason this is a struct rather than a bare `ISocket*`.** Expiry closes
/// the socket, so an exchange that ran out of budget and one whose peer went away arrive at the
/// caller as the same thing: a broken socket. Those are opposite diagnoses — one says the work
/// was too slow, the other says the machine is gone — and an operator seeing one sentence for
/// both has nothing to act on. Only the timer knows which happened, so only the timer can record
/// it.
///
/// Caller-owned rather than returned beside the timer, because the timer captures its address at
/// construction: a flag living inside a returned value would be pointed at through whatever
/// storage the return expression used.
struct SocketDeadlineTarget
{
    /// Closed on expiry; must outlive the timer.
    ISocket* socket = nullptr;

    /// Set by the timer, and only by the timer. False means the exchange ended for its own
    /// reasons — which, for a keepalive-armed dial, is how a dead peer looks.
    bool expired = false;
};

/// Arms a deadline that closes @p target's socket when it expires, or arms nothing.
///
/// **A non-positive ceiling arms NOTHING**, and that is the decision worth having in one place.
/// The arithmetic says the opposite of what the value means: a zero ceiling puts the deadline at
/// `now()`, so the socket dies on the loop's next turn — a knob documented as "turn the ceiling
/// off" that turns the CONNECTION off instead, silently.
///
/// A null loop also arms nothing, which is why this takes a pointer rather than a reference: a
/// helper written against a caller that always has a loop would not serve one that may not.
///
/// `std::optional` rather than `std::unique_ptr`: a timer that is not armed is honestly spelled
/// by an empty optional, and it costs no allocation. @c DeadlineTimer is immovable, so the value
/// is constructed in place and returned as a prvalue.
///
/// @param loop Where to arm it, or null for nowhere.
/// @param ceiling How long the operation may take; non-positive means unbounded.
/// @param target What to close on expiry and where to record it; must outlive the returned timer.
/// @return The armed timer, or `std::nullopt` when nothing was armed.
[[nodiscard]] inline std::optional<DeadlineTimer> armSocketDeadline(EventLoop* loop,
                                                                    std::chrono::milliseconds ceiling,
                                                                    SocketDeadlineTarget* target)
{
    if (loop == nullptr || target == nullptr || target->socket == nullptr
        || ceiling <= std::chrono::milliseconds::zero())
        return std::nullopt;

    return std::optional<DeadlineTimer> { std::in_place,
                                          *loop,
                                          loop->clock().now() + ceiling,
                                          [](void* state) {
                                              auto& fired = *static_cast<SocketDeadlineTarget*>(state);
                                              // Recorded BEFORE the close, so a caller resumed BY
                                              // the close can never observe a socket that shut
                                              // without a reason attached.
                                              fired.expired = true;
                                              fired.socket->close();
                                          },
                                          target };
}

} // namespace core::net
