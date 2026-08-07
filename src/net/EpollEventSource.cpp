// SPDX-License-Identifier: Apache-2.0
#include <net/EpollEventSource.hpp>

#ifdef __linux__

    #include <sys/epoll.h>

    #include <array>
    #include <span>

    #include <unistd.h>

namespace net
{

namespace
{
    /// The largest number of ready events one epoll_wait call reports. A wait that
    /// fills the batch simply reports the rest on the next call — level-triggered
    /// interest means nothing is lost by capping it.
    constexpr std::size_t ReadyBatchSize = 64;

    /// Translates a readiness interest mask into epoll event bits.
    /// @param interest The interest to translate.
    /// @return The corresponding EPOLLIN/EPOLLOUT bits.
    [[nodiscard]] std::uint32_t toEpollEvents(FdInterest interest) noexcept
    {
        auto events = std::uint32_t { 0 };
        if (hasInterest(interest, FdInterest::Read))
            events |= EPOLLIN;
        if (hasInterest(interest, FdInterest::Write))
            events |= EPOLLOUT;
        return events;
    }

    /// Routes an fd's reported epoll events into a wait outcome's token lists.
    ///
    /// EPOLLHUP / EPOLLERR are reported as readable — like poll(2)'s POLLHUP, they
    /// must wake a parked reader so it can observe EOF rather than the loop spinning.
    /// @param token The registration's token.
    /// @param events The events epoll reported.
    /// @param outcome The outcome to append the token to.
    void routeEpollEvents(FdToken token, std::uint32_t events, WaitOutcome& outcome)
    {
        if ((events & (EPOLLIN | EPOLLHUP | EPOLLERR)) != 0)
            outcome.readyRead.push_back(token);
        if ((events & EPOLLOUT) != 0)
            outcome.readyWrite.push_back(token);
    }
} // namespace

EpollEventSource::EpollEventSource() noexcept: _epollFd { ::epoll_create1(EPOLL_CLOEXEC) }
{
}

EpollEventSource::~EpollEventSource()
{
    // Close the duplicates this source owns; the caller's own descriptors are not ours.
    for (auto const& [token, owned]: _registered)
        ::close(owned);
    if (_epollFd >= 0)
        ::close(_epollFd);
}

bool EpollEventSource::applyInterest(NativeHandle fd, FdInterest interest, FdToken token) const noexcept
{
    auto event = epoll_event {};
    event.events = toEpollEvents(interest);
    // The token, not the fd, identifies the registration: two registrations may
    // share an fd, and the token is what the loop maps back to a parked coroutine.
    event.data.u64 = token.value;
    return ::epoll_ctl(_epollFd, EPOLL_CTL_ADD, fd, &event) == 0;
}

FdToken EpollEventSource::attach(NativeHandle fd, FdInterest interest)
{
    if (_epollFd < 0 || fd == InvalidHandle)
        return FdToken::invalid();

    auto const token = _registry.attach(fd, interest);
    if (!token)
        return FdToken::invalid();

    // Register a private duplicate: an epoll set is keyed by descriptor, so adding
    // the same one twice fails with EEXIST, while poll(2) happily takes two entries.
    // The registry allows two registrations on one descriptor, so without this the
    // backends would disagree about what is even registrable.
    auto const owned = ::dup(fd);

    // A failed registration must not leave the registry claiming the fd is watched:
    // the awaiting flow has to fail rather than park on an interest the kernel never
    // accepted, which nothing could ever resume.
    if (owned < 0 || !applyInterest(owned, interest, token))
    {
        if (owned >= 0)
            ::close(owned);
        _registry.detach(token);
        return FdToken::invalid();
    }

    _registered.emplace(token.value, owned);
    return token;
}

void EpollEventSource::detach(FdToken token)
{
    if (auto const it = _registered.find(token.value); it != _registered.end())
    {
        auto event = epoll_event {};
        ::epoll_ctl(_epollFd, EPOLL_CTL_DEL, it->second, &event);
        ::close(it->second); // the duplicate this source owns, not the caller's fd
        _registered.erase(it);
    }
    _registry.detach(token);
}

WaitOutcome EpollEventSource::wait(int timeoutMs)
{
    auto outcome = WaitOutcome {};
    if (_epollFd < 0)
        return outcome;

    // Nothing to watch: honour the timeout so a parked timer can still fire, and
    // treat an infinite timeout as a benign timeout rather than blocking forever
    // with no wakeable source. Mirrors PollEventSource.
    auto events = std::array<epoll_event, ReadyBatchSize> {};
    if (_registry.size() == 0)
    {
        // epoll_wait requires a valid buffer even when nothing can be reported, so
        // wait on the real array with room for one event rather than passing null.
        if (timeoutMs > 0)
            ::epoll_wait(_epollFd, events.data(), 1, timeoutMs);
        return outcome;
    }

    auto const ready = ::epoll_wait(_epollFd, events.data(), static_cast<int>(events.size()), timeoutMs);
    if (ready <= 0)
        // 0: timed out. <0: EINTR or error — nothing ready this round. Level-triggered
        // interest re-reports a still-ready fd on the next wait, so nothing is lost.
        return outcome;

    for (auto const& event: std::span { events.data(), static_cast<std::size_t>(ready) })
        routeEpollEvents(FdToken { event.data.u64 }, event.events, outcome);
    return outcome;
}

} // namespace net

#endif // __linux__
