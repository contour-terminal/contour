// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <system_error>
#include <variant>

namespace crispy
{

template <typename T>
struct failure
{
    T value;
};

template <typename T>
failure(T) -> failure<T>;

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

    constexpr result(failure<error_type> error) noexcept(std::is_nothrow_move_constructible_v<error_type>):
        _data(std::move(error))
    {
    }

    constexpr result(result const&) = delete;
    constexpr result(result&&) noexcept = default;

    constexpr result& operator=(result const&) = delete;
    constexpr result& operator=(result&&) noexcept = default;

    ~result() noexcept = default;

    constexpr void emplace(value_type value) noexcept(std::is_nothrow_move_constructible_v<value_type>)
    {
        _data = std::move(value);
    }

    // clang-format off

    [[nodiscard]] constexpr explicit operator bool() const noexcept { return std::holds_alternative<value_type>(_data); }
    [[nodiscard]] constexpr bool operator !() const noexcept { return std::holds_alternative<failure<error_type>>(_data); }

    [[nodiscard]] constexpr value_type& operator*() & noexcept { return std::get<value_type>(_data); }
    [[nodiscard]] constexpr value_type const& operator*() const& noexcept { return std::get<value_type>(_data); }
    [[nodiscard]] constexpr value_type operator*() && noexcept { return std::move(std::get<value_type>(_data)); }

    [[nodiscard]] constexpr bool has_value() const noexcept { return std::holds_alternative<value_type>(_data); }
    [[nodiscard]] constexpr bool is_error() const noexcept { return std::holds_alternative<failure<error_type>>(_data); }

    [[nodiscard]] constexpr value_type& value() & noexcept { return std::get<value_type>(_data); }
    [[nodiscard]] constexpr value_type const& value() const& noexcept { return std::get<value_type>(_data); }
    [[nodiscard]] constexpr value_type&& value() && noexcept { return std::move(std::get<value_type>(_data)); }
    [[nodiscard]] constexpr value_type const&& value() const&& noexcept { return std::move(std::get<value_type>(_data)); }

    [[nodiscard]] constexpr error_type& error() & noexcept { return std::get<failure<error_type>>(_data).value; }
    [[nodiscard]] constexpr error_type const& error() const& noexcept { return std::get<failure<error_type>>(_data).value; }
    [[nodiscard]] constexpr error_type&& error() && noexcept { return std::move(std::get<failure<error_type>>(_data).value); }
    [[nodiscard]] constexpr error_type const&& error() const&& noexcept { return std::move(std::get<failure<error_type>>(_data).value); }

    // clang-format on

    [[nodiscard]] constexpr value_type value_or(value_type alternative) noexcept(
        std::is_nothrow_move_constructible_v<value_type>)
    {
        return std::holds_alternative<value_type>(_data) ? std::get<value_type>(_data)
                                                         : std::move(alternative);
    }

    constexpr auto transform(auto&& continuation) const
        noexcept(noexcept(continuation(std::declval<value_type>())))
    // NB: Declaring the return type does break MSVC
    // -> result<decltype(continuation(std::declval<value_type>())), error_type>
    {
        using new_value_type = decltype(continuation(std::declval<value_type>()));
        using new_result_type = result<new_value_type, error_type>;

        if (has_value())
            return new_result_type { continuation(value()) };
        else
            return new_result_type { failure { error() } };
    }

    constexpr auto transform_error(auto&& continuation) const
        noexcept(noexcept(continuation(std::declval<error_type>())))
    // NB: Declaring the return type does break MSVC
    // -> result<value_type, decltype(continuation(std::declval<error_type>()))>
    {
        using new_error_type = decltype(continuation(std::declval<error_type>()));
        using new_result_type = result<value_type, new_error_type>;

        if (has_value())
            return new_result_type { value() };
        else
            return new_result_type { failure { continuation(error()) } };
    }

  private:
    std::variant<value_type, failure<error_type>> _data;
};

} // namespace crispy
