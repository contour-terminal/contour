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
struct ring_iterator;
template <typename T, typename Vector>
struct ring_reverse_iterator;

/**
 * Implements an efficient ring buffer over type T
 * and the underlying storage Vector.
 */
template <typename T, typename Vector = std::vector<T>>
class basic_ring // NOLINT(readability-identifier-naming)
{
  public:
    using value_type = T;
    using iterator = ring_iterator<value_type, Vector>;
    using const_iterator = ring_iterator<value_type const, Vector>;
    using reverse_iterator = ring_reverse_iterator<value_type, Vector>;
    using const_reverse_iterator = ring_reverse_iterator<value_type const, Vector>;
    using difference_type = long;
    using offset_type = long;

    basic_ring() = default; // NOLINT(cppcoreguidelines-pro-type-member-init)
    basic_ring(basic_ring const&) = default;
    basic_ring& operator=(basic_ring const&) = default;
    basic_ring(basic_ring&&) noexcept = default;
    basic_ring& operator=(basic_ring&&) noexcept = default;
    virtual ~basic_ring() = default;

    explicit basic_ring(Vector storage): _storage(std::move(storage)) {}

    [[nodiscard]] value_type const& operator[](offset_type i) const noexcept
    {
        return _storage[size_t(offset_type(_zero + size()) + i) % size()];
    }
    [[nodiscard]] value_type& operator[](offset_type i) noexcept
    {
        return _storage[size_t(offset_type(_zero + size()) + i) % size()];
    }

    [[nodiscard]] value_type const& at(offset_type i) const noexcept
    {
        return _storage[size_t(_zero + size() + i) % size()];
    }
    [[nodiscard]] value_type& at(offset_type i) noexcept
    {
        return _storage[size_t(offset_type(_zero + size()) + i) % size()];
    }

    [[nodiscard]] Vector& storage() noexcept { return _storage; }
    [[nodiscard]] Vector const& storage() const noexcept { return _storage; }
    [[nodiscard]] std::size_t zero_index() const noexcept { return _zero; }

    void rezero(iterator i);
    void rezero();

    [[nodiscard]] std::size_t size() const noexcept { return _storage.size(); }

    // positvie count rotates right, negative count rotates left
    void rotate(int count) noexcept { _zero = size_t(offset_type(_zero + size()) - count) % size(); }

    void rotate_left(std::size_t count) noexcept { _zero = (_zero + size() + count) % size(); }
    void rotate_right(std::size_t count) noexcept { _zero = (_zero + size() - count) % size(); }
    void unrotate() { _zero = 0; }

    [[nodiscard]] value_type& front() noexcept { return at(0); }
    [[nodiscard]] value_type const& front() const noexcept { return at(0); }

    [[nodiscard]] value_type& back()
    {
        if (size() == 0)
            throw std::length_error("empty");

        return at(static_cast<offset_type>(size()) - 1);
    }

    [[nodiscard]] value_type const& back() const
    {
        if (size() == 0)
            throw std::length_error("empty");

        return at(static_cast<offset_type>(size()) - 1);
    }

    [[nodiscard]] iterator begin() noexcept { return iterator { this, 0 }; }
    [[nodiscard]] iterator end() noexcept { return iterator { this, static_cast<difference_type>(size()) }; }

    [[nodiscard]] const_iterator cbegin() const noexcept
    {
        return const_iterator { (basic_ring<value_type const, Vector>*) this, 0 };
    }
    [[nodiscard]] const_iterator cend() const noexcept
    {
        return const_iterator { (basic_ring<value_type const, Vector>*) this,
                                static_cast<difference_type>(size()) };
    }

    [[nodiscard]] const_iterator begin() const noexcept { return cbegin(); }
    [[nodiscard]] const_iterator end() const noexcept { return cend(); }

    [[nodiscard]] reverse_iterator rbegin() noexcept;
    [[nodiscard]] reverse_iterator rend() noexcept;

    [[nodiscard]] const_reverse_iterator rbegin() const noexcept;
    [[nodiscard]] const_reverse_iterator rend() const noexcept;

