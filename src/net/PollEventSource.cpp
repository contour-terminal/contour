// SPDX-License-Identifier: Apache-2.0
#include <net/PollEventSource.h>

#ifdef _WIN32
    #include <windows.h>
#else
    #include <poll.h>
#endif

namespace net
{

#ifndef _WIN32

namespace
{
    /// Translates a readiness interest mask into poll(2) event bits.
    /// @param interest The interest to translate.
    /// @return The corresponding POLLIN/POLLOUT bitmask.
    [[nodiscard]] short toPollEvents(FdInterest interest) noexcept
    {
        short events = 0;
        if (hasInterest(interest, FdInterest::Read))
            events |= POLLIN;
        if (hasInterest(interest, FdInterest::Write))
            events |= POLLOUT;
        return events;
    }

    /// Routes a registered fd's poll(2) revents into a wait outcome's ready-token
    /// lists. Read-readiness includes HUP/ERR/NVAL so a parked reader is resumed to
    /// observe EOF rather than the pump spinning.
    /// @param token The registration's token.
    /// @param revents The revents poll(2) reported for the fd.
    /// @param outcome The outcome to append the token to (readyRead / readyWrite).
    void routePollRevents(FdToken token, short revents, WaitOutcome& outcome)
    {
        if ((revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0)
            outcome.readyRead.push_back(token);
        if ((revents & POLLOUT) != 0)
            outcome.readyWrite.push_back(token);
    }
} // namespace

WaitOutcome PollEventSource::wait(int timeoutMs)
{
    auto const& registrations = _registry.registrations();
    static thread_local std::vector<pollfd> fds;
    fds.clear();
    for (auto const& reg: registrations)
        fds.push_back({ .fd = reg.fd, .events = toPollEvents(reg.interest), .revents = 0 });

    auto outcome = WaitOutcome {};

    // Nothing to watch: honour the timeout (a parked timer supplies a finite one) so
    // the loop's timer can still fire; a negative timeout with no fds would block
    // forever, so report it as a benign timeout instead.
    if (fds.empty())
    {
        if (timeoutMs > 0)
            ::poll(nullptr, 0, timeoutMs);
        return outcome;
    }

    auto const result = ::poll(fds.data(), static_cast<nfds_t>(fds.size()), timeoutMs);
    if (result <= 0)
        // 0: timed out. <0: EINTR or error — re-poll next pump (level-triggered fds
        // re-report readiness); a persistent error surfaces as the fd's HUP below on
        // the next successful poll. Either way, nothing ready this round.
        return outcome;

    for (std::size_t i = 0; i < registrations.size(); ++i)
        routePollRevents(registrations[i].token, fds[i].revents, outcome);
    return outcome;
}

#else // _WIN32

WaitOutcome PollEventSource::wait(int timeoutMs)
{
    auto const& registrations = _registry.registrations();
    static thread_local std::vector<HANDLE> handles;
    handles.clear();
    for (auto const& reg: registrations)
        if (reg.fd != nullptr && reg.fd != InvalidHandle && reg.interest != FdInterest::None)
            handles.push_back(reg.fd);

    auto outcome = WaitOutcome {};

    auto const timeout = (timeoutMs < 0) ? INFINITE : static_cast<DWORD>(timeoutMs);
    if (handles.empty())
    {
        if (timeoutMs > 0)
            Sleep(static_cast<DWORD>(timeoutMs));
        return outcome;
    }

    auto const waitResult =
        WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE, timeout);
    if (waitResult == WAIT_TIMEOUT || waitResult == WAIT_FAILED)
        return outcome;

    auto const signalled = [](HANDLE handle) {
        return handle != nullptr && WaitForSingleObject(handle, 0) == WAIT_OBJECT_0;
    };
    for (auto const& reg: registrations)
    {
        if (!signalled(reg.fd))
            continue;
        if (hasInterest(reg.interest, FdInterest::Read))
            outcome.readyRead.push_back(reg.token);
        if (hasInterest(reg.interest, FdInterest::Write))
            outcome.readyWrite.push_back(reg.token);
    }
    return outcome;
}

#endif

} // namespace net
