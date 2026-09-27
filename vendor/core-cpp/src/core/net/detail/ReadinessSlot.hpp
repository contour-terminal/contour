// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `ReadinessSlot` — the backend-owned object a completion packet is allowed to name —
/// and `ReadinessSlotRef`, the share of one a @c ReadinessHandler holds.
///
/// **Why it exists, in one sentence: a completion can arrive after the handler is
/// gone.** On a readiness backend the kernel is handed a descriptor and answers with a
/// descriptor; nothing of the caller's travels into the kernel and comes back. On a
/// completion port it does — `lpOverlapped` is a pointer the caller supplies and the
/// kernel returns — and an operation the caller has since cancelled still delivers its
/// packet, on a later turn, with that pointer in it. If the pointer aimed into the
/// @c ReadinessHandler, dequeuing the packet would read storage whose owner has been
/// destroyed, with a kernel on the other end of it and no diagnostic anywhere.
///
/// So the pointer aims here instead, at an object the BACKEND owns and refcounts, and
/// the handler holds one share of it. The rule is written out beside
/// @c ReadinessHandler::slot as well, because that is where the next reader meets it.
///
/// Portable on purpose although only the IOCP backend has a use for it today: a public
/// header declares a member on every platform (`.agent/rules/platform.md`), and the
/// next completion-based backend — an `io_uring` one is the obvious candidate — wants
/// exactly this shape.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace core::net::detail
{

/// The backend-owned object a completion packet names.
///
/// Abstract, because what a slot HOLDS is the backend's business — an `OVERLAPPED` per
/// direction, a threadpool wait, a socket event — and none of that can appear in a
/// public header (`<Windows.h>` in one defines `min` and `max` into every consumer's
/// translation unit). What is public is the ownership protocol and nothing else.
///
/// **Two counters, and they answer different questions.**
///
/// - The **reference count** answers *may this storage be freed*. A share is held by
///   the handler for the length of its registration, and one by each operation the
///   kernel has been handed; the last one out calls @c dispose(). It is atomic because
///   an operation's share can be released from a thread-pool callback when a post
///   fails, which is the only path that is not the loop's.
/// - The **generation** answers *does this packet belong to the arm that is current*.
///   A slot serves one registration for its whole life and is re-armed on every turn
///   that re-watches it, so a packet from a cancelled arm and a packet from the live
///   one are the same pointer and differ only in this number. Without it, a readiness
///   the loop muted and then re-watched is dispatched twice: once for the packet the
///   mute could not take back, and once for the arm that replaced it. It is written and
///   read on the loop's thread alone — a helper thread only ever copies it into a
///   packet — so it is a plain integer, like @c detail::WorkerIdentity's depth.
///
/// Neither is a substitute for the other: a refcount says the storage is there, and
/// says nothing about whether reading @c ReadinessHandler through it is still allowed.
/// That is what @c retired() is for, and it is why a dequeued packet asks all three.
class ReadinessSlot
{
  public:
    ReadinessSlot() noexcept = default;

    ReadinessSlot(ReadinessSlot const&) = delete;
    ReadinessSlot& operator=(ReadinessSlot const&) = delete;
    ReadinessSlot(ReadinessSlot&&) = delete;
    ReadinessSlot& operator=(ReadinessSlot&&) = delete;

    /// Takes one share of this slot. Safe from any thread.
    void retain() noexcept { _references.fetch_add(1, std::memory_order_relaxed); }

    /// Gives one share back, disposing of the slot when it was the last. Safe from any
    /// thread, and the reason @c dispose() must be, too.
    void release() noexcept
    {
        if (_references.fetch_sub(1, std::memory_order_acq_rel) == 1)
            dispose();
    }

    /// @return How many shares are outstanding. For an assertion at teardown, where a
    ///         non-zero count is an operation the kernel was never asked to give back.
    [[nodiscard]] std::size_t referenceCount() const noexcept
    {
        return _references.load(std::memory_order_acquire);
    }

    /// @return Which arm is current. A packet carrying anything else is from an arm
    ///         that has been taken back, and is dropped.
    [[nodiscard]] std::uint64_t generation() const noexcept { return _generation; }

    /// @return True once the handler this slot spoke for has been detached. A packet
    ///         that arrives afterwards is dropped **without reading the handler
    ///         pointer**: by then it names storage the caller may have freed, and a
    ///         backend that validated it after the fact would have to survive address
    ///         reuse, which a bare pointer cannot (`IoBackend::detach`'s comment).
    [[nodiscard]] bool retired() const noexcept { return _retired; }

  protected:
    /// Protected and NON-virtual: nothing deletes a slot through this type, because
    /// nothing may. @c release() is the only way out, @c dispose() is what it calls, and
    /// the derived class deletes itself through its own complete type. A public virtual
    /// destructor would be the other way to satisfy the same rule and would be the wrong
    /// one here, because it would make `delete slot` compile — which is exactly the
    /// bypass of the refcount this type exists to prevent.
    ~ReadinessSlot() = default;

    /// Frees or recycles this slot. Called exactly once, by whichever thread released
    /// the last share, so an implementation may not touch the backend's own state
    /// unless it is safe off the loop's thread.
    virtual void dispose() noexcept = 0;

    /// Moves to the next arm, invalidating every packet still in flight for the
    /// previous one.
    /// @return The new generation, to be copied into the operation being armed.
    std::uint64_t nextGeneration() noexcept { return ++_generation; }

    /// Marks the handler gone. One way only: a retired slot is never revived, because
    /// the address that made it valid may since belong to something else.
    void retire() noexcept { _retired = true; }

  private:
    std::atomic<std::size_t> _references { 0 };
    /// Loop-thread only; see the class comment for why it is not atomic.
    std::uint64_t _generation = 0;
    /// Loop-thread only, and written before the handler's share is dropped.
    bool _retired = false;
};

/// One share of a @c ReadinessSlot, released when it goes out of scope.
///
/// Copyable, because @c ReadinessHandler is: the handler is an aggregate that callers
/// build with designated initialisers and assign wholesale, and a move-only member
/// would make every one of those a compile error for no gain. A copy takes another
/// share, which is what a refcounted reference means.
class ReadinessSlotRef
{
  public:
    ReadinessSlotRef() noexcept = default;

    /// Takes a share of @p slot.
    /// @param slot The slot to reference.
    explicit ReadinessSlotRef(ReadinessSlot& slot) noexcept: _slot { &slot } { _slot->retain(); }

    ReadinessSlotRef(ReadinessSlotRef const& other) noexcept: _slot { other._slot }
    {
        if (_slot != nullptr)
            _slot->retain();
    }

    ReadinessSlotRef(ReadinessSlotRef&& other) noexcept: _slot { std::exchange(other._slot, nullptr) } {}

    ReadinessSlotRef& operator=(ReadinessSlotRef const& other) noexcept
    {
        // The self-assignment guard is belt to the braces below: retaining before the
        // old share is given back already makes `a = a` safe, because the count never
        // reaches zero in between. It is spelled out anyway, because "this is safe if
        // you read the next three lines in order" is not a property a reader should
        // have to reconstruct, and clang-tidy cannot.
        if (this == &other)
            return *this;
        if (other._slot != nullptr)
            other._slot->retain();
        if (_slot != nullptr)
            _slot->release();
        _slot = other._slot;
        return *this;
    }

    ReadinessSlotRef& operator=(ReadinessSlotRef&& other) noexcept
    {
        if (this != &other)
        {
            if (_slot != nullptr)
                _slot->release();
            _slot = std::exchange(other._slot, nullptr);
        }
        return *this;
    }

    ~ReadinessSlotRef() { reset(); }

    /// Gives the share back and becomes empty.
    void reset() noexcept
    {
        if (auto* const slot = std::exchange(_slot, nullptr); slot != nullptr)
            slot->release();
    }

    /// @return The slot, or nullptr when this reference is empty.
    [[nodiscard]] ReadinessSlot* get() const noexcept { return _slot; }

    /// @return True when this reference holds a slot.
    [[nodiscard]] explicit operator bool() const noexcept { return _slot != nullptr; }

  private:
    ReadinessSlot* _slot = nullptr;
};

} // namespace core::net::detail
