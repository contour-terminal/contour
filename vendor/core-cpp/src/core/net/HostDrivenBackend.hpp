// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The @c IoBackend for a loop that does not own its thread.
///
/// Every other backend blocks: the loop calls `wait()` and the kernel wakes it. This
/// one cannot, and the reason is not only the browser — a single-threaded WebAssembly
/// build has no thread to block, and an application that already runs Qt's or GLib's
/// loop has one that is not ours to stop. So the loop is PUMPED instead: it runs one
/// turn, asks for the next pump, and returns. @c IHostScheduler is the only thing it
/// needs from the host to do that.
///
/// **It is portable, and the browser is only one of its hosts.** It builds and is
/// tested on every platform over @c testing::ManualHostScheduler, which is what makes
/// its behaviour a thing this repository can hold still rather than something only a
/// node run can observe.

#include <core/net/IHostScheduler.hpp>
#include <core/net/IoBackend.hpp>
#include <core/platform/Clock.hpp>

#include <cstddef>
#include <expected>
#include <memory>
#include <optional>

namespace core::net
{

/// An @c IoBackend with no wait of its own, driven by a host event loop.
class HostDrivenBackend final: public IoBackend
{
  public:
    /// @param host The host's timer seam (not owned; outlives this backend).
    /// @param clock The clock @c armWakeAt measures a deadline against (not owned).
    ///        Defaults to the process steady clock; a test injects a
    ///        @c platform::ManualClock so the delay it asks the host for is exact
    ///        rather than approximately right.
    explicit HostDrivenBackend(IHostScheduler& host,
                               platform::IClock& clock = platform::defaultSteadyClock()) noexcept;

    [[nodiscard]] BackendKind kind() const noexcept override { return BackendKind::HostDriven; }

    /// Refuses: this backend has no readiness at all.
    ///
    /// It is a refusal rather than a silent acceptance because the alternative is a
    /// flow parked on a registration nothing will ever report — the hang this layer's
    /// rules exist to prevent. What a host-driven loop CAN do is timers, `post` and
    /// `spawn`; descriptor readiness in a browser goes through the host's own APIs and
    /// arrives as a `post`.
    /// @param handler Ignored.
    /// @return @c NetErrorCode::Unsupported, always.
    [[nodiscard]] std::expected<void, NetError> attach(ReadinessHandler& handler) override;

    /// Refuses, for the reason @c attach does.
    /// @param handler Ignored.
    /// @param interest Ignored.
    /// @return @c NetErrorCode::Unsupported, always.
    [[nodiscard]] std::expected<void, NetError> setInterest(ReadinessHandler& handler,
                                                            Interest interest) override;

    /// A no-op: nothing was ever attached. Detaching what was refused must be
    /// harmless, or every teardown path would have to remember which backend it is on.
    /// @param handler Ignored.
    void detach(ReadinessHandler& handler) noexcept override;

    /// Returns at once, whatever the timeout. It NEVER blocks: there is nothing to
    /// block on and, under single-threaded WebAssembly, nothing to block with.
    ///
    /// The timeout is not ignored, it is DELEGATED: the loop's own next deadline
    /// reaches the host through @c armWakeAt, and the host is what waits.
    /// @param timeout Ignored, for the reason above.
    /// @return Always nothing dispatched.
    [[nodiscard]] WaitResult wait(std::optional<platform::SteadyDuration> timeout) override;

    /// Asks the host to pump the loop as soon as it is idle.
    ///
    /// COALESCED: several wakes before the host gets a turn schedule exactly one pump.
    /// Without that, a burst of `post()`s — from inside the loop or from a worker —
    /// would queue one browser timer each, and the page would spend its frame budget
    /// in the scheduler.
    ///
    /// The one member of @c IoBackend another thread may call, and here that
    /// obligation lands on the HOST: a host-driven loop is single-threaded by
    /// construction (that is what it is for), and a host that can be pumped from
    /// another thread must make its own @c IHostScheduler::callAfter safe to reach
    /// from one. `emscripten_async_call` under single-threaded WebAssembly has no
    /// other thread to be reached from.
    void wake() noexcept override;

