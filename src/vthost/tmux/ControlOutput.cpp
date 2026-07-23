// SPDX-License-Identifier: Apache-2.0
#include <vthost/tmux/ControlOutput.h>

#include <algorithm>
#include <format>
#include <iterator>
#include <utility>

namespace vthost::tmux
{

std::string escapeOutput(std::string_view bytes)
{
    auto out = std::string {};
    out.reserve(bytes.size());
    for (auto const raw: bytes)
    {
        auto const byte = static_cast<unsigned char>(raw);
        // tmux escapes ONLY control bytes and the backslash; 0x7F and every
        // byte >= 0x80 (UTF-8 continuation included) pass through untouched.
        if (byte < 0x20 || byte == '\\')
            std::format_to(std::back_inserter(out), "\\{:03o}", byte);
        else
            out += raw;
    }
    return out;
}

ControlOutput::ControlOutput(std::function<void(std::string)> sink, ControlOutputLimits limits):
    _sink(std::move(sink)), _limits(limits)
{
}

void ControlOutput::enqueueNotification(std::string line)
{
    _allBlocks.push_back(std::make_unique<Block>(
        Block { .pane = std::nullopt, .data = std::move(line), .offset = 0, .enqueued = {} }));
    flushReadyNotifications();
}

void ControlOutput::enqueueOutput(std::uint64_t pane,
                                  std::string_view bytes,
                                  std::chrono::steady_clock::time_point now)
{
    if (bytes.empty())
        return;
    // A paused pane's output is dropped at the source until %continue, exactly
    // like tmux's CONTROL_PANE_PAUSED gate; a disabled pane (-A pane:off,
    // CONTROL_PANE_OFF) is dropped the same way but without any handshake.
    if (auto const it = _paused.find(pane); it != _paused.end() && it->second)
        return;
    if (auto const it = _disabled.find(pane); it != _disabled.end() && it->second)
        return;

    auto block = std::make_unique<Block>(
        Block { .pane = pane, .data = std::string { bytes }, .offset = 0, .enqueued = now });
    _paneQueues[pane].push_back(block.get());
    _allBlocks.push_back(std::move(block));
}

void ControlOutput::flushReadyNotifications()
{
    while (!_allBlocks.empty())
    {
        auto& front = *_allBlocks.front();
        if (front.pane.has_value())
        {
            if (front.offset < front.data.size())
                return; // an unfinished output block gates everything behind it
            _allBlocks.pop_front();
            continue;
        }
        _sink(front.data + "\n");
        _allBlocks.pop_front();
    }
}

std::vector<std::uint64_t> ControlOutput::panesWithPending() const
{
    auto panes = std::vector<std::uint64_t> {};
    for (auto const& [pane, queue]: _paneQueues)
        if (!queue.empty())
            panes.push_back(pane);
    std::ranges::sort(panes); // deterministic fairness order
    return panes;
}

void ControlOutput::discardPaneOutput(std::uint64_t pane)
{
    auto const it = _paneQueues.find(pane);
    if (it == _paneQueues.end())
        return;
    // Mark every pending block as fully emitted so the all-queue advances past
    // them; the blocks are destroyed by the flush, keeping single ownership.
    for (auto* block: it->second)
        block->offset = block->data.size();
    it->second.clear();
}

void ControlOutput::continuePane(std::uint64_t pane)
{
    auto const it = _paused.find(pane);
    if (it == _paused.end() || !it->second)
        return;
    it->second = false;
    enqueueNotification(std::format("%continue %{}", pane));
}

void ControlOutput::pausePane(std::uint64_t pane)
{
    if (isPaused(pane))
        return;
    _paused[pane] = true;
    discardPaneOutput(pane);
    enqueueNotification(std::format("%pause %{}", pane));
}

void ControlOutput::setPaneEnabled(std::uint64_t pane, bool enabled)
{
    _disabled[pane] = !enabled;
    if (!enabled)
        discardPaneOutput(pane);
}

bool ControlOutput::isPaused(std::uint64_t pane) const
{
    auto const it = _paused.find(pane);
    return it != _paused.end() && it->second;
}

std::size_t ControlOutput::pendingBytes(std::uint64_t pane) const
{
    auto const it = _paneQueues.find(pane);
    if (it == _paneQueues.end())
        return 0;
    auto total = std::size_t { 0 };
    for (auto const* block: it->second)
        total += block->data.size() - block->offset;
    return total;
}

void ControlOutput::pump(std::size_t buffered, std::chrono::steady_clock::time_point now)
{
    flushReadyNotifications();

    // Pause panes whose oldest unwritten output got too old (the client is not
    // keeping up): drop their backlog and tell them once.
    if (_pauseAfter)
    {
        for (auto& [pane, queue]: _paneQueues)
        {
            if (queue.empty() || isPaused(pane))
                continue;
            if (now - queue.front()->enqueued > *_pauseAfter)
                pausePane(pane);
        }
    }

    auto const panes = panesWithPending();
    if (panes.empty())
    {
        flushReadyNotifications();
        return;
    }

    // tmux's budget (control.c:778-780): the remaining buffer headroom, divided
    // fairly, divided by 3 for worst-case octal expansion, floored at the
    // write minimum so a congested connection still makes progress.
    auto const headroom = _limits.bufferHigh > buffered ? _limits.bufferHigh - buffered : 0;
    auto const budget = std::max(headroom / panes.size() / 3, _limits.writeMinimum);

    for (auto const pane: panes)
    {
        auto& queue = _paneQueues[pane];
        auto emitted = std::size_t { 0 };
        while (!queue.empty() && emitted < budget)
        {
            auto* block = queue.front();
            auto const chunk = std::min(budget - emitted, block->data.size() - block->offset);
            auto const bytes = std::string_view { block->data }.substr(block->offset, chunk);
            if (_extendedOutput)
            {
                // The single age field (milliseconds since the block was
                // queued) before " : " — control.c:653-658.
                auto const age = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::max(now - block->enqueued, std::chrono::steady_clock::duration::zero()));
                _sink(std::format("%extended-output %{} {} : {}\n", pane, age.count(), escapeOutput(bytes)));
            }
            else
                _sink(std::format("%output %{} {}\n", pane, escapeOutput(bytes)));
            block->offset += chunk;
            emitted += chunk;
            if (block->offset == block->data.size())
                queue.pop_front();
        }
    }

    // Completions may have unblocked notifications queued behind them.
    flushReadyNotifications();
}

} // namespace vthost::tmux
