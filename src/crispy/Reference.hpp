// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <functional>

template <typename T>
class Reference
{
  public:
    constexpr explicit Reference(T& _ref) noexcept: ref_ { &_ref } {}
    constexpr Reference(std::reference_wrapper<T> _ref) noexcept: ref_ { &_ref.get() } {}

    Reference(Reference const&) = default;
    Reference(Reference&&) noexcept = default;
    Reference& operator=(Reference const&) = default;
    Reference& operator=(Reference&&) noexcept = default;

    constexpr Reference& operator=(T& _ref)
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
constexpr Reference<T> mut(T& _ref) noexcept
{
    return Reference<T>(_ref);
}