    /// @return True. What @c isHostDriven exists to answer.
    [[nodiscard]] bool isHostDriven() const noexcept override { return true; }

    /// Asks the host to pump the loop at @p deadline — the loop's next timer.
    ///
    /// The delay handed to the host is `deadline - now`, clamped at zero: a deadline
    /// already past asks for the next turn rather than a negative delay, which
    /// `setTimeout` reads as zero anyway on one host and as an error on another. A
    /// nullopt deadline schedules nothing: the loop has no timer, so only a @c wake
    /// should bring it back.
    ///
    /// It shares the coalescing with @c wake, and deliberately: both ask for "a pump,
    /// at this instant or before it", and two mechanisms would let a wake be lost
    /// behind an armed deadline. A request no EARLIER than the pump already scheduled
    /// is therefore dropped — the loop re-arms after every turn, so the earlier pump
    /// arrives first and the next turn asks again. A request that IS earlier schedules
    /// a second pump beside the first, because a host's timer cannot be retracted: a
    /// spurious pump costs one empty turn, and a missed one is a hang.
    /// @param deadline When the next pump is due, or nullopt if nothing is scheduled.
    void armWakeAt(std::optional<platform::SteadyTimePoint> deadline) noexcept override;

    /// Registers what a pump calls. The loop that this backend drives sets it once,
    /// at construction; Task B4's `EventLoop` is that caller.
    ///
    /// Until it is set, a pump still fires and still clears the coalescing flag — it
    /// simply has nothing to run. That is what makes a backend usable on its own in a
    /// test, and it is why this is not a constructor argument: the loop needs the
    /// backend before it can hand over a pointer to itself.
    /// @param pump What to call on the loop's thread when the host pumps.
    /// @param state Passed to @p pump untouched.
    void setPump(HostCallback pump, void* state) noexcept override;

    /// @return How many times the host has actually pumped this backend.
    [[nodiscard]] std::size_t pumpCount() const noexcept { return _pumpCount; }

    /// @return True while a pump is scheduled and has not yet run. What "coalesced"
    ///         means, made observable.
    [[nodiscard]] bool pumpScheduled() const noexcept { return _scheduledAt.has_value(); }

  private:
    /// What @c IHostScheduler calls back. Static and `noexcept`, because that is what
    /// a @c HostCallback is.
    ///
    /// Its state is a ticket, not the backend: a host's timer cannot be retracted, so a
    /// pump can arrive after the backend is gone — a `PlatformLoop` destroyed with a
    /// deadline armed frees the backend it owns. The ticket names the backend through
    /// @c _liveness, which has expired by then, and is freed here whichever way this
    /// returns. Each pump out with the host owns exactly one.
    /// @param state The pump's ticket, which this takes ownership of.
    static void onHostPump(void* state) noexcept;

    /// Asks the host for a pump due at @p when, unless one is already scheduled no
    /// later than that.
    /// @param when The instant the pump is wanted at.
    void scheduleAt(platform::SteadyTimePoint when) noexcept;

    IHostScheduler& _host;
    platform::IClock& _clock;
    HostCallback _pump = nullptr; ///< What a pump runs; null until the loop sets it.
    void* _pumpState = nullptr;   ///< Handed to @c _pump.
    /// When the soonest pump out with the host is due, or nullopt if none is. Not a
    /// flag, because a deadline armed for later must not swallow a wake wanted now.
    std::optional<platform::SteadyTimePoint> _scheduledAt;
    std::size_t _pumpCount = 0; ///< Pumps the host has delivered.
    /// The one owner of a cell holding this backend's address; a pump's ticket holds a
    /// weak reference to it, so it expires with the backend and a late pump runs nothing.
    std::shared_ptr<HostDrivenBackend*> _liveness;
};

} // namespace core::net
