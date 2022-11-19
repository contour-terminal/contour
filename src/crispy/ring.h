/**
 * This file is part of the Contour terminal project
 *   Copyright (c) 2019-2021 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <gsl/span>
#include <gsl/span_ext>

#include <algorithm>
#include <cassert>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <vector>

namespace crispy
{

template <typename T, typename Vector, typename Ring_type>
struct RingIterator;
template <typename T, typename Vector, typename Ring_type>
struct RingReverseIterator;

template <typename T, typename Vector = std::vector<T>>
class sparse_ring
{
  public:
    using Vector_ind = std::vector<unsigned long>;
    using value_type = T;
    using iterator = RingIterator<value_type, Vector, sparse_ring<value_type, Vector>>;
    using const_iterator = RingIterator<value_type const, Vector, sparse_ring<value_type const, Vector>>;
    using reverse_iterator = RingReverseIterator<value_type, Vector, sparse_ring<value_type, Vector>>;
    using const_reverse_iterator =
        RingReverseIterator<value_type const, Vector, sparse_ring<value_type const, Vector>>;
    using difference_type = long;
    using offset_type = long;

    sparse_ring() = default;
    sparse_ring(sparse_ring const&) = default;
    sparse_ring& operator=(sparse_ring const&) = default;
    sparse_ring(sparse_ring&&) noexcept = default;
    sparse_ring& operator=(sparse_ring&&) noexcept = default;

    sparse_ring(size_t capacity, T value): _storage(capacity, value), _indexes(capacity)
    {
        unsigned long ind { 0 };
        std::for_each(_indexes.begin(), _indexes.end(), [&ind](unsigned long& n) { n = ind++; });
    }
    explicit sparse_ring(size_t capacity): _storage(capacity), _indexes(capacity)
    {
        unsigned long ind { 0 };
        std::for_each(_indexes.begin(), _indexes.end(), [&ind](unsigned long& n) { n = ind++; });
    }

    ~sparse_ring() = default;

    explicit sparse_ring(Vector storage): _storage(std::move(storage)) {}

    value_type const& operator[](offset_type i) const noexcept
    {
        auto offset = size_t(offset_type(_zero + size()) + i) % size();
        return _storage[_indexes[offset]];
    }
    value_type& operator[](offset_type i) noexcept
    {
        auto offset = size_t(offset_type(_zero + size()) + i) % size();
        return _storage[_indexes[offset]];
    }

    value_type const& at(offset_type i) const noexcept
    {
        auto offset = size_t(offset_type(_zero + size()) + i) % size();
        return _storage[_indexes[offset]];
    }
    value_type& at(offset_type i) noexcept
    {
        auto offset = size_t(offset_type(_zero + size()) + i) % size();
        return _storage[_indexes[offset]];
    }

    void push_back(value_type const& _value)
    {
        _storage.push_back(_value);
        _indexes.push_back(_storage.size() - 1);
    }

    void push_back(value_type&& _value) { this->emplace_back(std::move(_value)); }

    template <typename... Args>
    void emplace_back(Args&&... args)
    {
        this->_storage.emplace_back(std::forward<Args>(args)...);
        this->_indexes.emplace_back(_storage.size() - 1);
    }

    template <typename... Args>
    void emplace_before(offset_type i, Args&&... args)
    {
        auto offset = size_t(offset_type(_zero + size()) + i) % size();
        this->_storage.emplace_back(std::forward<Args>(args)...);
        auto it = Vector_ind::const_iterator(_indexes.begin() + offset);
        this->_indexes.emplace(it, _storage.size() - 1);
    }

    void insert_before(value_type const& _value, offset_type i)
    {
        _storage.push_back(_value);
        auto offset_of_inserted = _storage.size() - 1;
        auto offset = size_t(offset_type(_zero + size()) + i) % size();
        if (i < 0)
            offset++;
        auto it = Vector_ind::const_iterator(_indexes.begin() + offset);
        _indexes.emplace(it, offset_of_inserted);
    }

    void erase(offset_type i)
    {
        auto offset = size_t(offset_type(_zero + size()) + i) % size();
        if (i < 0)
            offset++;
        _storage[_indexes[offset]] = value_type();
        auto it = Vector_ind::const_iterator(_indexes.begin() + offset);
        _indexes.erase(it);
    }

    void pop_front()
    {
        this->_storage[this->_indexes[0]] = value_type();
        this->_indexes.erase(this->_indexes.begin());
    }

    [[nodiscard]] std::size_t zero_index() const noexcept { return _zero; }

    [[nodiscard]] size_t size() const noexcept { return this->_indexes.size(); }

    // positvie count rotates right, negative count rotates left
    void rotate(int count) noexcept { _zero = size_t(offset_type(_zero + size()) - count) % size(); }

    void rotate_left(std::size_t count) noexcept { _zero = (_zero + size() + count) % size(); }
    void rotate_right(std::size_t count) noexcept { _zero = (_zero + size() - count) % size(); }
    void unrotate() { _zero = 0; }

    void rezero()
    {
        std::rotate(begin(), std::next(begin(), static_cast<difference_type>(_zero)), end()); // shift-left
        _zero = 0;
    }

    void rezero(iterator i)
    {
        std::rotate(begin(), std::next(begin(), i.current), end()); // shift-left
        _zero = 0;
    }
    void reserve(size_t capacity)
    {
        this->_storage.reserve(capacity);
        this->_indexes.reserve(capacity);
    }
    void resize(size_t newSize)
    {
        this->rezero();
        this->_storage.resize(newSize);
        this->_indexes.resize(newSize);
    }

    value_type& front() noexcept { return at(0); }
    value_type const& front() const noexcept { return at(0); }

    value_type& back()
    {
        if (size() == 0)
            throw std::length_error("empty");

        return at(static_cast<offset_type>(size()) - 1);
    }

    value_type const& back() const
    {
        if (size() == 0)
            throw std::length_error("empty");

        return at(static_cast<offset_type>(size()) - 1);
    }

    iterator begin() noexcept { return iterator { this, 0 }; }
    iterator end() noexcept { return iterator { this, static_cast<difference_type>(size()) }; }

    const_iterator cbegin() const noexcept
    {
        return const_iterator { (sparse_ring<value_type const, Vector>*) this, 0 };
    }
    const_iterator cend() const noexcept
    {
        return const_iterator { (sparse_ring<value_type const, Vector>*) this,
                                static_cast<difference_type>(size()) };
    }

    const_iterator begin() const noexcept { return cbegin(); }
    const_iterator end() const noexcept { return cend(); }

    reverse_iterator rbegin() noexcept;
    reverse_iterator rend() noexcept;

    const_reverse_iterator rbegin() const noexcept;
    const_reverse_iterator rend() const noexcept;

    gsl::span<value_type> span(offset_type start, size_t count) noexcept
    {
        auto a = std::next(begin(), start);
        auto b = std::next(a, count);
        return gsl::make_span(a, b);
    }

    gsl::span<value_type const> span(offset_type start, size_t count) const noexcept
    {
        auto a = std::next(begin(), start);
        auto b = std::next(a, count);
        return gsl::make_span(a, b);
    }

  private:
    Vector _storage;
    Vector_ind _indexes;
    std::size_t _zero = 0;
};

/**
 * Implements an efficient ring buffer over type T
 * and the underlying storage Vector.
 */
