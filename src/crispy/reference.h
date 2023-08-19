// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <functional>

template <typename T>
class reference
{
  public:
    constexpr explicit reference(T& _ref) noexcept: ref_ { &_ref } {}
    constexpr reference(std::reference_wrapper<T> _ref) noexcept: ref_ { &_ref.get() } {}

    reference(reference const&) = default;
    reference(reference&&) noexcept = default;
    reference& operator=(reference const&) = default;
    reference& operator=(reference&&) noexcept = default;

    constexpr reference& operator=(T& _ref)
    {
        ref_ = _ref.ref_;
        return *this;
    }

    constexpr T& get() noexcept { return *ref_; }
    constexpr T const& get() const noexcept { return *ref_; }

  private:
    T* ref_;
};

template <typename T>
constexpr reference<T> mut(T& _ref) noexcept
{
    return reference<T>(_ref);
}
