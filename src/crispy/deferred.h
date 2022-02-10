/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
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

#include <crispy/assert.h>

#include <optional>

namespace crispy
{

template <typename T>
struct deferred
{
    std::optional<T> storage;

    constexpr bool is_initialized() const noexcept { return storage.has_value(); }

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
