// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <functional>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>

namespace crispy
{

template <typename E>
class failure
{
  public:
    using error_type = E;

    constexpr failure(const failure&) = default;
    constexpr failure(failure&&) noexcept = default;

    template <typename Err = E, typename = std::enable_if_t<std::is_constructible_v<E, Err&&>>>
    constexpr explicit failure(Err&& e): _value { std::forward<Err>(e) }
    {
    }

    template <typename... Args>
    constexpr explicit failure(std::in_place_t, Args&&... args): _value { std::forward<Args>(args)... }
    {
    }

    template <typename U, typename... Args>
    constexpr explicit failure(std::in_place_t, std::initializer_list<U> il, Args&&... args):
        _value { il, std::forward<Args>(args)... }
    {
    }

    constexpr failure& operator=(const failure&) = default;
    constexpr failure& operator=(failure&&) noexcept = default;

    constexpr const E& error() const& noexcept { return _value; }
    constexpr E& error() & noexcept { return _value; }
    constexpr const E&& error() const&& noexcept { return std::move(_value); }
    constexpr E&& error() && noexcept { return std::move(_value); }

    constexpr void swap(failure& other) noexcept { std::swap(_value, other._value); }

    template <typename E2>
    friend constexpr bool operator==(failure<E> const& lhs, failure<E2> const& rhs) noexcept
    {
        return lhs.error() == rhs.error();
    }

    template <typename E2>
    friend constexpr bool operator!=(failure<E> const& lhs, failure<E2> const& rhs) noexcept
    {
        return lhs.error() != rhs.error();
    }

  private:
    E _value;
};

template <typename T>
failure(T) -> failure<T>;

// TODO: Finish support for T = void (this is not so important for now, however)

// result<T, E> is a type that represents either a value of type T or an error of type E.
//
// The API is inspired by C++23's upcoming std::expected<T, E> type.
// It is not intended to be fully implemented, but only as much as needed for the
// current project.
//
// The API is kept as close as possible to C++23's std::expected<T, E> type,
// so that it can be easily replaced once C++23 is available.
template <typename T, typename E = std::error_code>
class result
{
  private:
    struct failure_t
    {
    };

    static inline constexpr failure_t Failure {};

    static_assert(!std::is_reference_v<T>, "T must not be a reference");
    static_assert(!std::is_same_v<T, std::remove_cv_t<std::in_place_t>>, "T must not be in_place_t");
    static_assert(!std::is_same_v<T, std::remove_cv_t<failure_t>>, "T must not be failure_t");
    static_assert(!std::is_same_v<T, typename std::remove_cv_t<failure<E>>>, "T must not be failure<E>");

  public:
    using value_type = T;
    using error_type = E;

    constexpr result() noexcept(std::is_nothrow_default_constructible_v<value_type>): _data(value_type {}) {}
    constexpr result(result const&) = default;
    constexpr result(result&&) noexcept = default;

    template <typename U, typename G>
    constexpr explicit result(const result<U, G>& other):
        _data { std::holds_alternative<U>(other._data) ? std::get<U>(other._data)
                                                       : failure<G> { other.error() } }
    {
    }

    template <typename U, typename G>
    constexpr explicit result(result<U, G>&& other):
        _data { std::holds_alternative<U>(other._data) ? value_type { std::get<U>(std::move(other)._data) }
                                                       : failure<G> { std::move(other).error() } }
    {
    }

    template <typename U = T, typename = std::enable_if_t<!std::is_void_v<U>>>
    constexpr result(U&& value) noexcept(std::is_nothrow_move_constructible_v<value_type>):
        _data { T { std::forward<U>(value) } }
    {
    }

    template <typename U = T, typename = std::enable_if_t<!std::is_void_v<U>>>
    constexpr result(std::in_place_t, U&& value) noexcept(std::is_nothrow_move_constructible_v<T>):
        _data { std::forward<U>(value) }
    {
    }

    constexpr result(failure<error_type> error) noexcept(std::is_nothrow_move_constructible_v<error_type>):
        _data { std::move(error) }
    {
    }

    constexpr result(failure_t,
                     error_type&& error) noexcept(std::is_nothrow_move_constructible_v<error_type>):
        _data { error_type { std::move(error) } }
    {
    }

    constexpr result& operator=(result const&) = delete;
    constexpr result& operator=(result&&) noexcept = default;

    ~result() noexcept = default;

    // clang-format off

    [[nodiscard]] constexpr explicit operator bool() const noexcept { return std::holds_alternative<value_type>(_data); }
    [[nodiscard]] constexpr bool operator !() const noexcept { return std::holds_alternative<failure<error_type>>(_data); }

