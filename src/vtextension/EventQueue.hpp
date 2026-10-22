// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <vtextension/Event.hpp>

namespace vtextension
{

/// What became of an event handed to EventQueue::tryPush.
enum class PushResult : uint8_t
{
    Dropped,   ///< The queue was full (or out of memory): the event is lost, and counted.
    Queued,    ///< The event waits to be delivered.
    Coalesced, ///< The event replaced a pending one of the same kind, which is not delivered.
};

/// How much an EventQueue holds.
struct EventQueueLimits
{
    std::size_t capacity = 0;     ///< How many replies (and overflowing user variables) may be pending.
    std::size_t userVarNames = 0; ///< How many user variables (the first to arrive) coalesce by name.
};

/// The bounded queue between the threads that observe the terminal and the one extension runner
/// they tell about it.
///
/// Producers never wait: tryPush() is lock-free. State events coalesce while pending, so that the
/// extension sees the latest value rather than every intermediate one:
///
/// - Each coalescing kind holds at most one pending event, which a newer one of its kind replaces;
///   such an event is never dropped.
/// - A user variable coalesces by name, in one of `userVarNames` slots the first that many names to
///   arrive claim for good; its latest value is never dropped either. A later name queues like a reply
///   (and coalesces by name when drained).
/// - Replies do not coalesce. They count against `capacity`; once it is reached they are dropped and
///   counted.
///
/// Any number of threads may push at once; one thread at a time drains.
class EventQueue
{
  public:
    /// @param limits How much the queue holds.
    explicit EventQueue(EventQueueLimits limits);

    EventQueue(EventQueue const&) = delete;
    EventQueue(EventQueue&&) = delete;
    EventQueue& operator=(EventQueue const&) = delete;
    EventQueue& operator=(EventQueue&&) = delete;
    ~EventQueue();

    /// Hands an event over without ever blocking.
    /// @param event The event.
    /// @return Whether it was queued, replaced a pending one, or was dropped (also when it could not be
    ///         copied for lack of memory).
    [[nodiscard]] PushResult tryPush(Event const& event) noexcept { return push(event); }

    /// @copydoc tryPush(Event const&)
    [[nodiscard]] PushResult tryPush(Event&& event) noexcept { return push(std::move(event)); }

    /// Takes every pending event, oldest first by when it was (last) pushed. Of several pending
    /// values of one user variable, only the latest is returned.
    /// @return The events.
    [[nodiscard]] std::vector<Event> drain();

    /// @return How many events were dropped over the queue's whole life.
    [[nodiscard]] uint64_t dropCount() const noexcept { return _dropped.load(std::memory_order_relaxed); }

  private:
    struct Node
    {
        Event event;
        uint64_t sequence = 0; ///< When the event was pushed, by _sequence.
        Node* next = nullptr;  ///< The next older node of the stack.
    };

    /// One user variable's coalescing slot: the name that claimed it, and that name's pending value.
    ///
    /// A slot is claimed once, by compare-exchange, and keeps its name for the queue's whole life: the
    /// name is immutable from then on, so a producer compares against it without touching any node, and
    /// a slot only ever holds values of its own name. A pending value is published by an exchange; the
    /// value it replaces belongs to the producer that took it, and no producer reads a node once it has
    /// published it.
    struct UserVarSlot
    {
        std::atomic<std::string const*> name { nullptr }; ///< Owned; set once, freed with the queue.
        std::atomic<Node*> pending { nullptr };
    };

    /// @see tryPush
    template <typename E>
    [[nodiscard]] PushResult push(E&& event) noexcept;

    /// Pushes a user variable's value into its name's slot, or onto the stack if its name has none.
    [[nodiscard]] PushResult pushUserVar(Node* node) noexcept;

    /// @return The slot of @p name, claiming a free one for it if it has none; null if every slot is
    ///         another name's (or no copy of the name could be allocated).
    [[nodiscard]] UserVarSlot* slotOf(std::string const& name) noexcept;

    /// Pushes a node onto the stack, if the capacity allows; otherwise deletes and counts it.
    [[nodiscard]] PushResult pushOnStack(Node* node) noexcept;

    /// Allocates a node for @p event, stamped with the next sequence number.
    /// @return The node; null (and the event counted as dropped) if it cannot be allocated.
    template <typename E>
    [[nodiscard]] Node* makeNode(E&& event) noexcept;

    /// Deletes a stack of nodes.
    static void deleteList(Node* node) noexcept;

    EventQueueLimits _limits;
    std::array<std::atomic<Node*>, std::variant_size_v<Event>> _slots {}; ///< By alternative index.
    std::unique_ptr<UserVarSlot[]> _userVars;                             ///< _limits.userVarNames of them.
    std::atomic<Node*> _stack { nullptr };   ///< Replies and overflowing user variables, newest first.
    std::atomic<std::size_t> _pending { 0 }; ///< How many nodes _stack holds, or is about to.
    std::atomic<uint64_t> _sequence { 0 };
    std::atomic<uint64_t> _dropped { 0 };
};

} // namespace vtextension