    [[nodiscard]] gsl::span<value_type> span(offset_type start, size_t count) noexcept
    {
        auto a = std::next(begin(), start);
        auto b = std::next(a, count);
        return gsl::make_span(a, b);
    }

    [[nodiscard]] gsl::span<value_type const> span(offset_type start, size_t count) const noexcept
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
 * Implements an efficient ring buffer over type T
 * and the underlying dynamic storage type Vector<T, Allocator>.
 */
template <typename T,
          template <typename, typename> class Container = std::vector,
          typename Allocator = std::allocator<T>>
class ring: public basic_ring<T, Container<T, Allocator>> // NOLINT(readability-identifier-naming)
{
  public:
    using basic_ring<T, Container<T, Allocator>>::basic_ring;

    ring(size_t capacity, T value): ring(Container<T, Allocator>(capacity, value)) {}
    explicit ring(size_t capacity): ring(capacity, T {}) {}

    [[nodiscard]] size_t size() const noexcept { return this->_storage.size(); }

    void reserve(size_t capacity) { this->_storage.reserve(capacity); }
    void resize(size_t newSize)
    {
        this->rezero();
        this->_storage.resize(newSize);
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

/// Fixed-size basic_ring<T> implementation
template <typename T, std::size_t N>
using fixed_size_ring = basic_ring<T, std::array<T, N>>;

// {{{ iterator
template <typename T, typename Vector>
struct ring_iterator
{
    using iterator_category = std::random_access_iterator_tag;
    using value_type = T;
    using difference_type = long;
    using pointer = T*;
    using reference = T&;

    basic_ring<T, Vector>* ring {};
    difference_type current {};

    ring_iterator(basic_ring<T, Vector>* aRing, difference_type aCurrent):
        ring { aRing }, current { aCurrent }
    {
    }

    ring_iterator() = default;

    ring_iterator(ring_iterator const&) = default;
    ring_iterator& operator=(ring_iterator const&) = default;

    ring_iterator(ring_iterator&&) noexcept = default;
    ring_iterator& operator=(ring_iterator&&) noexcept = default;

    ring_iterator& operator++() noexcept
    {
        ++current;
        return *this;
    }

    ring_iterator operator++(int) noexcept
    {
        auto old = *this;
        ++(*this);
        return old;
    }

    ring_iterator& operator--() noexcept
    {
        --current;
        return *this;
    }

    ring_iterator operator--(int) noexcept
    {
        auto old = *this;
        --(*this);
        return old;
    }

    ring_iterator& operator+=(int n) noexcept
    {
        current += n;
        return *this;
    }
    ring_iterator& operator-=(int n) noexcept
    {
        current -= n;
        return *this;
    }

    ring_iterator operator+(difference_type n) noexcept { return ring_iterator { ring, current + n }; }
    ring_iterator operator-(difference_type n) noexcept { return ring_iterator { ring, current - n }; }

    ring_iterator operator+(ring_iterator const& rhs) const noexcept
    {
        return ring_iterator { ring, current + rhs.current };
    }
    difference_type operator-(ring_iterator const& rhs) const noexcept { return current - rhs.current; }

    friend ring_iterator operator+(difference_type n, ring_iterator a)
    {
        return ring_iterator { a.ring, n + a.current };
    }
    friend ring_iterator operator-(difference_type n, ring_iterator a)
    {
        return ring_iterator { a.ring, n - a.current };
    }

    bool operator==(ring_iterator const& rhs) const noexcept { return current == rhs.current; }
    bool operator!=(ring_iterator const& rhs) const noexcept { return current != rhs.current; }

    T& operator*() noexcept { return (*ring)[current]; }
    T const& operator*() const noexcept { return (*ring)[current]; }

    T* operator->() noexcept { return &(*ring)[current]; }
    T* operator->() const noexcept { return &(*ring)[current]; }
};
// }}}

// {{{ reverse iterator
template <typename T, typename Vector>
struct ring_reverse_iterator
{
    using iterator_category = std::random_access_iterator_tag;
    using value_type = T;
    using difference_type = long;
    using pointer = T*;
    using reference = T&;

    basic_ring<T, Vector>* ring;
    difference_type current;

    ring_reverse_iterator(basic_ring<T, Vector>* ring, difference_type current):
        ring { ring }, current { current }
    {
    }

    ring_reverse_iterator(ring_reverse_iterator const&) = default;
    ring_reverse_iterator& operator=(ring_reverse_iterator const&) = default;

    ring_reverse_iterator(ring_reverse_iterator&&) noexcept = default;
    ring_reverse_iterator& operator=(ring_reverse_iterator&&) noexcept = default;

    ring_reverse_iterator& operator++() noexcept
    {
        ++current;
        return *this;
    }
    ring_reverse_iterator& operator++(int) noexcept { return ++(*this); }

    ring_reverse_iterator& operator--() noexcept
    {
        --current;
        return *this;
    }
    ring_reverse_iterator& operator--(int) noexcept { return --(*this); }

    ring_reverse_iterator& operator+=(int n) noexcept
    {
        current += n;
        return *this;
    }
    ring_reverse_iterator& operator-=(int n) noexcept
    {
        current -= n;
        return *this;
    }

    ring_reverse_iterator operator+(difference_type n) noexcept
    {
        return ring_reverse_iterator { ring, current + n };
    }
    ring_reverse_iterator operator-(difference_type n) noexcept
    {
        return ring_reverse_iterator { ring, current - n };
    }

    ring_reverse_iterator operator+(ring_reverse_iterator const& rhs) const noexcept
    {
        return ring_reverse_iterator { ring, current + rhs.current };
    }
    difference_type operator-(ring_reverse_iterator const& rhs) const noexcept
    {
        return current - rhs.current;
    }

    friend ring_reverse_iterator operator+(difference_type n, ring_reverse_iterator a)
    {
        return ring_reverse_iterator { a.ring, n + a.current };
    }
    friend ring_reverse_iterator operator-(difference_type n, ring_reverse_iterator a)
    {
        return ring_reverse_iterator { a.ring, n - a.current };
    }

    bool operator==(ring_reverse_iterator const& rhs) const noexcept { return current == rhs.current; }
    bool operator!=(ring_reverse_iterator const& rhs) const noexcept { return current != rhs.current; }

    T& operator*() noexcept { return (*ring)[ring->size() - current - 1]; }
    T const& operator*() const noexcept { return (*ring)[ring->size() - current - 1]; }

    T* operator->() noexcept { return &(*ring)[static_cast<difference_type>(ring->size()) - current - 1]; }

    T* operator->() const noexcept
    {
        return &(*ring)[static_cast<difference_type>(ring->size()) - current - 1];
    }
};
// }}}

// {{{ basic_ring<T> impl
template <typename T, typename Vector>
typename basic_ring<T, Vector>::reverse_iterator basic_ring<T, Vector>::rbegin() noexcept
{
    return reverse_iterator { this, 0 };
}

template <typename T, typename Vector>
typename basic_ring<T, Vector>::reverse_iterator basic_ring<T, Vector>::rend() noexcept
{
    return reverse_iterator { this, size() };
}

template <typename T, typename Vector>
typename basic_ring<T, Vector>::const_reverse_iterator basic_ring<T, Vector>::rbegin() const noexcept
{
    return const_reverse_iterator { (basic_ring<T const, Vector>*) this, 0 };
}

template <typename T, typename Vector>
typename basic_ring<T, Vector>::const_reverse_iterator basic_ring<T, Vector>::rend() const noexcept
{
    return const_reverse_iterator { (basic_ring<T const, Vector>*) this,
                                    static_cast<difference_type>(size()) };
}

template <typename T, typename Vector>
void basic_ring<T, Vector>::rezero()
{
    std::rotate(begin(), std::next(begin(), static_cast<difference_type>(_zero)), end()); // shift-left
    _zero = 0;
}

template <typename T, typename Vector>
void basic_ring<T, Vector>::rezero(iterator i)
{
    std::rotate(begin(), std::next(begin(), i.current), end()); // shift-left
    _zero = 0;
}
// }}}

} // namespace crispy
