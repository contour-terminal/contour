#pragma once

#include <type_traits>
#include <cstddef>
#include <cstdint>
#include <string>

namespace crispy {

/**
 * This implements the FNV1a (Fowler–Noll–Vo) hash function.
 *
 * @see http://en.wikipedia.org/wiki/Fowler-Noll-Vo_hash_function
 */
template <typename T, typename U = size_t>
class FNV {
  public:
    constexpr FNV() noexcept : basis_{2166136261llu}, prime_{16777619llu} {}
    constexpr FNV(U prime, U basis) noexcept : basis_(basis), prime_(prime) {}

    constexpr U prime() const noexcept { return prime_; }
    constexpr U basis() const noexcept { return basis_; }

    /// Initiate incremental hash building for the first value.
    constexpr U operator()(T _value) const noexcept
    {
        return (*this)(basis(), _value);
    }

    /// Incrementally builds up an FNV hash by applying @p _value to already constructed @p _memory
    /// and returns the applied FNV result.
    constexpr U operator()(U _memory, T _value) const noexcept
    {
        _memory ^= static_cast<std::common_type_t<T, U>>(_value);
        _memory *= prime_;
        return _memory;
    }

    template <typename... V>
    constexpr U operator()(U _memory, T _value, V... _moreValues) const noexcept
    {
        return (*this)((*this)(_memory, _value), _moreValues...);
    }

    template <typename... V>
    constexpr U operator()(U _memory, std::basic_string_view<T> _str, V... _moreValues) const noexcept
    {
        for (auto const ch : _str)
            _memory = (*this)(_memory, ch);
        return (*this)(_memory, _moreValues...);
    }

    /// Builds the FNV hash between [_begin, _end)
    constexpr U operator()(U _memory, std::basic_string_view<T> _str) const noexcept
    {
        for (auto const ch : _str)
            _memory = (*this)(_memory, ch);
        return _memory;
    }

    /// Builds the FNV hash between [_begin, _end)
    constexpr U operator()(T const* _begin, T const* _end) const noexcept
    {
        auto memory = basis_;
        while (_begin != _end)
            memory = (*this)(memory, *_begin++);
        return memory;
    }

    /// Builds the FNV hash between [_begin, _begin + len)
    constexpr U operator()(T const* data, size_t len) const noexcept
    {
        return (*this)(data, data + len);
    }

    /// Builds the FNV hash between [_begin, _begin + len)
    constexpr U operator()(std::basic_string<T> const& _str) const noexcept
    {
        return (*this)(_str.data(), _str.data() + _str.size());
    }

  protected:
    U const basis_;
    U const prime_;
};

} // end namespace crispy
