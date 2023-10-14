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
    constexpr flags(flag_type flag) noexcept { enable(flag); }
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

    [[nodiscard]] constexpr bool test(flag_type flag) const noexcept
    {
        return _value & static_cast<unsigned>(flag);
    }

    [[nodiscard]] constexpr bool operator&(flag_type flag) const noexcept { return test(flag); }

    constexpr flags& operator|=(flags<flag_type> flags) noexcept
    {
        _value |= flags._value;
        return *this;
    }

    [[nodiscard]] constexpr flags& operator|=(flag_type flag) noexcept
    {
        enable(flag);
        return *this;
    }

    [[nodiscard]] constexpr flags& operator&=(flag_type flag) noexcept
    {
        disable(flag);
        return *this;
    }

    [[nodiscard]] constexpr bool none() const noexcept { return _value == 0; }
    [[nodiscard]] constexpr bool any() const noexcept { return _value != 0; }

    [[nodiscard]] constexpr unsigned value() const noexcept { return _value; }

    // NOLINTNEXTLINE(readability-identifier-naming)
    [[nodiscard]] static constexpr flags<flag_type> from_value(unsigned value) noexcept
    {
        auto result = flags<flag_type> {};
        result._value = value;
        return result;
    }

    [[nodiscard]] constexpr flags<flag_type> with(flag_type flag) const noexcept
    {
        return flags<flag_type>::from_value(_value) | flag;
    }

    [[nodiscard]] constexpr flags<flag_type> without(flag_type flag) const noexcept
    {
        auto result = flags<flag_type>::from_value(_value);
        result &= flag;
        return result;
    }

  private:
    unsigned _value = 0;
};

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
