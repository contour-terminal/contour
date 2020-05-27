#pragma once
#include <type_traits>
#include <iterator>

namespace crispy {

template <typename T>
class span {
  public:
    using value_type = T;

    using pointer_type = value_type*;
    using const_pointer_type = value_type const*;

    using iterator = pointer_type;
    using const_iterator = pointer_type;

    using reference_type = value_type&;
    using const_reference_type = value_type const&;

    // constructors
    //
    constexpr span(iterator _begin, iterator _end) noexcept : begin_{_begin}, end_{_end} {}
    constexpr span() noexcept : begin_{}, end_{} {}
    constexpr span(span<T> const&) noexcept = default;
    constexpr span(span<T>&&) noexcept = default;
    constexpr span& operator=(span<T> const&) noexcept = default;
    constexpr span& operator=(span<T>&&) noexcept = default;

    // readonly properties
    //
    constexpr bool empty() const noexcept { return begin_ == end_; }
    constexpr size_t size() const noexcept { return static_cast<size_t>(std::distance(begin_, end_)); }

    // iterators
    //
    constexpr iterator begin() noexcept { return begin_; }
    constexpr iterator end() noexcept { return end_; }
    constexpr const_iterator begin() const noexcept { return begin_; }
    constexpr const_iterator end() const noexcept { return end_; }

    // random access
    //
    constexpr reference_type operator[](size_t i) noexcept { return begin_[i]; }
    constexpr const_reference_type operator[](size_t i) const noexcept { return begin_[i]; }

  private:
    pointer_type begin_;
    pointer_type end_;
};

template <typename T> constexpr bool operator==(span<T> const& a, span<T> const& b) noexcept
{
    if (a.size() != b.size())
        return false;

    for (size_t i = 0; i < a.size(); ++i)
        if (a[i] != b[i])
            return false;

    return true;
}

template <typename T> constexpr bool operator!=(span<T> const& a, span<T> const& b) noexcept { return !(a == b); }

template <typename T> constexpr auto begin(span<T>& _span) noexcept { return _span.begin(); }
template <typename T> constexpr auto end(span<T>& _span) noexcept { return _span.end(); }

}
