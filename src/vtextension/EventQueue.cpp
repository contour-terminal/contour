// SPDX-License-Identifier: Apache-2.0
#include <vtextension/EventQueue.hpp>

#include <algorithm>
#include <functional>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

// Every atomic operation on the stack, the slots and the user-variable slots is sequentially
// consistent (the default): a runner clears its "drain scheduled" flag and then takes the stack, while
// a producer pushes and then tests that flag. With anything weaker, each side's store could be
// reordered after its load, both would miss the other, and the event would sit undrained.

namespace vtextension
{

namespace
{
    /// @return Where a user variable's name starts probing for its slot.
    [[nodiscard]] std::size_t userVarKey(std::string_view name) noexcept
    {
        return std::hash<std::string_view> {}(name);
    }

    /// @return The name of the user variable a node of the user-variable kind carries.
    [[nodiscard]] std::string const& userVarName(Event const& event) noexcept
    {
        return std::get<UserVarChanged>(event).name;
    }
} // namespace

EventQueue::EventQueue(EventQueueLimits limits):
    _limits { limits }, _userVars { std::make_unique<UserVarSlot[]>(limits.userVarNames) }
{
}

EventQueue::~EventQueue()
{
    for (auto& slot: _slots)
        delete slot.exchange(nullptr);
    for (auto const index: std::views::iota(std::size_t { 0 }, _limits.userVarNames))
    {
        delete _userVars[index].pending.exchange(nullptr);
        delete _userVars[index].name.exchange(nullptr);
    }
    deleteList(_stack.exchange(nullptr));
}

void EventQueue::deleteList(Node* node) noexcept
{
    while (node)
        delete std::exchange(node, node->next);
}

template <typename E>
PushResult EventQueue::push(E&& event) noexcept
{
    auto const index = event.index();
    auto const kind = kindOf(event);
    auto* const node = makeNode(std::forward<E>(event));
    if (!node)
        return PushResult::Dropped;

    if (kind == EventKind::UserVar)
        return pushUserVar(node);
    if (!coalesces(kind))
        return pushOnStack(node);

    // Whoever exchanges a node out of a slot owns it: no one else can still be reading it.
    auto* const replaced = _slots.at(index).exchange(node);
    if (!replaced)
        return PushResult::Queued;
    delete replaced;
    return PushResult::Coalesced;
}

EventQueue::UserVarSlot* EventQueue::slotOf(std::string const& name) noexcept
{
    auto const slotCount = _limits.userVarNames;
    auto const start = userVarKey(name);
    auto claim = std::unique_ptr<std::string const> {};
    for (auto const probe: std::views::iota(std::size_t { 0 }, slotCount))
    {
        auto& slot = _userVars[(start + probe) % slotCount];
        auto const* claimed = slot.name.load();
        if (!claimed)
        {
            try
            {
                if (!claim)
                    claim = std::make_unique<std::string const>(name);
            }
            catch (...)
            {
                return nullptr;
            }
            if (slot.name.compare_exchange_strong(claimed, claim.get()))
            {
                // Owned by the slot now, which frees it with the queue.
                [[maybe_unused]] auto const* const owned = claim.release();
                return &slot;
            }
            // Another producer claimed it first; `claimed` is now its name.
        }
        if (*claimed == name)
            return &slot;
    }
    return nullptr;
}

PushResult EventQueue::pushUserVar(Node* node) noexcept
{
    // Everything about the node is read before it is published: afterwards a drain may take it.
    auto* const slot = slotOf(userVarName(node->event));
    if (!slot)
        return pushOnStack(node);
    // A slot only holds values of its own name, so what this replaces is a superseded value of it.
    auto* const replaced = slot->pending.exchange(node);
    if (!replaced)
        return PushResult::Queued;
    delete replaced;
    return PushResult::Coalesced;
}

PushResult EventQueue::pushOnStack(Node* node) noexcept
{
    if (_pending.fetch_add(1) >= _limits.capacity)
    {
        _pending.fetch_sub(1);
        _dropped.fetch_add(1, std::memory_order_relaxed);
        delete node;
        return PushResult::Dropped;
    }
    // The consumer only ever takes the whole stack, so a node is never popped from under a
    // producer's compare-exchange: there is no ABA.
    node->next = _stack.load();
    while (!_stack.compare_exchange_weak(node->next, node))
        ;
    return PushResult::Queued;
}

template <typename E>
EventQueue::Node* EventQueue::makeNode(E&& event) noexcept
{
    try
    {
        return new Node { .event = std::forward<E>(event),
                          .sequence = _sequence.fetch_add(1, std::memory_order_relaxed),
                          .next = nullptr };
    }
    catch (...)
    {
        _dropped.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
}

template PushResult EventQueue::push<Event const&>(Event const&) noexcept;
template PushResult EventQueue::push<Event>(Event&&) noexcept;

std::vector<Event> EventQueue::drain()
{
    auto nodes = std::vector<std::unique_ptr<Node>> {};
    nodes.reserve(_slots.size() + _limits.userVarNames + _limits.capacity);
    for (auto& slot: _slots)
        if (auto* const node = slot.exchange(nullptr))
            nodes.emplace_back(node);
    for (auto const index: std::views::iota(std::size_t { 0 }, _limits.userVarNames))
    {
        if (auto* const node = _userVars[index].pending.exchange(nullptr))
            nodes.emplace_back(node);
    }
    auto taken = std::size_t { 0 };
    for (auto* node = _stack.exchange(nullptr); node; ++taken)
        nodes.emplace_back(std::exchange(node, node->next));
    _pending.fetch_sub(taken);

    std::ranges::sort(nodes, {}, [](auto const& node) { return node->sequence; });

    // Of the pending values of one user variable, the latest wins: walk newest first, keep the
    // first seen per name, and turn the result round.
    auto seenNames = std::unordered_set<std::string> {};
    auto events = std::vector<Event> {};
    events.reserve(nodes.size());
    for (auto& node: nodes | std::views::reverse)
    {
        auto const* const userVar = std::get_if<UserVarChanged>(&node->event);
        if (userVar && !seenNames.insert(userVar->name).second)
            continue;
        events.push_back(std::move(node->event));
    }
    std::ranges::reverse(events);
    return events;
}

} // namespace vtextension
