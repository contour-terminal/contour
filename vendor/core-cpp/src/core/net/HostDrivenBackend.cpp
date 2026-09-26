// SPDX-License-Identifier: Apache-2.0
#include <core/net/HostDrivenBackend.hpp>

#include <algorithm>
#include <chrono>
#include <memory>

namespace core::net
{

namespace
{
    /// What one pump out with the host owns: a weak reference to the backend that asked
    /// for it. Heap-allocated per request and handed to the host as its `void*`, because
    /// the host is the only thing that knows when the pump has been delivered.
    struct PumpTicket
    {
        std::weak_ptr<HostDrivenBackend*> backend; ///< Expired once the backend is destroyed.
    };
} // namespace

HostDrivenBackend::HostDrivenBackend(IHostScheduler& host, platform::IClock& clock) noexcept:
    _host(host), _clock(clock), _liveness(std::make_shared<HostDrivenBackend*>(this))
{
}

std::expected<void, NetError> HostDrivenBackend::attach(ReadinessHandler& /*handler*/)
{
    return std::unexpected { makeNetError(
        NetErrorCode::Unsupported, 0, "HostDrivenBackend: this backend has no readiness") };
}

std::expected<void, NetError> HostDrivenBackend::setInterest(ReadinessHandler& /*handler*/,
                                                             Interest /*interest*/)
{
    return std::unexpected { makeNetError(
        NetErrorCode::Unsupported, 0, "HostDrivenBackend: this backend has no readiness") };
}

void HostDrivenBackend::detach(ReadinessHandler& /*handler*/) noexcept
{
}

WaitResult HostDrivenBackend::wait(std::optional<platform::SteadyDuration> /*timeout*/)
{
    return WaitResult {};
}

void HostDrivenBackend::setPump(HostCallback pump, void* state) noexcept
{
    _pump = pump;
    _pumpState = state;
}

void HostDrivenBackend::scheduleAt(platform::SteadyTimePoint when) noexcept
{
    // Already covered: a pump is out with the host, and it is due no later than this
    // one wants. The loop re-arms after every turn, so whatever this request was for
    // is asked again then.
    if (_scheduledAt.has_value() && *_scheduledAt <= when)
        return;

    auto const now = _clock.now();
    // Clamped at zero: a deadline already past asks for the next turn of the host's
    // loop, not for a negative delay — which `setTimeout` reads as zero on one host
    // and refuses on another.
    auto const delay = std::max(std::chrono::duration_cast<std::chrono::milliseconds>(when - now),
                                std::chrono::milliseconds { 0 });
    _scheduledAt = when;
    // The host owns the ticket until it delivers the pump; `onHostPump` adopts it back.
    auto ticket = std::make_unique<PumpTicket>(PumpTicket { .backend = _liveness });
    _host.callAfter(delay, &HostDrivenBackend::onHostPump, ticket.release());
}

void HostDrivenBackend::wake() noexcept
{
    scheduleAt(_clock.now());
}

void HostDrivenBackend::armWakeAt(std::optional<platform::SteadyTimePoint> deadline) noexcept
{
    // Nothing is scheduled on the loop's side, so only a wake should bring it back.
    // Asking the host for a pump here would spin the page at the host's timer
    // resolution for a loop that has nothing to do.
    if (!deadline.has_value())
        return;
    scheduleAt(*deadline);
}

void HostDrivenBackend::onHostPump(void* state) noexcept
{
    auto const ticket = std::unique_ptr<PumpTicket> { static_cast<PumpTicket*>(state) };
    auto const alive = ticket->backend.lock();
    // The backend was destroyed with this pump out, and it was the host's to deliver
    // anyway: there is nobody to pump.
    if (alive == nullptr)
        return;
    auto* const self = *alive;
    // Cleared BEFORE the pump runs, not after: the turn it drives will arm the next
    // deadline and may wake for work it queues, and neither may be dropped as "one is
    // already scheduled" when the one scheduled is the pump that is running.
    self->_scheduledAt.reset();
    ++self->_pumpCount;
    if (self->_pump != nullptr)
        self->_pump(self->_pumpState);
}

} // namespace core::net
