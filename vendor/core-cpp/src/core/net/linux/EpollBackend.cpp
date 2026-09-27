// SPDX-License-Identifier: Apache-2.0
#include <core/net/linux/EpollBackend.hpp>

#include <core/net/detail/WaitTimeout.hpp>

#include <sys/epoll.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>

#include <unistd.h>

namespace core::net
{

namespace
{
    /// Translates a readiness interest into epoll event bits.
    /// @param interest What the registration is watched for.
    /// @return The corresponding EPOLLIN/EPOLLOUT bits.
    [[nodiscard]] std::uint32_t toEpollEvents(Interest interest) noexcept
    {
        auto events = std::uint32_t { 0 };
        if (hasInterest(interest, Interest::Read))
            events |= EPOLLIN;
        if (hasInterest(interest, Interest::Write))
            events |= EPOLLOUT;
        return events;
    }

    /// Translates epoll's answer for one registration into the portable vocabulary
    /// @c selectReadinessCallback routes on.
    /// @param events The `epoll_event::events` bitset as the kernel reported it.
    /// @return What was observed.
    [[nodiscard]] Readiness fromEpollEvents(std::uint32_t events) noexcept
    {
        auto observed = Readiness::None;
        if ((events & static_cast<std::uint32_t>(EPOLLIN)) != 0)
            observed = observed | Readiness::Readable;
        if ((events & static_cast<std::uint32_t>(EPOLLOUT)) != 0)
            observed = observed | Readiness::Writable;
        if ((events & static_cast<std::uint32_t>(EPOLLERR | EPOLLHUP)) != 0)
            observed = observed | Readiness::Failed;
        return observed;
    }
} // namespace

struct EpollBackend::ReadyEvents
{
    std::array<epoll_event, ReadyBatchSize> entries {};
};

EpollBackend::EpollBackend():
    _events { std::make_unique<ReadyEvents>() }, _epollFd { ::epoll_create1(EPOLL_CLOEXEC) }
{
    // A backend whose epoll instance failed to materialise reports `good() == false`
    // and is discarded by makeBackend, which falls back to poll(2).
    if (_epollFd < 0)
        return;

    // The wakeup channel goes through the ordinary attach/setInterest path, so the
    // path shutdown depends on is the path every case exercises. Its failure is not
    // survivable and not reportable from here: a loop that cannot be woken across
    // threads deadlocks at shutdown with no message.
    if (!attach(_wakeup.handler()) || !setInterest(_wakeup.handler(), Interest::Read))
        throw std::runtime_error("core::net::EpollBackend: cannot register the wakeup channel");
}

EpollBackend::~EpollBackend()
{
    // Close only the duplicates this backend made; the caller's own descriptors are
    // registered directly and are not ours to close.
    for (auto const& [handler, registration]: _registrations)
        if (registration.owned)
            ::close(registration.watched);
    if (_epollFd >= 0)
        ::close(_epollFd);
}

std::expected<void, NetError> EpollBackend::attach(ReadinessHandler& handler)
{
    if (_epollFd < 0)
        return std::unexpected { makeNetError(
            NetErrorCode::SystemError, 0, "EpollBackend::attach: no epoll instance") };
    if (handler.handle == platform::InvalidHandle)
        return std::unexpected { makeNetError(NetErrorCode::BadHandle, 0, "EpollBackend::attach") };
    // Refused by name rather than handed to the kernel: the handle of a completion registration is
    // the address of an overlapped operation, and this backend lends no port to issue one on.
    if (handler.kind == HandleKind::Completion)
        return std::unexpected { makeNetError(NetErrorCode::Unsupported,
                                              0,
                                              "EpollBackend::attach: HandleKind::Completion needs a "
                                              "completion port, and this backend has none") };
    if (_registrations.contains(&handler))
        return std::unexpected { makeNetError(
            NetErrorCode::BadHandle, 0, "EpollBackend::attach: handler is already attached") };

    // Nothing reaches the kernel here. epoll HAS an "add with no interest" operation,
    // but a descriptor sitting in the set with `events == 0` is not silent: EPOLLHUP
    // and EPOLLERR are reported whatever was asked for, so a muted registration left
    // in the set would be reported ready on every single wait, waking a flow the
    // caller asked to be silent and spinning the pump while it does. Interest
    // therefore decides membership, and `Interest::None` means out of the set.
    _registrations.emplace(
        &handler,
        Registration {
            .interest = Interest::None, .watched = handler.handle, .owned = false, .armed = false });
    return {};
}

std::expected<void, NetError> EpollBackend::arm(ReadinessHandler& handler,
                                                Registration& registration,
                                                Interest interest)
{
    if (!registration.armed && !registration.owned)
    {
        // A second registration on one descriptor needs a private one: an epoll set is
        // keyed by descriptor and refuses the same one twice with EEXIST, while poll(2)
        // simply takes two entries. Only a genuine duplicate pays for it — see
        // Registration::watched for why a dup() is not the default.
        auto const duplicate = std::ranges::any_of(_registrations, [&handler](auto const& entry) noexcept {
            return entry.second.armed && entry.second.watched == handler.handle;
        });
        if (duplicate)
        {
            auto const copy = ::dup(handler.handle);
            if (copy < 0)
                return std::unexpected { makeNetError(
                    NetErrorCode::SystemError, errno, "EpollBackend::setInterest: dup") };
            registration.watched = copy;
            registration.owned = true;
        }
    }

    auto event = epoll_event {};
    event.events = toEpollEvents(interest);
    // The handler, not the descriptor, identifies the registration: two registrations
    // may share a descriptor, and the handler is what the readiness is dispatched to.
    event.data.ptr = &handler;
    auto const operation = registration.armed ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
    if (::epoll_ctl(_epollFd, operation, registration.watched, &event) != 0)
        return std::unexpected { makeNetError(
            NetErrorCode::SystemError, errno, "EpollBackend::setInterest: epoll_ctl") };

    registration.armed = true;
    registration.interest = interest;
    return {};
}

void EpollBackend::disarm(Registration& registration) const noexcept
{
    if (!registration.armed)
        return;
    auto event = epoll_event {};
    ::epoll_ctl(_epollFd, EPOLL_CTL_DEL, registration.watched, &event);
    registration.armed = false;
    registration.interest = Interest::None;
}

std::expected<void, NetError> EpollBackend::setInterest(ReadinessHandler& handler, Interest interest)
{
    auto const found = _registrations.find(&handler);
    if (found == _registrations.end())
        return std::unexpected { makeNetError(
            NetErrorCode::BadHandle, 0, "EpollBackend::setInterest: handler is not attached") };

    if (interest == Interest::None)
    {
        disarm(found->second);
        return {};
    }
    return arm(handler, found->second, interest);
}

void EpollBackend::detach(ReadinessHandler& handler) noexcept
{
    if (auto const found = _registrations.find(&handler); found != _registrations.end())
    {
        disarm(found->second);
        if (found->second.owned)
            ::close(found->second.watched); // a duplicate we made, never the caller's
        _registrations.erase(found);
    }
    // ... and out of the batch a wait in flight is walking, which dropping the kernel
    // registration above does nothing about: `epoll_wait` has already written its
    // entry into the array this dispatch is walking.
    _batch.withdraw(handler);
}

WaitResult EpollBackend::wait(std::optional<platform::SteadyDuration> timeout)
{
    if (_epollFd < 0)
        return WaitResult {};

    auto const ready = ::epoll_wait(_epollFd,
                                    _events->entries.data(),
                                    static_cast<int>(_events->entries.size()),
                                    detail::toTimeoutMillis(timeout));
    if (ready <= 0)
        // 0: timed out. <0: EINTR or an error — nothing ready this round.
        // Level-triggered interest re-reports a still-ready descriptor on the next
        // wait, so nothing is lost.
        return WaitResult {};

    for (auto const& event: std::span { _events->entries.data(), static_cast<std::size_t>(ready) })
        _batch.add(*static_cast<ReadinessHandler*>(event.data.ptr), fromEpollEvents(event.events));

    return WaitResult { .dispatched = _batch.dispatch() };
}

} // namespace core::net
