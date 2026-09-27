// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `IHostScheduler` — the one thing a host event loop has to lend core-cpp's.
///
/// An @c EventLoop normally owns its thread: it blocks in @c IoBackend::wait and
/// resumes what became ready. Inside a browser there is no thread to block — the page
/// stops responding — and inside an application that already runs Qt's or GLib's loop
/// there is one, but somebody else owns it. Both cases want the same thing: the loop
/// does not wait, it is PUMPED, and the only thing it needs from its host is "call
/// this back in N milliseconds".
///
/// That is the whole of this interface, deliberately. Anything larger would be a
/// second event loop pretending to be an adapter.

#include <chrono>

namespace core::net
{

/// What a host calls back when a scheduled pump comes due.
///
/// A function pointer and a `void*`, not a `std::function`: the implementations are
/// `emscripten_async_call` and `QTimer::singleShot`, both of which take exactly this,
/// and an allocation per pump on the browser's timer path is not free.
using HostCallback = void (*)(void* state) noexcept;

/// The seam through which a host event loop drives a @c HostDrivenBackend.
///
/// Implementations: @c EmscriptenHostScheduler (`emscripten_async_call`, which is the
/// browser's `setTimeout`), @c testing::ManualHostScheduler (deterministic, in native
/// tests on every platform), and whatever a Qt or GLib application writes over its
/// own timer.
class IHostScheduler
{
  public:
    /// Asks the host to call @p fn with @p state after @p delay.
    ///
    /// A delay of zero means "as soon as the host is idle", not "now": calling back
    /// synchronously from here would re-enter the loop from inside whatever asked for
    /// the pump, which is the recursion the whole design exists to avoid.
    /// @param delay How long to wait. Zero means the next turn of the host's loop.
    /// @param fn The callback; it must not throw, because a host's loop cannot catch.
    /// @param state Passed to @p fn untouched.
    ///
    /// **Every request accepted is delivered exactly once**, even one that arrives after
    /// whatever asked for it is gone: @p state may own storage that only @p fn frees, as a
    /// @c HostDrivenBackend's pump does, so a host that drops a request leaks it and a
    /// host that delivers one twice frees it twice. There is no retraction, because
    /// `emscripten_async_call` has none.
    virtual void callAfter(std::chrono::milliseconds delay, HostCallback fn, void* state) = 0;

  protected:
    /// Protected and non-virtual: this is a borrowed seam, and nobody deletes a host
    /// through it. The host owns its scheduler and outlives the backend.
    ~IHostScheduler() = default;
};

} // namespace core::net
