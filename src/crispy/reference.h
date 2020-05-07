/**
 * This file is part of the "contour" project.
 *   Copyright (c) 2020 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <functional>

template <typename T>
class reference {
public:
    constexpr explicit reference(T& _ref) noexcept : ref_{&_ref} {}
    constexpr reference(std::reference_wrapper<T> _ref) noexcept : ref_{&_ref.get()} {}

    reference(reference const&) = default;
    reference(reference&&) = default;
    reference& operator=(reference const&) = default;
    reference& operator=(reference&&) = default;

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
constexpr reference<T> mut(T& _ref) noexcept {
    return reference<T>(_ref);
}
