// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <fmt/format.h>

namespace crispy
{

// Provides a type-safe way to handle flags.
//
// Usage:
// ======
//     enum class flag_type
//     {
//         flag1 = 1 << 0,
//         flag2 = 1 << 1,
//         flag3 = 1 << 2,
//     };
//     using flags = crispy::flags<flag_type>;
//     flags f;
//     f.enable(flag_type::flag1);
//     if (f & flag_type::flag1) { ... }
//
template <typename flag_type>
class flags
{
  public:
    constexpr flags(flag_type flag) noexcept: _value(static_cast<unsigned>(flag)) {}
    constexpr flags(std::initializer_list<flag_type> bits) noexcept
    {
        for (auto const bit: bits)
            enable(bit);
    }
    constexpr flags() noexcept = default;
    constexpr flags(flags&&) noexcept = default;
    constexpr flags(flags const&) noexcept = default;
    constexpr flags& operator=(flags&&) noexcept = default;
    constexpr flags& operator=(flags const&) noexcept = default;

    constexpr void enable(flag_type flag) noexcept { _value |= static_cast<unsigned>(flag); }
    constexpr void disable(flag_type flag) noexcept { _value &= ~static_cast<unsigned>(flag); }

    constexpr void enable(flags<flag_type> flags) noexcept { _value |= static_cast<unsigned>(flags._value); }

    constexpr void disable(flags<flag_type> flags) noexcept
    {
        _value &= ~static_cast<unsigned>(flags._value);
    }

    [[nodiscard]] constexpr bool contains(flags<flag_type> flags) const noexcept
    {
        return _value & flags.value();
    }

    [[nodiscard]] constexpr bool test(flag_type flag) const noexcept { return contains(flag); }
    [[nodiscard]] constexpr bool operator&(flag_type flag) const noexcept { return contains(flag); }

    constexpr flags& operator|=(flags<flag_type> flags) noexcept
    {
        _value |= flags._value;
        return *this;
    }

    constexpr flags& operator|=(flag_type flag) noexcept
    {
        enable(flag);
        return *this;
    }

    constexpr flags& operator&=(flag_type flag) noexcept
    {
        disable(flag);
        return *this;
    }

    [[nodiscard]] constexpr bool none() const noexcept { return _value == 0; }
    [[nodiscard]] constexpr bool any() const noexcept { return _value != 0; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return _value != 0; }

    [[nodiscard]] constexpr unsigned value() const noexcept { return _value; }

    // NOLINTNEXTLINE(readability-identifier-naming)
    [[nodiscard]] static constexpr flags<flag_type> from_value(unsigned value) noexcept
    {
        auto result = flags<flag_type> {};
        result._value = value;
        return result;
    }

    [[nodiscard]] constexpr flags<flag_type> with(flag_type other) const noexcept
    {
        return flags<flag_type>::from_value(_value | static_cast<unsigned>(other));
    }

    constexpr flags<flag_type> without(flags<flag_type> other) const noexcept
    {
        return flags<flag_type>::from_value(_value & ~other.value());
    }

    constexpr auto operator<=>(flags<flag_type> const& other) const noexcept = default;

  private:
    unsigned _value = 0;
};

template <typename flag_type>
constexpr flags<flag_type> operator|(flags<flag_type> lhs, flags<flag_type> rhs) noexcept
{
    return flags<flag_type>::from_value(lhs.value() | rhs.value());
}

} // namespace crispy

template <typename Enum>
struct fmt::formatter<crispy::flags<Enum>>: public fmt::formatter<std::string>
{
    auto format(crispy::flags<Enum> const& flags, format_context& ctx) -> format_context::iterator
    {
        std::string result;
        for (auto i = 0u; i < sizeof(Enum) * 8; ++i)
        {
            auto const flag = static_cast<Enum>(1 << i);
            if (!flags.test(flag))
                continue;

            // We assume that only valid enum values resulting into non-empty strings.
            auto const element = fmt::format("{}", flag);
            if (element.empty())
                continue;

            if (!result.empty())
                result += '|';

            result += element;
        }
        return formatter<std::string>::format(result, ctx);
    }
};