template <typename T, typename Vector = std::vector<T>>
class basic_ring
{
  public:
    using value_type = T;
    using iterator = RingIterator<value_type, Vector, basic_ring<value_type, Vector>>;
    using const_iterator = RingIterator<value_type const, Vector, basic_ring<value_type const, Vector>>;
    using reverse_iterator = RingReverseIterator<value_type, Vector, basic_ring<value_type, Vector>>;
    using const_reverse_iterator =
        RingReverseIterator<value_type const, Vector, basic_ring<value_type const, Vector>>;
    using difference_type = long;
    using offset_type = long;

    basic_ring() = default;
    basic_ring(basic_ring const&) = default;
    basic_ring& operator=(basic_ring const&) = default;
    basic_ring(basic_ring&&) noexcept = default;
    basic_ring& operator=(basic_ring&&) noexcept = default;
    virtual ~basic_ring() = default;

    explicit basic_ring(Vector storage): _storage(std::move(storage)) {}

    value_type const& operator[](offset_type i) const noexcept
    {
        return _storage[size_t(offset_type(_zero + size()) + i) % size()];
    }
    value_type& operator[](offset_type i) noexcept
    {
        return _storage[size_t(offset_type(_zero + size()) + i) % size()];
    }

    value_type const& at(offset_type i) const noexcept
    {
        return _storage[size_t(_zero + size() + i) % size()];
    }
    value_type& at(offset_type i) noexcept
    {
        return _storage[size_t(offset_type(_zero + size()) + i) % size()];
    }

