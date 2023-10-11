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

    [[nodiscard]] constexpr unsigned value() const noexcept { return _value; }

  private:
    unsigned _value = 0;
};

} // namespace crispy
