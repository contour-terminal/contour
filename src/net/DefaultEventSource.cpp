// SPDX-License-Identifier: Apache-2.0
#include <net/DefaultEventSource.hpp>

#include <memory>

#include <net/PollEventSource.hpp>

#ifdef __linux__
    #include <net/EpollEventSource.hpp>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    #include <net/KqueueEventSource.hpp>
#endif

namespace net
{

EventSourceKind preferredEventSourceKind() noexcept
{
#ifdef __linux__
    return EventSourceKind::Epoll;
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    return EventSourceKind::Kqueue;
#else
    return EventSourceKind::Poll;
#endif
}

std::unique_ptr<EventSource> makeEventSource(EventSourceKind kind)
{
    switch (kind)
    {
        case EventSourceKind::Poll: return std::make_unique<PollEventSource>();

        case EventSourceKind::Epoll: {
#ifdef __linux__
            auto source = std::make_unique<EpollEventSource>();
            // A source whose kernel object failed to materialize would refuse every
            // attach; report it as unavailable so the caller can fall back.
            return source->good() ? std::unique_ptr<EventSource> { std::move(source) } : nullptr;
#else
            return nullptr;
#endif
        }

        case EventSourceKind::Kqueue: {
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
            auto source = std::make_unique<KqueueEventSource>();
            return source->good() ? std::unique_ptr<EventSource> { std::move(source) } : nullptr;
#else
            return nullptr;
#endif
        }
    }
    // Every enumerator returns above, so this is only reached for a value outside
    // the enumeration. Report it as unavailable rather than quietly substituting a
    // working poll source: a new enumerator added without a case here must surface
    // as a null (and fall back explicitly in makeDefaultEventSource), not be masked.
    return nullptr;
}

std::unique_ptr<EventSource> makeDefaultEventSource()
{
    if (auto native = makeEventSource(preferredEventSourceKind()))
        return native;
    // poll(2) is always available and behaviourally identical, just costlier.
    return std::make_unique<PollEventSource>();
}

} // namespace net
