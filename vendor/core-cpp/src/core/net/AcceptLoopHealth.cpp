// SPDX-License-Identifier: Apache-2.0
#include <core/net/AcceptLoopHealth.hpp>

#include <utility>

namespace core::net
{

void AcceptLoopHealth::subscribe(Listener listener)
{
    auto const guard = std::scoped_lock { _mutex };
    _listener = std::move(listener);
}

void AcceptLoopHealth::forward(AcceptLoopHealth& target)
{
    auto already = std::vector<SurfaceCondition> {};
    {
        auto const guard = std::scoped_lock { _mutex };
        _forward = &target;
        already = _conditions;
    }
    // A loop that reported before the process was assembled far enough to forward is still told.
    for (auto const& condition: already)
        target.record(AcceptLoopEvent {
            .surface = condition.surface, .line = condition.reason, .error = {}, .kind = condition.kind });
}

void AcceptLoopHealth::record(AcceptLoopEvent const& event)
{
    if (event.kind == AcceptLoopEventKind::Warning)
        return;
    auto listener = Listener {};
    AcceptLoopHealth* forward = nullptr;
    {
        auto const guard = std::scoped_lock { _mutex };
        // A surface holds at most one degraded entry: the one its current run reported.
        std::erase_if(_conditions, [&event](SurfaceCondition const& condition) {
            return condition.surface == event.surface && condition.kind == AcceptLoopEventKind::Degraded;
        });
        if (event.kind != AcceptLoopEventKind::Recovered && event.kind != AcceptLoopEventKind::Stopped)
            _conditions.push_back(
                SurfaceCondition { .surface = event.surface, .reason = event.line, .kind = event.kind });
        listener = _listener;
        forward = _forward;
    }
    // Outside the lock: a listener that takes another registry's lock, or reads this one back,
    // must not deadlock on it.
    if (listener)
        listener(event);
    if (forward != nullptr)
        forward->record(event);
}

std::vector<SurfaceCondition> AcceptLoopHealth::snapshot() const
{
    auto const guard = std::scoped_lock { _mutex };
    return _conditions;
}

} // namespace core::net
