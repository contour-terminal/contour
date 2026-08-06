// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <crispy/Assert.hpp>

#include <optional>

namespace crispy
{

template <typename T>
struct Deferred
{
    std::optional<T> storage;

    [[nodiscard]] constexpr bool isInitialized() const noexcept { return storage.has_value(); }

    template <typename... Args>
    void initialize(Args&&... args)
    {
        Require(!storage.has_value());
        storage.emplace(std::forward<Args>(args)...);
    }

    T& get() { return storage.value(); }
    T const& get() const { return storage.value(); }

    T& operator*() { return storage.value(); }
    T const& operator*() const { return storage.value(); }

    T* operator->() { return &storage.value(); }
    T const* operator->() const { return &storage.value(); }
};

} // namespace crispy
