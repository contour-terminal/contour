#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>

namespace crispy
{

/**
 * This implements the FNV1a (Fowler–Noll–Vo) hash function.
 *
 * @see http://en.wikipedia.org/wiki/Fowler-Noll-Vo_hash_function
 */
template <typename T, typename U = size_t>
class FNV
{
  public:
    constexpr FNV() noexcept: _basis { 2166136261llu }, _prime { 16777619llu } {}
    constexpr FNV(U prime, U basis) noexcept: _basis(basis), _prime(prime) {}

    [[nodiscard]] constexpr U prime() const noexcept { return _prime; }
    [[nodiscard]] constexpr U basis() const noexcept { return _basis; }

    /// Initiate incremental hash building for the first value.
    constexpr U operator()(T value) const noexcept { return (*this)(basis(), value); }

    /// Incrementally builds up an FNV hash by applying @p _value to already constructed @p _memory
    /// and returns the applied FNV result.
    constexpr U operator()(U memory, T value) const noexcept
    {
        memory ^= static_cast<U>(value);
        memory *= _prime;
        return memory;
    }

    template <typename... V>
    constexpr U operator()(U memory, T value, V... moreValues) const noexcept
    {
        return (*this)((*this)(memory, value), moreValues...);
    }

    template <typename... V>
    constexpr U operator()(U memory, std::basic_string_view<T> str, V... moreValues) const noexcept
    {
        for (auto const ch: str)
            memory = (*this)(memory, ch);
        return (*this)(memory, moreValues...);
    }

    /// Builds the FNV hash between [_begin, _end)
    constexpr U operator()(U memory, std::basic_string_view<T> str) const noexcept
    {
        for (auto const ch: str)
            memory = (*this)(memory, ch);
        return memory;
    }

    /// Builds the FNV hash between [_begin, _end)
    constexpr U operator()(T const* begin, T const* end) const noexcept
    {
        auto memory = _basis;
        while (begin != end)
            memory = (*this)(memory, *begin++);
        return memory;
    }

    /// Builds the FNV hash between [_begin, _begin + len)
    constexpr U operator()(T const* data, size_t len) const noexcept { return (*this)(data, data + len); }

    /// Builds the FNV hash between [_begin, _begin + len)
    constexpr U operator()(std::basic_string<T> const& str) const noexcept
    {
        return (*this)(str.data(), str.data() + str.size());
    }

  protected:
    U const _basis;
    U const _prime;
};

} // end namespace crispy
