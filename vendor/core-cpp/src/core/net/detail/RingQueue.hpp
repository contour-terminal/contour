// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `RingQueue` — the event loop's ready queue: a double-ended FIFO over one buffer that keeps its
/// capacity, so a loop in its steady state allocates nothing to queue and resume.
///
/// **Why not `std::deque`, which the ready queue was until 0.4.0.** A deque stores its elements in
/// fixed-size nodes and frees a node once the front passes it, so a FIFO whose length never grows
/// -- one completion queued and resumed per request -- still allocates a node and frees one every
/// few entries: 512 bytes hold nine 56-byte entries in libstdc++, eight 64-byte ones, and
/// fastcached measured the difference as allocations per request (`_M_push_back_aux`). A ring
/// allocates when it grows past its largest length so far and never again.
///
/// **What it is not.** It keeps a moved-from element in every slot it is not using, so its element
/// type must be default-constructible and its moved-from state must own nothing -- true of
/// @c EventLoop::ReadyEntry, whose members all empty themselves on a move. The operations are the
/// ones the loop uses and no more: both ends, a front read, a search and an erase from the middle
/// for @c EventLoop::cancelPending, and iteration for teardown.

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <iterator>
#include <ranges>
#include <type_traits>
#include <utility>
#include <vector>

namespace core::net::detail
{

/// A double-ended FIFO over one buffer whose capacity is a power of two and is kept.
/// @tparam T The element type: default-constructible, move-assignable, and owning nothing once
///         moved from.
template <typename T>
class RingQueue
{
  public:
    /// A position in the queue, from the front; invalidated by any change to the queue but the
    /// element's own.
    /// @tparam Value `T` or `T const`.
    template <typename Value>
    class Iterator
    {
      public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = std::remove_const_t<Value>;
        using difference_type = std::ptrdiff_t;
        using pointer = Value*;
        using reference = Value&;

        Iterator() noexcept = default;

        /// @param queue The queue.
        /// @param offset The position, from the front.
        Iterator(std::conditional_t<std::is_const_v<Value>, RingQueue const*, RingQueue*> queue,
                 std::size_t offset) noexcept:
            _queue(queue), _offset(offset)
        {
        }

        /// @return The element.
        [[nodiscard]] reference operator*() const noexcept { return (*_queue)[_offset]; }

        /// @return The element's address.
        [[nodiscard]] pointer operator->() const noexcept { return &(*_queue)[_offset]; }

        /// @return This iterator, advanced.
        Iterator& operator++() noexcept
        {
            ++_offset;
            return *this;
        }

        /// @return This iterator before it was advanced.
        Iterator operator++(int) noexcept
        {
            auto const before = *this;
            ++_offset;
            return before;
        }

        /// @return True if both name the same position.
        [[nodiscard]] friend bool operator==(Iterator const& lhs, Iterator const& rhs) noexcept
        {
            return lhs._offset == rhs._offset;
        }

        /// @return The position, from the front.
        [[nodiscard]] std::size_t offset() const noexcept { return _offset; }

      private:
        std::conditional_t<std::is_const_v<Value>, RingQueue const*, RingQueue*> _queue = nullptr;
        std::size_t _offset = 0;
    };

    using iterator = Iterator<T>;
    using const_iterator = Iterator<T const>;

    /// @return Whether the queue holds nothing.
    [[nodiscard]] bool empty() const noexcept { return _size == 0; }

    /// @return How many elements the queue holds.
    [[nodiscard]] std::size_t size() const noexcept { return _size; }

    /// @return How many elements it can hold before it next allocates.
    [[nodiscard]] std::size_t capacity() const noexcept { return _slots.size(); }

    /// @param offset A position, from the front; must be less than @c size().
    /// @return The element there.
    [[nodiscard]] T& operator[](std::size_t offset) noexcept
    {
        assert(offset < _size);
        return _slots[slotOf(offset)];
    }

    /// @param offset A position, from the front; must be less than @c size().
    /// @return The element there.
    [[nodiscard]] T const& operator[](std::size_t offset) const noexcept
    {
        assert(offset < _size);
        return _slots[slotOf(offset)];
    }

    /// @return The first element; the queue must not be empty.
    [[nodiscard]] T& front() noexcept { return (*this)[0]; }