    [[nodiscard]] constexpr const T* operator->() const noexcept { return &std::get<value_type>(_data); }
    [[nodiscard]] constexpr T* operator->() noexcept { return &std::get<value_type>(_data); }
    template <std::enable_if_t<!std::is_same_v<T, void>, int> = 0>
    [[nodiscard]] constexpr const T& operator*() const& noexcept { return std::get<value_type>(_data); }
    template <std::enable_if_t<!std::is_same_v<T, void>, int> = 0>
    [[nodiscard]] constexpr T& operator*() & noexcept { return std::get<value_type>(_data); }
    template <std::enable_if_t<!std::is_same_v<T, void>, int> = 0>
    [[nodiscard]] constexpr const T&& operator*() const&& noexcept { return std::move(std::get<value_type>(_data)); }
    template <std::enable_if_t<!std::is_same_v<T, void>, int> = 0>
    [[nodiscard]] constexpr T&& operator*() && noexcept { return std::move(std::get<value_type>(_data)); }

    [[nodiscard]] constexpr bool has_value() const noexcept { return std::holds_alternative<value_type>(_data); }
    [[nodiscard]] constexpr bool is_error() const noexcept { return std::holds_alternative<failure<error_type>>(_data); }

    [[nodiscard]] constexpr value_type& value() & noexcept { return std::get<value_type>(_data); }
    [[nodiscard]] constexpr value_type const& value() const& noexcept { return std::get<value_type>(_data); }
    [[nodiscard]] constexpr value_type&& value() && noexcept { return std::move(std::get<value_type>(_data)); }
    [[nodiscard]] constexpr value_type const&& value() const&& noexcept { return std::move(std::get<value_type>(_data)); }

    [[nodiscard]] constexpr error_type& error() & noexcept { return std::get<failure<error_type>>(_data).error(); }
    [[nodiscard]] constexpr error_type const& error() const& noexcept { return std::get<failure<error_type>>(_data).error(); }
    [[nodiscard]] constexpr error_type&& error() && noexcept { return std::move(std::get<failure<error_type>>(_data).error()); }
    [[nodiscard]] constexpr error_type const&& error() const&& noexcept { return std::move(std::get<failure<error_type>>(_data).error()); }

    // clang-format on

    // {{{ emplace
    template <typename U = T, typename = std::enable_if_t<!std::is_void_v<U>>>
    constexpr U& emplace(value_type&& exp) noexcept(std::is_nothrow_move_constructible_v<value_type>)
    {
        _data = std::move(exp);
        return value();
    }

    template <typename U = T, typename = std::enable_if_t<std::is_void_v<U>>>
    constexpr void emplace() noexcept
    {
        _data = value_type {};
    }
    // }}}

    // {{{ value_or(...)

    [[nodiscard]] constexpr value_type value_or(value_type&& alternative) const& noexcept(
        std::is_nothrow_move_constructible_v<value_type>)
    {
        return has_value() ? value() : std::move(alternative);
    }

    [[nodiscard]] constexpr value_type value_or(value_type&& alternative) && noexcept(
        std::is_nothrow_move_constructible_v<value_type>)
    {
        return has_value() ? std::move(value()) : std::move(alternative);
    }

    // }}}

    // {{{ and_then(F)
    template <typename F>
    constexpr auto and_then(F&& f) & -> std::invoke_result_t<F, value_type>
    {
        if (has_value())
            return std::invoke(std::forward<F>(f), value());
        else
            return failure { error() };
    }

    template <typename F>
    constexpr auto and_then(F&& f) const& -> std::invoke_result_t<F, value_type>
    {
        if (has_value())
            return std::invoke(std::forward<F>(f), value());
        else
            return failure { error() };
    }

    template <typename F>
    constexpr auto and_then(F&& f) && -> std::invoke_result_t<F, value_type>
    {
        if (has_value())
            return std::invoke(std::forward<F>(f), std::move(value()));
        else
            return failure { std::move(error()) };
    }

    template <typename F>
    constexpr auto and_then(F&& f) const&& -> std::invoke_result_t<F, value_type>
    {
        if (has_value())
            return std::invoke(std::forward<F>(f), std::move(value()));
        else
            return failure { std::move(error()) };
    }
    // }}}

    // {{{ transform(F)

    template <typename F>
    constexpr auto transform(F&& f) & noexcept(noexcept(f(std::declval<value_type>())))
    {
        using new_value_type = decltype(f(std::declval<value_type>()));
        using new_result_type = result<new_value_type, error_type>;
        if (has_value())
        {
            if constexpr (std::is_void_v<new_value_type>)
                return new_result_type {};
            else
                return new_result_type { std::invoke(std::forward<F>(f))(value()) };
        }
        else
            return new_result_type { failure { error() } };
    }

