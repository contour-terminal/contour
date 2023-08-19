// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <crispy/assert.h>

#include <optional>

namespace crispy
{

template <typename T>
struct deferred // NOLINT(readability-identifier-naming)
{
    std::optional<T> storage;

    [[nodiscard]] constexpr bool is_initialized() const noexcept { return storage.has_value(); }

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
