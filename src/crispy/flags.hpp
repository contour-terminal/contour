// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <format>

namespace crispy
{

// Provides a type-safe way to handle flags.
//
// Usage:
// ======
//     enum class FlagType
//     {
//         Flag1 = 1 << 0,
//         Flag2 = 1 << 1,
//         Flag3 = 1 << 2,
//     };
//     using MyFlags = crispy::Flags<FlagType>;
//     MyFlags f;
//     f.enable(FlagType::Flag1);
//     if (f & FlagType::Flag1) { ... }
//
template <typename flag_type>
class Flags
{
  public:
    using value_type = std::underlying_type_t<flag_type>;

    constexpr Flags(flag_type flag) noexcept: _value(static_cast<value_type>(flag)) {}
    constexpr Flags(std::initializer_list<flag_type> bits) noexcept
    {
        for (auto const bit: bits)
            enable(bit);
    }
    constexpr Flags() noexcept = default;
    constexpr Flags(Flags&&) noexcept = default;
    constexpr Flags(Flags const&) noexcept = default;
    constexpr Flags& operator=(Flags&&) noexcept = default;
    constexpr Flags& operator=(Flags const&) noexcept = default;

    constexpr void enable(flag_type flag) noexcept { _value |= static_cast<value_type>(flag); }
    constexpr void disable(flag_type flag) noexcept { _value &= ~static_cast<value_type>(flag); }

    constexpr void enable(Flags<flag_type> flags) noexcept { _value |= flags._value; }

    constexpr void disable(Flags<flag_type> flags) noexcept { _value &= ~flags._value; }

    // Tests for existence of all given flags to be present.
    // @return true if all flags are set in this flags set, false otherwise.
    [[nodiscard]] constexpr bool contains(Flags<flag_type> flags) const noexcept
    {
        return (_value & flags.value()) == flags.value();
    }

    [[nodiscard]] constexpr bool test(flag_type flag) const noexcept { return contains(flag); }

    [[nodiscard]] constexpr Flags<flag_type> operator&(Flags<flag_type> other) const noexcept
    {
        return Flags<flag_type>::from_value(_value & other.value());
    }

    constexpr Flags& operator|=(Flags<flag_type> flags) noexcept
    {
        _value |= flags._value;
        return *this;
    }

    constexpr Flags& operator|=(flag_type flag) noexcept
    {
        enable(flag);
        return *this;
    }

    constexpr Flags& operator&=(flag_type flag) noexcept
    {
        disable(flag);
        return *this;
    }

    [[nodiscard]] constexpr bool none() const noexcept { return _value == 0; }
    [[nodiscard]] constexpr bool any() const noexcept { return _value != 0; }

    [[nodiscard]] constexpr bool any(Flags<flag_type> other) const noexcept
    {
        return (_value & other.value()) != 0;
    }

    [[nodiscard]] constexpr bool operator!() const noexcept { return _value == 0; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return _value != 0; }

    [[nodiscard]] constexpr value_type value() const noexcept { return _value; }

    // NOLINTNEXTLINE(readability-identifier-naming)
    [[nodiscard]] static constexpr Flags<flag_type> from_value(value_type value) noexcept
    {
        auto result = Flags<flag_type> {};
        result._value = value;
        return result;
    }

    [[nodiscard]] constexpr Flags<flag_type> with(flag_type other) const noexcept
    {
        return Flags<flag_type>::from_value(_value | static_cast<value_type>(other));
    }

    [[nodiscard]] constexpr Flags<flag_type> with(Flags<flag_type> other) const noexcept
    {
        return Flags<flag_type>::from_value(_value | other.value());
    }

    [[nodiscard]] constexpr Flags<flag_type> intersect(Flags<flag_type> other) const noexcept
    {
        return Flags<flag_type>::from_value(_value & other.value());
    }

    [[nodiscard]] constexpr Flags<flag_type> without(Flags<flag_type> other) const noexcept
    {
        return Flags<flag_type>::from_value(_value & ~other.value());
    }

    [[nodiscard]] constexpr auto operator<=>(Flags<flag_type> const& other) const noexcept = default;

    [[nodiscard]] constexpr Flags<flag_type> operator|(Flags<flag_type> other) const noexcept
    {
        return Flags<flag_type>::from_value(_value | other.value());
    }

    [[nodiscard]] constexpr Flags<flag_type> operator|(flag_type other) const noexcept
    {
        return Flags<flag_type>::from_value(_value | static_cast<value_type>(other));
    }

    [[nodiscard]] auto reduce(auto init, auto f) const
    {
        auto result = std::move(init);
        for (auto i = 0u; i < sizeof(flag_type) * 8; ++i)
            if (auto const flag = static_cast<flag_type>(1 << i); test(flag))
                result = f(std::move(result), flag);
        return result;
    }

  private:
    value_type _value = 0;
};

} // namespace crispy

template <typename Enum>
// NOLINTNEXTLINE(readability-identifier-naming): std::formatter is the standard's spelling.
struct std::formatter<crispy::Flags<Enum>>: public std::formatter<std::string>
{
    auto format(crispy::Flags<Enum> const& flags, auto& ctx) const
    {
        std::string result;
        for (auto i = 0u; i < sizeof(Enum) * 8; ++i)
        {
            auto const flag = static_cast<Enum>(1 << i);
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