    Vector& storage() noexcept { return _storage; }
    Vector const& storage() const noexcept { return _storage; }
    [[nodiscard]] std::size_t zero_index() const noexcept { return _zero; }

    void rezero(iterator i);
    void rezero();

    [[nodiscard]] std::size_t size() const noexcept { return _storage.size(); }

    // positvie count rotates right, negative count rotates left
    void rotate(int count) noexcept { _zero = size_t(offset_type(_zero + size()) - count) % size(); }

    void rotate_left(std::size_t count) noexcept { _zero = (_zero + size() + count) % size(); }
    void rotate_right(std::size_t count) noexcept { _zero = (_zero + size() - count) % size(); }
    void unrotate() { _zero = 0; }

    value_type& front() noexcept { return at(0); }
    value_type const& front() const noexcept { return at(0); }

    value_type& back()
    {
        if (size() == 0)
            throw std::length_error("empty");

        return at(static_cast<offset_type>(size()) - 1);
    }

    value_type const& back() const
    {
        if (size() == 0)
            throw std::length_error("empty");

        return at(static_cast<offset_type>(size()) - 1);
    }

    iterator begin() noexcept { return iterator { this, 0 }; }
    iterator end() noexcept { return iterator { this, static_cast<difference_type>(size()) }; }

    const_iterator cbegin() const noexcept
    {
        return const_iterator { (basic_ring<value_type const, Vector>*) this, 0 };
    }
    const_iterator cend() const noexcept
    {
        return const_iterator { (basic_ring<value_type const, Vector>*) this,
                                static_cast<difference_type>(size()) };
    }

    const_iterator begin() const noexcept { return cbegin(); }
    const_iterator end() const noexcept { return cend(); }

    reverse_iterator rbegin() noexcept;
    reverse_iterator rend() noexcept;

    const_reverse_iterator rbegin() const noexcept;
    const_reverse_iterator rend() const noexcept;

    gsl::span<value_type> span(offset_type start, size_t count) noexcept
    {
        auto a = std::next(begin(), start);
        auto b = std::next(a, count);
        return gsl::make_span(a, b);
    }

    gsl::span<value_type const> span(offset_type start, size_t count) const noexcept
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
class ring: public basic_ring<T, Container<T, Allocator>>
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
    void push_back(T const& _value) { this->_storage.push_back(_value); }

    void push_back(T&& _value) { this->emplace_back(std::move(_value)); }

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
template <typename T, typename Vector, typename Ring_type>
struct RingIterator
{
    using iterator_category = std::random_access_iterator_tag;
    using value_type = T;
    using difference_type = long;
    using pointer = T*;
    using reference = T&;

    Ring_type* ring {};
    difference_type current {};

    RingIterator(Ring_type* aRing, difference_type aCurrent): ring { aRing }, current { aCurrent } {}

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
template <typename T, typename Vector, typename Ring_type>
struct RingReverseIterator
{
    using iterator_category = std::random_access_iterator_tag;
    using value_type = T;
    using difference_type = long;
    using pointer = T*;
    using reference = T&;

    Ring_type* ring;
    difference_type current;

    RingReverseIterator(Ring_type* _ring, difference_type _current): ring { _ring }, current { _current } {}

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

template <typename T, typename Vector>
typename sparse_ring<T, Vector>::reverse_iterator sparse_ring<T, Vector>::rbegin() noexcept
{
    return reverse_iterator { this, 0 };
}

template <typename T, typename Vector>
typename sparse_ring<T, Vector>::reverse_iterator sparse_ring<T, Vector>::rend() noexcept
{
    return reverse_iterator { this, size() };
}

template <typename T, typename Vector>
typename sparse_ring<T, Vector>::const_reverse_iterator sparse_ring<T, Vector>::rbegin() const noexcept
{
    return const_reverse_iterator { (sparse_ring<T const, Vector>*) this, 0 };
}

template <typename T, typename Vector>
typename sparse_ring<T, Vector>::const_reverse_iterator sparse_ring<T, Vector>::rend() const noexcept
{
    return const_reverse_iterator { (sparse_ring<T const, Vector>*) this,
                                    static_cast<difference_type>(size()) };
}
// }}}

} // namespace crispy
