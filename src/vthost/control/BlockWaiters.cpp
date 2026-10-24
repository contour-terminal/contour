// SPDX-License-Identifier: Apache-2.0
#include <vthost/control/BlockWaiters.hpp>

#include <algorithm>
#include <format>
#include <iterator>
#include <tuple>
#include <utility>

namespace vthost::control
{

template <typename Matches>
std::vector<BlockWaiters::Waiter> BlockWaiters::take(Matches const& matches)
{
    auto taken = std::vector<Waiter> {};
    auto const matched =
        std::ranges::stable_partition(_waiters, [&](Waiter const& waiter) { return !matches(waiter); });
    std::ranges::move(matched, std::back_inserter(taken));
    _waiters.erase(matched.begin(), matched.end());
    for (auto const& waiter: taken)
        if (auto const held = _perSink.find(waiter.sink); held != _perSink.end() && --held->second == 0)
            _perSink.erase(held);
    return taken;
}

std::expected<void, vtcontrol::ControlError> BlockWaiters::add(Waiter waiter)
{
    auto& count = _perSink[waiter.sink];
    if (count >= _maxPerSink)
    {
        auto error = vtcontrol::ControlError {
            .code = vtcontrol::ControlErrorCode::LimitReached,
            .message = std::format("this connection waits for {} blocks, as many as it may", count),
            .candidates = {}
        };
        if (count == 0)
            _perSink.erase(waiter.sink);
        waiter.done(std::unexpected(error));
        return std::unexpected(std::move(error));
    }
    ++count;
    _waiters.push_back(std::move(waiter));
    return {};
}

std::expected<void, vtcontrol::ControlError> BlockWaiters::waitFor(
    vtcontrol::EventSink& sink, vtcontrol::BlockRef block, vtcontrol::Completion<vtcontrol::BlockInfo> done)
{
    return add(Waiter { .sink = &sink,
                        .pane = block.pane,
                        .from = block.serial,
                        .to = block.serial,
                        .done = std::move(done) });
}

std::expected<void, vtcontrol::ControlError> BlockWaiters::waitForNext(
    vtcontrol::EventSink& sink,
    vtcontrol::Uuid pane,
    uint64_t afterSerial,
    vtcontrol::Completion<vtcontrol::BlockInfo> done)
{
    return add(Waiter { .sink = &sink,
                        .pane = pane,
                        .from = afterSerial + 1,
                        .to = std::numeric_limits<uint64_t>::max(),
                        .done = std::move(done) });
}

std::size_t BlockWaiters::countOf(vtcontrol::EventSink const& sink) const noexcept
{
    auto const held = _perSink.find(&sink);
    return held != _perSink.end() ? held->second : 0;
}

void BlockWaiters::blockEnded(vtcontrol::BlockInfo const& block)
{
    auto const ref = vtcontrol::parseBlockRef(block.id);
    if (!ref)
        return;
    for (auto& waiter: take([&](Waiter const& waiter) {
             return waiter.pane == ref->pane && waiter.from <= ref->serial && ref->serial <= waiter.to;
         }))
        waiter.done(block);
}

void BlockWaiters::paneClosed(vtcontrol::Uuid pane)
{
    auto const id =
        vtcontrol::toString(vtcontrol::ObjectRef { .kind = vtcontrol::ObjectKind::Pane, .uuid = pane });
    for (auto& waiter: take([&](Waiter const& waiter) { return waiter.pane == pane; }))
        waiter.done(std::unexpected(vtcontrol::ControlError { .code = vtcontrol::ControlErrorCode::NotFound,
                                                              .message = std::format("pane {} closed", id),
                                                              .candidates = {} }));
}

void BlockWaiters::forget(vtcontrol::EventSink& sink)
{
    std::ignore = take([&](Waiter const& waiter) { return waiter.sink == &sink; });
}

} // namespace vthost::control