    template <typename F>
    constexpr auto transform(F&& f) const& noexcept(noexcept(f(std::declval<value_type>())))
    {
        using new_value_type = decltype(f(std::declval<value_type>()));
        using new_result_type = result<new_value_type, error_type>;
        if (has_value())
        {
            if constexpr (std::is_void_v<new_value_type>)
                return new_result_type {};
            else
                return new_result_type { std::invoke(std::forward<F>(f))(value()) };
        }
        else
            return new_result_type { failure { error() } };
    }

    template <typename F>
    constexpr auto transform(F&& f) &&
    {
        using new_value_type = decltype(f(std::declval<value_type>()));
        using new_result_type = result<new_value_type, error_type>;
        if (has_value())
        {
            if constexpr (std::is_void_v<new_value_type>)
                return new_result_type {};
            else
                return new_result_type { std::invoke(std::forward<F>(f), std::move(value())) };
        }
        else
            return new_result_type { failure { std::move(error()) } };
    }

    template <typename F>
    constexpr auto transform(F&& f) const&&
    {
        using new_value_type = decltype(f(std::declval<value_type>()));
        using new_result_type = result<new_value_type, error_type>;
        if (has_value())
        {
            if constexpr (std::is_void_v<new_value_type>)
                return new_result_type {};
            else
                return new_result_type { std::invoke(std::forward<F>(f), std::move(value())) };
        }
        else
            return new_result_type { failure { std::move(error()) } };
    }

    // }}}

    // {{{ or_else(F)
    template <typename F>
    constexpr auto or_else(F&& f) & -> std::invoke_result_t<F, error_type>
    {
        if (has_value())
            return value();
        else
            return std::invoke(std::forward<F>(f), error());
    }

    template <typename F>
    constexpr auto or_else(F&& f) const& -> std::invoke_result_t<F, error_type>
    {
        if (has_value())
            return value();
        else
            return std::invoke(std::forward<F>(f), error());
    }

    template <typename F>
    constexpr auto or_else(F&& f) && -> std::invoke_result_t<F, error_type>
    {
        if (has_value())
            return std::move(*this);
        else
            return std::invoke(std::forward<F>(f), std::move(error()));
    }

    template <typename F>
    constexpr auto or_else(F&& f) const&& -> std::invoke_result<F, error_type>
    {
        if (has_value())
            return std::move(*this);
        else
            return std::invoke(std::forward<F>(f), std::move(error()));
    }

    // }}}

    // {{{ transform_error(F)

    template <typename F>
    constexpr auto transform_error(F&& f) & noexcept(noexcept(f(std::declval<error_type>())))
        -> result<value_type, typename std::invoke_result_t<F, error_type>::error_type>
    {
        using new_result_type = result<value_type, typename std::invoke_result_t<F, error_type>::error_type>;
        if (has_value())
        {
            if constexpr (std::is_void_v<value_type>)
                return new_result_type {};
            else
                return new_result_type { std::in_place, value() };
        }
        else
            return std::invoke(std::forward<F>(f), error());
    }

    template <typename F>
    constexpr auto transform_error(F&& f) const& noexcept(noexcept(f(std::declval<error_type>())))
        -> result<value_type, typename std::invoke_result_t<F, error_type>::error_type>
    {
        using new_result_type = result<value_type, typename std::invoke_result_t<F, error_type>::error_type>;
        if (has_value())
        {
            if constexpr (std::is_void_v<value_type>)
                return new_result_type {};
            else
                return new_result_type { std::in_place, value() };
        }
        else
            return std::invoke(std::forward<F>(f), error());
    }

    template <typename F>
    constexpr auto transform_error(F&& f) && noexcept(noexcept(f(std::declval<error_type>())))
        -> result<value_type, typename std::invoke_result_t<F, error_type>::error_type>
    {
        using new_result_type = result<value_type, typename std::invoke_result_t<F, error_type>::error_type>;
        if (has_value())
        {
            if constexpr (std::is_void_v<value_type>)
                return new_result_type {};
            else
                return new_result_type { std::in_place, std::move(value()) };
        }
        else
            return std::invoke(std::forward<F>(f), std::move(error()));
    }

    template <typename F>
    constexpr auto transform_error(F&& f) const&& noexcept(noexcept(f(std::declval<error_type>())))
        -> result<value_type, typename std::invoke_result_t<F, error_type>::error_type>
    {
        using new_result_type = result<value_type, typename std::invoke_result_t<F, error_type>::error_type>;
        if (has_value())
        {
            if constexpr (std::is_void_v<value_type>)
                return new_result_type {};
            else
                return new_result_type { std::in_place, std::move(value()) };
        }
        else
            return std::invoke(std::forward<F>(f), std::move(error()));
    }

    // }}}

  private:
    std::variant<value_type, failure<error_type>> _data;
};

} // namespace crispy
