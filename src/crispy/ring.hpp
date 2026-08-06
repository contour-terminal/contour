// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <gsl/span>
#include <gsl/span_ext>

#include <algorithm>
#include <cassert>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace crispy
{

template <typename T, typename Vector>
struct RingIterator;
template <typename T, typename Vector>
struct RingReverseIterator;

/**
 * Implements an efficient Ring buffer over type T
 * and the underlying storage Vector.
 */
template <typename T, typename Vector = std::vector<T>>
class BasicRing
{
  public:
    using value_type = T;
    using iterator = RingIterator<value_type, Vector>;
    using const_iterator = RingIterator<value_type const, Vector>;
    using reverse_iterator = RingReverseIterator<value_type, Vector>;
    using const_reverse_iterator = RingReverseIterator<value_type const, Vector>;
    using difference_type = long;
    using OffsetType = long;

    BasicRing() = default; // NOLINT(cppcoreguidelines-pro-type-member-init)
    BasicRing(BasicRing const&) = default;
    BasicRing& operator=(BasicRing const&) = default;
    BasicRing(BasicRing&&) noexcept = default;
    BasicRing& operator=(BasicRing&&) noexcept = default;
    virtual ~BasicRing() = default;

    explicit BasicRing(Vector storage): _storage(std::move(storage)) {}

    [[nodiscard]] value_type const& operator[](OffsetType i) const noexcept
    {
        return _storage[size_t(OffsetType(_zero + size()) + i) % size()];
    }
    [[nodiscard]] value_type& operator[](OffsetType i) noexcept
    {
        return _storage[size_t(OffsetType(_zero + size()) + i) % size()];
    }

    [[nodiscard]] value_type const& at(OffsetType i) const noexcept
    {
        return _storage[size_t(_zero + size() + i) % size()];
    }
    [[nodiscard]] value_type& at(OffsetType i) noexcept
    {
        return _storage[size_t(OffsetType(_zero + size()) + i) % size()];
    }

    [[nodiscard]] Vector& storage() noexcept { return _storage; }
    [[nodiscard]] Vector const& storage() const noexcept { return _storage; }
    [[nodiscard]] std::size_t zero_index() const noexcept { return _zero; }

    void rezero(iterator i);
    void rezero();

    [[nodiscard]] std::size_t size() const noexcept { return _storage.size(); }

    // positvie count rotates right, negative count rotates left
    void rotate(int count) noexcept { _zero = size_t(OffsetType(_zero + size()) - count) % size(); }

    void rotate_left(std::size_t count) noexcept { _zero = (_zero + size() + count) % size(); }
    void rotate_right(std::size_t count) noexcept { _zero = (_zero + size() - count) % size(); }
    void unrotate() { _zero = 0; }

    [[nodiscard]] value_type& front() noexcept { return at(0); }
    [[nodiscard]] value_type const& front() const noexcept { return at(0); }

    [[nodiscard]] value_type& back()
    {
        if (size() == 0)
            throw std::length_error("empty");

        return at(static_cast<OffsetType>(size()) - 1);
    }

    [[nodiscard]] value_type const& back() const
    {
        if (size() == 0)
            throw std::length_error("empty");

        return at(static_cast<OffsetType>(size()) - 1);
    }

    [[nodiscard]] iterator begin() noexcept { return iterator { this, 0 }; }
    [[nodiscard]] iterator end() noexcept { return iterator { this, static_cast<difference_type>(size()) }; }

    [[nodiscard]] const_iterator cbegin() const noexcept
    {
        return const_iterator { (BasicRing<value_type const, Vector>*) this, 0 };
    }
    [[nodiscard]] const_iterator cend() const noexcept
    {
        return const_iterator { (BasicRing<value_type const, Vector>*) this,
                                static_cast<difference_type>(size()) };
    }

    [[nodiscard]] const_iterator begin() const noexcept { return cbegin(); }
    [[nodiscard]] const_iterator end() const noexcept { return cend(); }

    [[nodiscard]] reverse_iterator rbegin() noexcept;
    [[nodiscard]] reverse_iterator rend() noexcept;

    [[nodiscard]] const_reverse_iterator rbegin() const noexcept;
    [[nodiscard]] const_reverse_iterator rend() const noexcept;

    [[nodiscard]] gsl::span<value_type> span(OffsetType start, size_t count) noexcept
    {
        auto a = std::next(begin(), start);
        auto b = std::next(a, count);
        return gsl::make_span(a, b);
    }

    [[nodiscard]] gsl::span<value_type const> span(OffsetType start, size_t count) const noexcept
    {
        auto a = std::next(begin(), start);
        auto b = std::next(a, count);
        return gsl::make_span(a, b);
    }

  protected:
    Vector _storage;
    std::size_t _zero = 0;
};

/**
 * Implements an efficient Ring buffer over type T
 * and the underlying dynamic storage type Vector<T, Allocator>.
 */
template <typename T,
          template <typename, typename> class Container = std::vector,
          typename Allocator = std::allocator<T>>
class Ring: public BasicRing<T, Container<T, Allocator>>
{
  public:
    using BasicRing<T, Container<T, Allocator>>::BasicRing;

    Ring(size_t capacity, T value): Ring(Container<T, Allocator>(capacity, value)) {}
    explicit Ring(size_t capacity): Ring(capacity, T {}) {}

    [[nodiscard]] size_t size() const noexcept { return this->_storage.size(); }

    void reserve(size_t capacity) { this->_storage.reserve(capacity); }
    void resize(size_t newSize)
    {
        this->rezero();
        this->_storage.resize(newSize);
    }

    /// Resize, copy-constructing newly added elements from @p proto instead of default-constructing.
    /// Mirrors @c std::vector::resize(n, value).
    void resize(size_t newSize, T const& proto)
    {
        this->rezero();
        this->_storage.resize(newSize, proto);
    }
    void clear()
    {
        this->_storage.clear();
        this->_zero = 0;
    }
    void push_back(T const& value) { this->_storage.push_back(value); }

    void push_back(T&& value) { this->emplace_back(std::move(value)); }

    template <typename... Args>
    void emplace_back(Args&&... args)
    {
        this->_storage.emplace_back(std::forward<Args>(args)...);
    }

    void pop_front() { this->_storage.erase(this->_storage.begin()); }
};

/// Fixed-size BasicRing<T> implementation
template <typename T, std::size_t N>
using FixedSizeRing = BasicRing<T, std::array<T, N>>;

// {{{ iterator
template <typename T, typename Vector>
struct RingIterator
{
    using iterator_category = std::random_access_iterator_tag;
    using value_type = T;
    using difference_type = long;
    using pointer = T*;
    using reference = T&;

    BasicRing<T, Vector>* ring {};
    difference_type current {};

    RingIterator(BasicRing<T, Vector>* aRing, difference_type aCurrent): ring { aRing }, current { aCurrent }
    {
    }

    RingIterator() = default;

    RingIterator(RingIterator const&) = default;
    RingIterator& operator=(RingIterator const&) = default;

    RingIterator(RingIterator&&) noexcept = default;
    RingIterator& operator=(RingIterator&&) noexcept = default;

    RingIterator& operator++() noexcept
    {
        ++current;
        return *this;
    }

    RingIterator operator++(int) noexcept
    {
        auto old = *this;
        ++(*this);
        return old;
    }

    RingIterator& operator--() noexcept
    {
        --current;
        return *this;
    }

    RingIterator operator--(int) noexcept
    {
        auto old = *this;
        --(*this);
        return old;
    }

    RingIterator& operator+=(int n) noexcept
    {
        current += n;
        return *this;
    }
    RingIterator& operator-=(int n) noexcept
    {
        current -= n;
        return *this;
    }

    RingIterator operator+(difference_type n) noexcept { return RingIterator { ring, current + n }; }
    RingIterator operator-(difference_type n) noexcept { return RingIterator { ring, current - n }; }

    RingIterator operator+(RingIterator const& rhs) const noexcept
    {
        return RingIterator { ring, current + rhs.current };
    }
    difference_type operator-(RingIterator const& rhs) const noexcept { return current - rhs.current; }

    friend RingIterator operator+(difference_type n, RingIterator a)
    {
        return RingIterator { a.ring, n + a.current };
    }
    friend RingIterator operator-(difference_type n, RingIterator a)
    {
        return RingIterator { a.ring, n - a.current };
    }

    bool operator==(RingIterator const& rhs) const noexcept { return current == rhs.current; }
    bool operator!=(RingIterator const& rhs) const noexcept { return current != rhs.current; }

    T& operator*() noexcept { return (*ring)[current]; }
    T const& operator*() const noexcept { return (*ring)[current]; }

    T* operator->() noexcept { return &(*ring)[current]; }
    T* operator->() const noexcept { return &(*ring)[current]; }
};
// }}}

// {{{ reverse iterator
template <typename T, typename Vector>
struct RingReverseIterator
{
    using iterator_category = std::random_access_iterator_tag;
    using value_type = T;
    using difference_type = long;
    using pointer = T*;
    using reference = T&;

    BasicRing<T, Vector>* ring;
    difference_type current;

    RingReverseIterator(BasicRing<T, Vector>* aRing, difference_type aCurrent):
        ring { aRing }, current { aCurrent }
    {
    }

    RingReverseIterator(RingReverseIterator const&) = default;
    RingReverseIterator& operator=(RingReverseIterator const&) = default;

    RingReverseIterator(RingReverseIterator&&) noexcept = default;
    RingReverseIterator& operator=(RingReverseIterator&&) noexcept = default;

    RingReverseIterator& operator++() noexcept
    {
        ++current;
        return *this;
    }
    RingReverseIterator& operator++(int) noexcept { return ++(*this); }

    RingReverseIterator& operator--() noexcept
    {
        --current;
        return *this;
    }
    RingReverseIterator& operator--(int) noexcept { return --(*this); }

    RingReverseIterator& operator+=(int n) noexcept
    {
        current += n;
        return *this;
    }
    RingReverseIterator& operator-=(int n) noexcept
    {
        current -= n;
        return *this;
    }

    RingReverseIterator operator+(difference_type n) noexcept
    {
        return RingReverseIterator { ring, current + n };
    }
    RingReverseIterator operator-(difference_type n) noexcept
    {
        return RingReverseIterator { ring, current - n };
    }

    RingReverseIterator operator+(RingReverseIterator const& rhs) const noexcept
    {
        return RingReverseIterator { ring, current + rhs.current };
    }
    difference_type operator-(RingReverseIterator const& rhs) const noexcept { return current - rhs.current; }

    friend RingReverseIterator operator+(difference_type n, RingReverseIterator a)
    {
        return RingReverseIterator { a.ring, n + a.current };
    }
    friend RingReverseIterator operator-(difference_type n, RingReverseIterator a)
    {
        return RingReverseIterator { a.ring, n - a.current };
    }

    bool operator==(RingReverseIterator const& rhs) const noexcept { return current == rhs.current; }
    bool operator!=(RingReverseIterator const& rhs) const noexcept { return current != rhs.current; }

    T& operator*() noexcept { return (*ring)[ring->size() - current - 1]; }
    T const& operator*() const noexcept { return (*ring)[ring->size() - current - 1]; }

    T* operator->() noexcept { return &(*ring)[static_cast<difference_type>(ring->size()) - current - 1]; }

    T* operator->() const noexcept
    {
        return &(*ring)[static_cast<difference_type>(ring->size()) - current - 1];
    }
};
// }}}

// {{{ BasicRing<T> impl
template <typename T, typename Vector>
BasicRing<T, Vector>::reverse_iterator BasicRing<T, Vector>::rbegin() noexcept
{
    return reverse_iterator { this, 0 };
}

template <typename T, typename Vector>
BasicRing<T, Vector>::reverse_iterator BasicRing<T, Vector>::rend() noexcept
{
    return reverse_iterator { this, size() };
}

template <typename T, typename Vector>
BasicRing<T, Vector>::const_reverse_iterator BasicRing<T, Vector>::rbegin() const noexcept
{
    return const_reverse_iterator { (BasicRing<T const, Vector>*) this, 0 };
}

template <typename T, typename Vector>
BasicRing<T, Vector>::const_reverse_iterator BasicRing<T, Vector>::rend() const noexcept
{
    return const_reverse_iterator { (BasicRing<T const, Vector>*) this,
                                    static_cast<difference_type>(size()) };
}

template <typename T, typename Vector>
void BasicRing<T, Vector>::rezero()
{
    std::rotate(begin(), std::next(begin(), static_cast<difference_type>(_zero)), end()); // shift-left
    _zero = 0;
}

template <typename T, typename Vector>
void BasicRing<T, Vector>::rezero(iterator i)
{
    std::rotate(begin(), std::next(begin(), i.current), end()); // shift-left
    _zero = 0;
}
// }}}

} // namespace crispy
