// SPDX-License-Identifier: Apache-2.0
#include <net/KqueueEventSource.hpp>

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)

    #include <sys/event.h>
    #include <sys/types.h>

    #include <algorithm>
    #include <array>
    #include <cerrno>
    #include <chrono>
    #include <iterator>
    #include <optional>
    #include <ranges>
    #include <span>

    #include <unistd.h>

namespace net
{

namespace
{
    /// The largest number of ready events one kevent() call reports. Level-triggered
    /// filters re-report a still-ready fd, so capping the batch loses nothing.
    constexpr std::size_t ReadyBatchSize = 64;

    /// One kqueue filter and whether the caller wants it armed. Lets the interest
    /// update express "arm it or drop it" once and drive both filters from a table
    /// rather than two hand-written EV_SET calls that must stay in sync.
    struct FilterInterest
    {
        std::int16_t filter; ///< EVFILT_READ or EVFILT_WRITE.
        bool wanted;         ///< True to arm the filter, false to drop it.
    };

    /// Converts a millisecond timeout into the timespec kevent() wants.
    /// @param timeoutMs -1 to block indefinitely, else the wait in milliseconds.
    /// @return The timespec, or nullopt for an indefinite wait.
    [[nodiscard]] std::optional<timespec> toTimespec(int timeoutMs) noexcept
    {
        if (timeoutMs < 0)
            return std::nullopt;
        auto const duration = std::chrono::milliseconds { timeoutMs };
        auto const seconds = std::chrono::duration_cast<std::chrono::seconds>(duration);
        auto const nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(duration - seconds);
        return timespec { .tv_sec = static_cast<time_t>(seconds.count()),
                          .tv_nsec = static_cast<long>(nanos.count()) };
    }
} // namespace

KqueueEventSource::KqueueEventSource() noexcept: _kq { ::kqueue() }
{
}

KqueueEventSource::~KqueueEventSource()
{
    if (_kq >= 0)
        ::close(_kq);
}

bool KqueueEventSource::applyInterest(NativeHandle fd, FdInterest interest, FdToken token) const noexcept
{
    if (_kq < 0 || fd == InvalidHandle)
        return false;

    auto const interests = std::array<FilterInterest, 2> { {
        { .filter = EVFILT_READ, .wanted = hasInterest(interest, FdInterest::Read) },
        { .filter = EVFILT_WRITE, .wanted = hasInterest(interest, FdInterest::Write) },
    } };

    auto changes = std::array<struct kevent, interests.size()> {};
    for (auto const i: std::views::iota(std::size_t { 0 }, interests.size()))
        EV_SET(std::next(changes.data(), static_cast<std::ptrdiff_t>(i)),
               static_cast<uintptr_t>(fd),
               interests[i].filter,
               (interests[i].wanted ? (EV_ADD | EV_ENABLE) : EV_DELETE) | EV_RECEIPT,
               0,
               0,
               // The token, not the fd, identifies the registration back to the loop.
               reinterpret_cast<void*>(static_cast<uintptr_t>(token.value)));

    auto results = std::array<struct kevent, interests.size()> {};
    auto const applied = ::kevent(_kq,
                                  changes.data(),
                                  static_cast<int>(changes.size()),
                                  results.data(),
                                  static_cast<int>(results.size()),
                                  nullptr);
    if (applied < 0)
        return false;

    // With EV_RECEIPT every returned entry carries EV_ERROR and `data` holds the
    // errno (0 when the change applied cleanly). Results are matched back by filter
    // rather than by position, so the check does not depend on the kernel preserving
    // changelist order. ENOENT on a filter we asked to drop just means it was not
    // armed — the normal steady state, not a failure.
    auto const reported = std::span { results.data(), static_cast<std::size_t>(applied) };
    return std::ranges::all_of(reported, [&interests](struct kevent const& result) noexcept {
        // EV_RECEIPT sets EV_ERROR on every entry; `data` carries the errno, 0 when
        // the change applied cleanly. An entry without EV_ERROR is not a report.
        if ((result.flags & EV_ERROR) == 0 || result.data == 0)
            return true;
        auto const row =
            std::ranges::find(interests, static_cast<std::int16_t>(result.filter), &FilterInterest::filter);
        return result.data == ENOENT && row != interests.end() && !row->wanted;
    });
}

void KqueueEventSource::dropFilters(NativeHandle fd) const noexcept
{
    if (_kq < 0 || fd == InvalidHandle)
        return;
    // EV_RECEIPT for the same reason as applyInterest: without an eventlist an
    // ENOENT on the first delete (a filter that was never armed) would abort the
    // changelist and leave the second filter registered against a registration
    // that is going away.
    auto changes = std::array<struct kevent, 2> {};
    EV_SET(changes.data(), static_cast<uintptr_t>(fd), EVFILT_READ, EV_DELETE | EV_RECEIPT, 0, 0, nullptr);
    EV_SET(std::next(changes.data()),
           static_cast<uintptr_t>(fd),
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
}

FdToken KqueueEventSource::attach(NativeHandle fd, FdInterest interest)
{
    if (_kq < 0 || fd == InvalidHandle)
        return FdToken::invalid();

    auto const token = _registry.attach(fd, interest);
    if (!token)
        return FdToken::invalid();

    // A failed arm must not leave the registry claiming the fd is watched: the
    // awaiting flow has to fail rather than park on a filter the kernel never
    // armed, which nothing could ever resume.
    if (!applyInterest(fd, interest, token))
    {
        _registry.detach(token);
        return FdToken::invalid();
    }

    _registered.emplace(token.value, fd);
    return token;
}

void KqueueEventSource::detach(FdToken token)
{
    if (auto const it = _registered.find(token.value); it != _registered.end())
    {
        dropFilters(it->second);
        _registered.erase(it);
    }
    _registry.detach(token);
}

WaitOutcome KqueueEventSource::wait(int timeoutMs)
{
    auto outcome = WaitOutcome {};
    if (_kq < 0)
        return outcome;

    auto const timeout = toTimespec(timeoutMs);
    auto const* const timeoutPtr = timeout.has_value() ? &*timeout : nullptr;

    // Nothing to watch: honour the timeout so a parked timer can still fire, and
    // treat an infinite timeout as a benign timeout rather than blocking forever
    // with no wakeable source. Mirrors PollEventSource.
    auto events = std::array<struct kevent, ReadyBatchSize> {};
    if (_registry.size() == 0)
    {
        if (timeoutMs > 0)
            static_cast<void>(::kevent(_kq, nullptr, 0, events.data(), 1, timeoutPtr));
        return outcome;
    }

    auto const ready = ::kevent(_kq, nullptr, 0, events.data(), static_cast<int>(events.size()), timeoutPtr);
    if (ready <= 0)
        // 0: timed out. <0: EINTR or error — nothing ready this round. Level-triggered
        // filters re-report a still-ready fd on the next wait, so nothing is lost.
        return outcome;

    for (auto const& event: std::span { events.data(), static_cast<std::size_t>(ready) })
    {
        auto const token = FdToken { static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(event.udata)) };
        // EV_EOF is reported as readable so a parked reader wakes and observes EOF,
        // matching how poll(2)'s POLLHUP is routed.
        if (event.filter == EVFILT_READ || (event.flags & EV_EOF) != 0)
            outcome.readyRead.push_back(token);
        if (event.filter == EVFILT_WRITE)
            outcome.readyWrite.push_back(token);
    }
    return outcome;
}

} // namespace net

#endif // kqueue platforms
