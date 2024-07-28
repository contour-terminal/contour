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
    using value_type = std::underlying_type_t<flag_type>;

    constexpr flags(flag_type flag) noexcept: _value(static_cast<value_type>(flag)) {}
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

    constexpr void enable(flag_type flag) noexcept { _value |= static_cast<value_type>(flag); }
    constexpr void disable(flag_type flag) noexcept { _value &= ~static_cast<value_type>(flag); }

    constexpr void enable(flags<flag_type> flags) noexcept
    {
        _value |= static_cast<value_type>(flags._value);
    }

    constexpr void disable(flags<flag_type> flags) noexcept
    {
        _value &= ~static_cast<value_type>(flags._value);
    }

    // Tests for existence of all given flags to be present.
    // @return true if all flags are set in this flags set, false otherwise.
    [[nodiscard]] constexpr bool contains(flags<flag_type> flags) const noexcept
    {
        return (_value & flags.value()) == flags.value();
    }

    [[nodiscard]] constexpr bool test(flag_type flag) const noexcept { return contains(flag); }

    [[nodiscard]] constexpr flags<flag_type> operator&(flags<flag_type> other) const noexcept
    {
        return flags<flag_type>::from_value(_value & other.value());
    }

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
    [[nodiscard]] constexpr bool operator!() const noexcept { return _value == 0; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return _value != 0; }

    [[nodiscard]] constexpr value_type value() const noexcept { return _value; }

    // NOLINTNEXTLINE(readability-identifier-naming)
    [[nodiscard]] static constexpr flags<flag_type> from_value(value_type value) noexcept
    {
        auto result = flags<flag_type> {};
        result._value = value;
        return result;
    }

    [[nodiscard]] constexpr flags<flag_type> with(flag_type other) const noexcept
    {
        return flags<flag_type>::from_value(_value | static_cast<value_type>(other));
    }

    [[nodiscard]] constexpr flags<flag_type> with(flags<flag_type> other) const noexcept
    {
        return flags<flag_type>::from_value(_value | other.value());
    }

    [[nodiscard]] constexpr flags<flag_type> without(flags<flag_type> other) const noexcept
    {
        return flags<flag_type>::from_value(_value & ~other.value());
    }

    [[nodiscard]] constexpr auto operator<=>(flags<flag_type> const& other) const noexcept = default;

    [[nodiscard]] constexpr flags<flag_type> operator|(flags<flag_type> other) const noexcept
    {
        return flags<flag_type>::from_value(_value | other.value());
    }

    [[nodiscard]] constexpr flags<flag_type> operator|(flag_type other) const noexcept
    {
        return flags<flag_type>::from_value(_value | static_cast<value_type>(other));
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
struct fmt::formatter<crispy::flags<Enum>>: public fmt::formatter<std::string>
{
    auto format(crispy::flags<Enum> const& flags, format_context& ctx) const -> format_context::iterator
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
