// SPDX-License-Identifier: Apache-2.0
#pragma once

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
    flags() noexcept = default;
    flags(flag_type flag) noexcept { enable(flag); }

    constexpr void enable(flag_type flag) noexcept { _value |= static_cast<unsigned>(flag); }
    constexpr void disable(flag_type flag) noexcept { _value &= ~static_cast<unsigned>(flag); }

    [[nodiscard]] constexpr bool test(flag_type flag) const noexcept
    {
        return _value & static_cast<unsigned>(flag);
    }

    [[nodiscard]] constexpr bool operator&(flag_type flag) const noexcept { return test(flag); }

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
