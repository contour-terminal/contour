// SPDX-License-Identifier: Apache-2.0
#include <core/net/bsd/KqueueBackend.hpp>

#include <core/net/detail/WaitTimeout.hpp>

#include <sys/event.h>
#include <sys/types.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>

#include <unistd.h>

namespace core::net
{

namespace
{
    /// The largest number of ready events one `kevent()` reports. Level-triggered
    /// filters re-report a still-ready descriptor, so capping the batch loses nothing.
    constexpr std::size_t ReadyBatchSize = 64;

    /// One kqueue filter and whether the caller wants it armed. Lets the interest
    /// update express "arm it or drop it" once and drive both filters from a table
    /// rather than two hand-written EV_SET calls that must stay in sync.
    struct FilterInterest
    {
        std::int16_t filter; ///< EVFILT_READ or EVFILT_WRITE.
        bool wanted;         ///< True to arm the filter, false to drop it.
    };

    /// Converts a backend timeout into the timespec `kevent()` wants.
    /// @param timeout How long to wait, or nullopt for an indefinite wait.
    /// @return The timespec, or nullopt for an indefinite wait.
    [[nodiscard]] std::optional<timespec> toTimespec(std::optional<platform::SteadyDuration> timeout) noexcept
    {
        auto const millis = detail::toTimeoutMillis(timeout);
        if (millis < 0)
            return std::nullopt;
        auto const duration = std::chrono::milliseconds { millis };
        auto const seconds = std::chrono::duration_cast<std::chrono::seconds>(duration);
        auto const nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(duration - seconds);
        return timespec { .tv_sec = static_cast<time_t>(seconds.count()),
                          .tv_nsec = static_cast<long>(nanos.count()) };
    }
} // namespace

KqueueBackend::KqueueBackend(): _kq { ::kqueue() }
{
    // A backend whose kqueue failed to materialise reports `good() == false` and is
    // discarded by makeBackend, which falls back to poll(2).
    if (_kq < 0)
        return;

    // The wakeup channel goes through the ordinary attach/setInterest path, so the
    // path shutdown depends on is the path every case exercises. Its failure is not
    // survivable and not reportable from here: a loop that cannot be woken across
    // threads deadlocks at shutdown with no message.
    if (!attach(_wakeup.handler()) || !setInterest(_wakeup.handler(), Interest::Read))
        throw std::runtime_error("core::net::KqueueBackend: cannot register the wakeup channel");
}

KqueueBackend::~KqueueBackend()
{
    // Close only the duplicates this backend made; the caller's own descriptors are
    // armed directly and are not ours to close.
    for (auto const& [handler, registration]: _registrations)
        if (registration.owned)
            ::close(registration.watched);
    if (_kq >= 0)
        ::close(_kq);
}

std::expected<void, NetError> KqueueBackend::attach(ReadinessHandler& handler)
{
    if (_kq < 0)
        return std::unexpected { makeNetError(
            NetErrorCode::SystemError, 0, "KqueueBackend::attach: no kqueue") };
    if (handler.handle == platform::InvalidHandle)
        return std::unexpected { makeNetError(NetErrorCode::BadHandle, 0, "KqueueBackend::attach") };
    // Refused by name rather than handed to the kernel: the handle of a completion registration is
    // the address of an overlapped operation, and this backend lends no port to issue one on.
    if (handler.kind == HandleKind::Completion)
        return std::unexpected { makeNetError(NetErrorCode::Unsupported,
                                              0,
                                              "KqueueBackend::attach: HandleKind::Completion needs a "
                                              "completion port, and this backend has none") };
    if (_registrations.contains(&handler))
        return std::unexpected { makeNetError(
            NetErrorCode::BadHandle, 0, "KqueueBackend::attach: handler is already attached") };

    _registrations.emplace(
        &handler,
        Registration {
            .interest = Interest::None, .watched = handler.handle, .owned = false, .armed = false });
    return {};
}

std::expected<void, NetError> KqueueBackend::arm(ReadinessHandler& handler,
                                                 Registration& registration,
                                                 Interest interest)
{
    if (!registration.armed && !registration.owned)
    {
        // A second registration on one descriptor needs a private one — see
        // Registration::watched for why a dup() is not the default.
        auto const duplicate = std::ranges::any_of(_registrations, [&handler](auto const& entry) noexcept {
            return entry.second.armed && entry.second.watched == handler.handle;
        });
        if (duplicate)
        {
            auto const copy = ::dup(handler.handle);
            if (copy < 0)
                return std::unexpected { makeNetError(
                    NetErrorCode::SystemError, errno, "KqueueBackend::setInterest: dup") };
            registration.watched = copy;
            registration.owned = true;
        }
    }

    auto const interests = std::array<FilterInterest, 2> { {
        { .filter = EVFILT_READ, .wanted = hasInterest(interest, Interest::Read) },
        { .filter = EVFILT_WRITE, .wanted = hasInterest(interest, Interest::Write) },
    } };

    auto changes = std::array<struct kevent, interests.size()> {};
    for (auto const i: std::views::iota(std::size_t { 0 }, interests.size()))
        EV_SET(std::next(changes.data(), static_cast<std::ptrdiff_t>(i)),
               static_cast<uintptr_t>(registration.watched),
               interests[i].filter,
               (interests[i].wanted ? (EV_ADD | EV_ENABLE) : EV_DELETE) | EV_RECEIPT,
               0,
               0,
               // The handler, not the descriptor, identifies the registration back to
               // the dispatch: two registrations may share a descriptor.
               &handler);

    auto results = std::array<struct kevent, interests.size()> {};
    auto const applied = ::kevent(_kq,
                                  changes.data(),
                                  static_cast<int>(changes.size()),
                                  results.data(),
                                  static_cast<int>(results.size()),
                                  nullptr);
    if (applied < 0)
        return std::unexpected { makeNetError(
            NetErrorCode::SystemError, errno, "KqueueBackend::setInterest: kevent") };

    // With EV_RECEIPT the kernel reports one entry per submitted change, so anything
    // short means a change went unreported and the filter cannot be claimed armed.
    if (static_cast<std::size_t>(applied) != changes.size())
        return std::unexpected { makeNetError(
            NetErrorCode::SystemError, 0, "KqueueBackend::setInterest: short receipt") };

    // Every returned entry carries EV_ERROR and `data` holds the errno (0 when the
    // change applied cleanly). Results are matched back by filter rather than by
    // position, so the check does not depend on the kernel preserving changelist
    // order. ENOENT on a filter asked to be dropped just means it was not armed — the
    // normal steady state, not a failure.
    auto refused = 0;
    for (auto const& result: std::span { results.data(), static_cast<std::size_t>(applied) })
    {
        // EV_RECEIPT guarantees EV_ERROR on every entry. An entry without it is not
        // the receipt that was asked for, so treat it as a failure rather than assume
        // the change applied: parking on a filter the kernel never armed is
        // unresumable, and reporting that refusal is the whole reason setInterest
        // answers an expected rather than a token
        // ([fastcached#1054](https://github.com/LASTRADA-Software/fastcached/issues/1054)).
        if ((result.flags & EV_ERROR) == 0)
        {
            refused = EINVAL;
            break;
        }
        if (result.data == 0)
            continue;
        auto const row =
            std::ranges::find(interests, static_cast<std::int16_t>(result.filter), &FilterInterest::filter);
        if (result.data == ENOENT && row != interests.end() && !row->wanted)
            continue;
        refused = static_cast<int>(result.data);
        break;
    }
    if (refused != 0)
        return std::unexpected { makeNetError(
            NetErrorCode::SystemError, refused, "KqueueBackend::setInterest: kevent refused a filter") };

    registration.armed = true;
    registration.interest = interest;
    return {};
}

void KqueueBackend::disarm(Registration& registration) const noexcept
{
    if (!registration.armed)
        return;
    // EV_RECEIPT for the same reason as arm(): without an eventlist an ENOENT on the
    // first delete (a filter that was never armed) would abort the changelist and
    // leave the second filter registered against a registration that is going away.
    auto changes = std::array<struct kevent, 2> {};
    EV_SET(changes.data(),
           static_cast<uintptr_t>(registration.watched),
           EVFILT_READ,
           EV_DELETE | EV_RECEIPT,
           0,
           0,
           nullptr);
    EV_SET(std::next(changes.data()),
           static_cast<uintptr_t>(registration.watched),
           EVFILT_WRITE,
           EV_DELETE | EV_RECEIPT,
           0,
           0,
           nullptr);
    auto results = std::array<struct kevent, 2> {};
    static_cast<void>(::kevent(_kq,
                               changes.data(),
                               static_cast<int>(changes.size()),
                               results.data(),
                               static_cast<int>(results.size()),
                               nullptr));
    registration.armed = false;
    registration.interest = Interest::None;
}

std::expected<void, NetError> KqueueBackend::setInterest(ReadinessHandler& handler, Interest interest)
{
    auto const found = _registrations.find(&handler);
    if (found == _registrations.end())
        return std::unexpected { makeNetError(
            NetErrorCode::BadHandle, 0, "KqueueBackend::setInterest: handler is not attached") };

    if (interest == Interest::None)
    {
        disarm(found->second);
        return {};
    }
    return arm(handler, found->second, interest);
}

void KqueueBackend::detach(ReadinessHandler& handler) noexcept
{
    if (auto const found = _registrations.find(&handler); found != _registrations.end())
    {
        disarm(found->second);
        if (found->second.owned)
            ::close(found->second.watched); // a duplicate we made, never the caller's
        _registrations.erase(found);
    }
    // ... and out of the batch a wait in flight is walking, which dropping the filters
    // above does nothing about: `kevent()` has already written its entry into the
    // array this dispatch is walking.
    _batch.withdraw(handler);
}

WaitResult KqueueBackend::wait(std::optional<platform::SteadyDuration> timeout)
{
    if (_kq < 0)
        return WaitResult {};

    auto const deadline = toTimespec(timeout);
    auto const* const deadlinePtr = deadline.has_value() ? &*deadline : nullptr;

    auto events = std::array<struct kevent, ReadyBatchSize> {};
    auto const ready = ::kevent(_kq, nullptr, 0, events.data(), static_cast<int>(events.size()), deadlinePtr);
    if (ready <= 0)
        // 0: timed out. <0: EINTR or an error — nothing ready this round.
        // Level-triggered filters re-report a still-ready descriptor on the next wait.
        return WaitResult {};

    for (auto const& event: std::span { events.data(), static_cast<std::size_t>(ready) })
    {
        if (event.udata == nullptr)
            continue;
        // Routed by the filter that fired, not by EV_EOF: a registration watching one
        // direction would otherwise have the other direction's EOF reported as its
        // own. EV_EOF still reaches whoever is parked, because it arrives on that
        // side's own filter — the peer closing makes the read filter fire (EOF is
        // readable) and the write filter fire (the write can no longer block), which
        // is how poll(2)'s POLLHUP reaches both.
        auto observed = Readiness::None;
        if (event.filter == EVFILT_READ)
            observed = observed | Readiness::Readable;
        else if (event.filter == EVFILT_WRITE)
            observed = observed | Readiness::Writable;
        if ((event.flags & EV_ERROR) != 0)
            observed = observed | Readiness::Failed;
        if (observed == Readiness::None)
            continue;
        _batch.add(*static_cast<ReadinessHandler*>(event.udata), observed);
    }

    return WaitResult { .dispatched = _batch.dispatch() };
}

} // namespace core::net