    /// Appends @p value.
    /// @param value What to queue.
    void pushBack(T&& value)
    {
        reserveOneMore();
        _slots[slotOf(_size)] = std::move(value);
        ++_size;
    }

    /// Prepends @p value.
    /// @param value What to queue ahead of everything.
    void pushFront(T&& value)
    {
        reserveOneMore();
        _head = (_head + _slots.size() - 1) & mask();
        _slots[_head] = std::move(value);
        ++_size;
    }

    /// Takes the first element out; the queue must not be empty.
    /// @return It.
    [[nodiscard]] T takeFront() noexcept
    {
        assert(_size != 0);
        auto value = std::move(_slots[_head]);
        _head = (_head + 1) & mask();
        --_size;
        return value;
    }

    /// Removes the first element without moving it out -- for an element that owns nothing, such as
    /// one already read in place; the queue must not be empty. It stays in its slot until a later
    /// element is moved over it, which is the state every unused slot is in.
    void dropFront() noexcept
    {
        assert(_size != 0);
        _head = (_head + 1) & mask();
        --_size;
    }

    /// Removes the element at @p position, moving the ones behind it forward.
    /// @param position A position in this queue.
    void erase(iterator position) noexcept
    {
        auto const offset = position.offset();
        assert(offset < _size);
        for (auto const next: std::views::iota(offset + 1, _size))
            (*this)[next - 1] = std::move((*this)[next]);
        // The vacated last slot is moved-from, and so owns nothing.
        (*this)[_size - 1] = T {};
        --_size;
    }

    /// Destroys every element, keeping the capacity.
    void clear() noexcept
    {
        for (auto const offset: std::views::iota(std::size_t { 0 }, _size))
            (*this)[offset] = T {};
        _head = 0;
        _size = 0;
    }

    /// @return The front.
    [[nodiscard]] iterator begin() noexcept { return iterator { this, 0 }; }

    /// @return Past the back.
    [[nodiscard]] iterator end() noexcept { return iterator { this, _size }; }

    /// @return The front.
    [[nodiscard]] const_iterator begin() const noexcept { return const_iterator { this, 0 }; }

    /// @return Past the back.
    [[nodiscard]] const_iterator end() const noexcept { return const_iterator { this, _size }; }

    RingQueue() noexcept = default;
    RingQueue(RingQueue const&) = delete;
    RingQueue& operator=(RingQueue const&) = delete;

    /// Takes @p other's elements, leaving it empty and usable: its head and size go with its slots,
    /// or it would answer for elements it no longer has.
    /// @param other The queue to take.
    RingQueue(RingQueue&& other) noexcept:
        _slots(std::exchange(other._slots, {})),
        _head(std::exchange(other._head, 0)),
        _size(std::exchange(other._size, 0))
    {
    }

    /// As the move constructor; what this held is released.
    /// @param other The queue to take.
    /// @return This.
    RingQueue& operator=(RingQueue&& other) noexcept
    {
        if (this != &other)
        {
            _slots = std::exchange(other._slots, {});
            _head = std::exchange(other._head, 0);
            _size = std::exchange(other._size, 0);
        }
        return *this;
    }

    ~RingQueue() = default;

  private:
    /// The capacity a first element allocates, and so the length below which a loop never grows.
    static constexpr std::size_t InitialCapacity = 16;

    [[nodiscard]] std::size_t mask() const noexcept { return _slots.size() - 1; }

    [[nodiscard]] std::size_t slotOf(std::size_t offset) const noexcept { return (_head + offset) & mask(); }

    /// Doubles the buffer when it is full, moving the elements to its start in queue order.
    void reserveOneMore()
    {
        if (_size < _slots.size())
            return;
        auto grown = std::vector<T>(std::max(InitialCapacity, _slots.size() * 2));
        for (auto const offset: std::views::iota(std::size_t { 0 }, _size))
            grown[offset] = std::move((*this)[offset]);
        _slots = std::move(grown);
        _head = 0;
    }

    std::vector<T> _slots; ///< Every slot outside [head, head + size) holds a moved-from element.
    std::size_t _head = 0; ///< The slot of the front element.
    std::size_t _size = 0; ///< How many elements are queued.
};

} // namespace core::net::detail
