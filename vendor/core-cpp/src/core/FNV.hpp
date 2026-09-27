// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

namespace core
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

    /// Incrementally hashes a value whose bytes are its value.
    ///
    /// The value is treated as a sequence of bytes and hashed byte-wise.
    /// This overload explicitly excludes std::string and std::string_view,
    /// which are handled by dedicated overloads.
    ///
    /// Only a type with unique object representations qualifies: a type with padding has bytes
    /// that are not part of its value, and hashing them made two objects with equal members hash
    /// differently depending on what their padding happened to hold.
    template <typename V>
    constexpr U operator()(U memory, V const& value) const noexcept
        requires(std::has_unique_object_representations_v<V> && !std::same_as<V, std::string>
                 && !std::same_as<V, std::string_view>)
    {
        // The step is spelled out rather than delegated to (*this)(memory, byte): an unsigned char
        // byte binds `V const&` exactly, better than it converts to T, so the call would pick this
        // overload again and recurse forever for every T but unsigned char.
        //
        // std::bit_cast rather than a reinterpret_cast through the object representation, which
        // no constant evaluation may do -- so the constexpr above was unusable at compile time.
        auto const bytes = std::bit_cast<std::array<unsigned char, sizeof(V)>>(value);
        for (auto const byte: bytes)
        {
            memory ^= static_cast<U>(byte);
            memory *= _prime;
        }
        return memory;
    }

    /// Incrementally hashes a string view and additional values (char only).
    ///
    /// Each character in @p str is hashed sequentially before applying
    /// the remaining values in @p moreValues.
    template <typename... V>
    constexpr U operator()(U memory, std::string_view str, V const&... moreValues) const noexcept
        requires(std::same_as<T, char>)
    {
        for (auto const ch: str)
            memory = (*this)(memory, ch);
        return (*this)(memory, moreValues...);
    }

    /// Builds the FNV hash between [_begin, _end)
    /// Builds the FNV hash for a string view (char only).
    ///
    /// Hashes all characters in @p str starting from the given memory value.
    constexpr U operator()(U memory, std::string_view str) const noexcept
        requires(std::same_as<T, char>)
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
    /// Builds the FNV hash for a std::string (char only).
    ///
    /// Hashes the string's character data sequentially.
    constexpr U operator()(std::string const& str) const noexcept
        requires(std::same_as<T, char>)
    {
        return (*this)(str.data(), str.data() + str.size());
    }

  protected:
    U const _basis;
    U const _prime;
};

} // end namespace core
