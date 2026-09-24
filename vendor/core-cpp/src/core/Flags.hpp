// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <compare>
#include <cstddef>
#include <format>
#include <initializer_list>
#include <ranges>
#include <string>
#include <type_traits>
#include <utility>

namespace core
{

/// Provides a type-safe way to handle flags.
///
/// Usage:
///
///     enum class FlagType
///     {
///         Flag1 = 1 << 0,
///         Flag2 = 1 << 1,
///         Flag3 = 1 << 2,
///     };
///     using MyFlags = core::Flags<FlagType>;
///     MyFlags f;
///     f.enable(FlagType::Flag1);
///     if (f & FlagType::Flag1) { ... }
///
template <typename FlagType>
class Flags
{
  public:
    using value_type = std::underlying_type_t<FlagType>;

    constexpr Flags(FlagType flag) noexcept: _value(static_cast<value_type>(flag)) {}
    constexpr Flags(std::initializer_list<FlagType> bits) noexcept
    {
        for (auto const bit: bits)
            enable(bit);
    }
    constexpr Flags() noexcept = default;
    constexpr Flags(Flags&&) noexcept = default;
    constexpr Flags(Flags const&) noexcept = default;
    constexpr Flags& operator=(Flags&&) noexcept = default;
    constexpr Flags& operator=(Flags const&) noexcept = default;

    constexpr void enable(FlagType flag) noexcept { _value |= static_cast<value_type>(flag); }
    constexpr void disable(FlagType flag) noexcept { _value &= ~static_cast<value_type>(flag); }

    constexpr void enable(Flags<FlagType> flags) noexcept { _value |= flags._value; }

    constexpr void disable(Flags<FlagType> flags) noexcept { _value &= ~flags._value; }

    // Tests for existence of all given flags to be present.
    // @return true if all flags are set in this flags set, false otherwise.
    [[nodiscard]] constexpr bool contains(Flags<FlagType> flags) const noexcept
    {
        return (_value & flags.value()) == flags.value();
    }

    [[nodiscard]] constexpr bool test(FlagType flag) const noexcept { return contains(flag); }

    [[nodiscard]] constexpr Flags<FlagType> operator&(Flags<FlagType> other) const noexcept
    {
        return Flags<FlagType>::fromValue(_value & other.value());
    }

    constexpr Flags& operator|=(Flags<FlagType> flags) noexcept
    {
        _value |= flags._value;
        return *this;
    }

    constexpr Flags& operator|=(FlagType flag) noexcept
    {
        enable(flag);
        return *this;
    }

    /// Intersects with @p flags, exactly as operator& does.
    ///
    /// This was disable(), which cleared the named flag instead: the complement of what the
    /// binary operator answers.
    constexpr Flags& operator&=(Flags<FlagType> flags) noexcept
    {
        _value &= flags._value;
        return *this;
    }

    constexpr Flags& operator&=(FlagType flag) noexcept
    {
        _value &= static_cast<value_type>(flag);
        return *this;
    }

    [[nodiscard]] constexpr bool none() const noexcept { return _value == 0; }
    [[nodiscard]] constexpr bool any() const noexcept { return _value != 0; }

    [[nodiscard]] constexpr bool any(Flags<FlagType> other) const noexcept
    {
        return (_value & other.value()) != 0;
    }

    [[nodiscard]] constexpr bool operator!() const noexcept { return _value == 0; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return _value != 0; }

    [[nodiscard]] constexpr value_type value() const noexcept { return _value; }

    [[nodiscard]] static constexpr Flags<FlagType> fromValue(value_type value) noexcept
    {
        auto result = Flags<FlagType> {};
        result._value = value;
        return result;
    }

    [[nodiscard]] constexpr Flags<FlagType> with(FlagType other) const noexcept
    {
        return Flags<FlagType>::fromValue(_value | static_cast<value_type>(other));
    }

    [[nodiscard]] constexpr Flags<FlagType> with(Flags<FlagType> other) const noexcept
    {
        return Flags<FlagType>::fromValue(_value | other.value());
    }

    [[nodiscard]] constexpr Flags<FlagType> intersect(Flags<FlagType> other) const noexcept
    {
        return Flags<FlagType>::fromValue(_value & other.value());
    }

    [[nodiscard]] constexpr Flags<FlagType> without(Flags<FlagType> other) const noexcept
    {
        return Flags<FlagType>::fromValue(_value & ~other.value());
    }

    [[nodiscard]] constexpr auto operator<=>(Flags<FlagType> const& other) const noexcept = default;

    [[nodiscard]] constexpr Flags<FlagType> operator|(Flags<FlagType> other) const noexcept
    {
        return Flags<FlagType>::fromValue(_value | other.value());
    }

    [[nodiscard]] constexpr Flags<FlagType> operator|(FlagType other) const noexcept
    {
        return Flags<FlagType>::fromValue(_value | static_cast<value_type>(other));
    }

    [[nodiscard]] auto reduce(auto init, auto f) const
    {
        auto result = std::move(init);
        for (auto const i: std::views::iota(std::size_t { 0 }, sizeof(FlagType) * 8))
            if (auto const flag = static_cast<FlagType>(value_type { 1 } << i); test(flag))
                result = f(std::move(result), flag);
        return result;
    }

  private:
    value_type _value = 0;
};

} // namespace core

template <typename Enum>
struct std::formatter<core::Flags<Enum>>: public std::formatter<std::string>
{
    auto format(core::Flags<Enum> const& flags, auto& ctx) const
    {
        std::string result;
        for (auto const i: std::views::iota(std::size_t { 0 }, sizeof(Enum) * 8))
        {
            auto const flag = static_cast<Enum>(std::underlying_type_t<Enum> { 1 } << i);
            if (!flags.test(flag))
                continue;

            // We assume that only valid enum values resulting into non-empty strings.
            auto const element = std::format("{}", flag);
            if (element.empty())
                continue;

            if (!result.empty())
                result += '|';

            result += element;
        }
        return formatter<std::string>::format(result, ctx);
    }
};
