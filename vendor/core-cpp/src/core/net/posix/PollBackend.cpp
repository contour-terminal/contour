// SPDX-License-Identifier: Apache-2.0
#include <core/net/posix/PollBackend.hpp>

#include <core/net/detail/WaitTimeout.hpp>

#include <algorithm>
#include <ranges>
#include <vector>

#include <poll.h>

namespace core::net
{

namespace
{
    /// Translates a readiness interest into poll(2) event bits.
    /// @param interest What the registration is watched for.
    /// @return The corresponding POLLIN/POLLOUT bitmask.
    [[nodiscard]] short toPollEvents(Interest interest) noexcept
    {
        short events = 0;
        if (hasInterest(interest, Interest::Read))
            events |= POLLIN;
        if (hasInterest(interest, Interest::Write))
            events |= POLLOUT;
        return events;
    }

    /// Translates poll(2)'s answer for one descriptor into the portable vocabulary
    /// @c selectReadinessCallback routes on.
    /// @param revents The revents poll(2) reported.
    /// @return What was observed.
    [[nodiscard]] Readiness fromPollRevents(short revents) noexcept
    {
        auto observed = Readiness::None;
        if ((revents & POLLIN) != 0)
            observed = observed | Readiness::Readable;
        if ((revents & POLLOUT) != 0)
            observed = observed | Readiness::Writable;
        // POLLNVAL belongs here beside the other two: it is what poll(2) answers for a
        // descriptor that was CLOSED while still registered, and routing it is the
        // difference between the flow parked on it being told and the pump reporting
        // the same dead descriptor on every wait forever.
        if ((revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
            observed = observed | Readiness::Failed;
        return observed;
    }
} // namespace

PollBackend::PollBackend()
{
    // The wakeup channel is registration 0 and is never removed, so the wait set is
    // never empty: poll(2) always has something to block on and `wake()` can always
    // break it. It goes through the ordinary attach/setInterest path rather than a
    // private one, so the path shutdown depends on is the path every case exercises.
    _registrations.push_back(Registration { .handler = &_wakeup.handler(), .interest = Interest::Read });
}

PollBackend::Registration* PollBackend::find(ReadinessHandler const& handler) noexcept
{
    auto const found = std::ranges::find(_registrations, &handler, &Registration::handler);
    return found == _registrations.end() ? nullptr : &*found;
}

std::expected<void, NetError> PollBackend::attach(ReadinessHandler& handler)
{
    if (handler.handle == platform::InvalidHandle)
        return std::unexpected { makeNetError(NetErrorCode::BadHandle, 0, "PollBackend::attach") };
    // Refused by name rather than handed to the kernel: the handle of a completion registration is
    // the address of an overlapped operation, and this backend lends no port to issue one on.
    if (handler.kind == HandleKind::Completion)
        return std::unexpected { makeNetError(NetErrorCode::Unsupported,
                                              0,
                                              "PollBackend::attach: HandleKind::Completion needs a "
                                              "completion port, and this backend has none") };
    if (find(handler) != nullptr)
        return std::unexpected { makeNetError(
            NetErrorCode::BadHandle, 0, "PollBackend::attach: handler is already attached") };

    // poll(2) has no kernel-side set, so attaching costs nothing and can only fail on
    // the two checks above. Interest arrives through setInterest, as it does on the
    // backends where the split is forced by the kernel.
    _registrations.push_back(Registration { .handler = &handler, .interest = Interest::None });
    return {};
}

std::expected<void, NetError> PollBackend::setInterest(ReadinessHandler& handler, Interest interest)
{
    auto* const registration = find(handler);
    if (registration == nullptr)
        return std::unexpected { makeNetError(
            NetErrorCode::BadHandle, 0, "PollBackend::setInterest: handler is not attached") };
    registration->interest = interest;
    return {};
}

void PollBackend::detach(ReadinessHandler& handler) noexcept
{
    std::erase_if(_registrations,
                  [&handler](Registration const& entry) { return entry.handler == &handler; });
    // ... and out of the batch a wait in flight is walking, which erasing above does
    // nothing about: the batch holds its own pointers, taken before any callback ran.
    _batch.withdraw(handler);
}

WaitResult PollBackend::wait(std::optional<platform::SteadyDuration> timeout)
{
    // Rebuilt per wait rather than cached: poll(2) hands the kernel the whole set
    // every time anyway, so a cache would save only the copy and would have to be
    // invalidated by attach, detach and setInterest alike. Thread-local so the
    // rebuild costs no allocation after the first wait, and so `pollfd` stays out of
    // the header.
    static thread_local auto fds = std::vector<pollfd> {};
    fds.clear();
    fds.reserve(_registrations.size());
    for (auto const& entry: _registrations)
        // A muted registration is submitted with a NEGATIVE descriptor, which poll(2)
        // ignores and reports 0 revents for. Submitting the real descriptor with
        // events == 0 does NOT mute it: the kernel reports POLLHUP/POLLERR/POLLNVAL
        // whatever was asked for, so a muted registration whose peer hung up would be
        // routed as a failure and wake the flow the caller asked to be silent. The
        // entry is kept rather than skipped, because the routing below pairs fds[i]
        // with _registrations[i].
        fds.push_back(pollfd { .fd = entry.interest == Interest::None ? -1 : entry.handler->handle,
                               .events = toPollEvents(entry.interest),
                               .revents = 0 });

    auto const ready = ::poll(fds.data(), static_cast<nfds_t>(fds.size()), detail::toTimeoutMillis(timeout));
    if (ready <= 0)
        // 0: timed out. <0: EINTR or an error — re-poll on the next wait, since
        // level-triggered descriptors re-report readiness and a persistent error
        // arrives as the descriptor's own POLLERR there.
        return WaitResult {};

    for (auto const index: std::views::iota(std::size_t { 0 }, _registrations.size()))
        if (auto const observed = fromPollRevents(fds[index].revents); observed != Readiness::None)
            _batch.add(*_registrations[index].handler, observed);

    return WaitResult { .dispatched = _batch.dispatch() };
}

} // namespace core::net
