// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <system_error>
#include <variant>

namespace crispy
{

template <typename T, typename E = std::error_code>
class result
{
  public:
    using value_type = T;
    using error_type = E;

    constexpr result(value_type value) noexcept(std::is_nothrow_move_constructible_v<value_type>):
        _data(std::move(value))
    {
    }

    constexpr result(error_type error) noexcept(std::is_nothrow_move_constructible_v<error_type>):
        _data(std::move(error))
    {
    }

    constexpr result(result const&) = delete;
    constexpr result(result&&) noexcept = default;

    constexpr result& operator=(result const&) = delete;
    constexpr result& operator=(result&&) noexcept = default;

    ~result() noexcept = default;

    // clang-format off

    constexpr explicit operator bool() const noexcept { return std::holds_alternative<value_type>(_data); }
    constexpr bool operator !() const noexcept { return std::holds_alternative<error_type>(_data); }

    [[nodiscard]] constexpr bool good() const noexcept { return std::holds_alternative<value_type>(_data); }
    [[nodiscard]] constexpr bool is_error() const noexcept { return std::holds_alternative<error_type>(_data); }

    [[nodiscard]] constexpr value_type& value() & noexcept { return std::get<value_type>(_data); }
    [[nodiscard]] constexpr value_type const& value() const& noexcept { return std::get<value_type>(_data); }
    [[nodiscard]] constexpr value_type&& value() && noexcept { return std::move(std::get<value_type>(_data)); }
    [[nodiscard]] constexpr value_type const&& value() const&& noexcept { return std::move(std::get<value_type>(_data)); }

    [[nodiscard]] constexpr error_type& error() & noexcept { return std::get<error_type>(_data); }
    [[nodiscard]] constexpr error_type const& error() const& noexcept { return std::get<error_type>(_data); }
    [[nodiscard]] constexpr error_type&& error() && noexcept { return std::move(std::get<error_type>(_data)); }
    [[nodiscard]] constexpr error_type const&& error() const&& noexcept { return std::move(std::get<error_type>(_data)); }

    // clang-format on

  private:
    std::variant<value_type, error_type> _data;
};

} // namespace crispy
