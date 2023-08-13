/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2022 Christian Parpart <christian@parpart.family>
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

#include <crispy/defines.h>

namespace crispy
{

/**
 * Owned<T> behaves mostly like std::unique_ptr<T> except that it can be also
 * used within packed structs.
 */
template <typename T>
struct CRISPY_PACKED owned
{
  public:
    ~owned() { reset(); }
    owned() noexcept = default;
    owned(owned&& v) noexcept: _ptr { v.release() } {}
    owned& operator=(owned&& v) noexcept
    {
        _ptr = v.release();
        return *this;
    }

    owned(owned const& v) noexcept = delete;
    owned& operator=(owned const& v) = delete;

    [[nodiscard]] T* get() noexcept { return _ptr; }
    [[nodiscard]] T const* get() const noexcept { return _ptr; }

    constexpr T* operator->() noexcept { return _ptr; }
    constexpr T const* operator->() const noexcept { return _ptr; }

    constexpr T& operator*() noexcept { return *_ptr; }
    constexpr T const& operator*() const noexcept { return *_ptr; }

    constexpr operator bool() const noexcept { return _ptr != nullptr; }

    void reset(T* p = nullptr)
    {
        delete _ptr;
        _ptr = p;
    }

    T* release() noexcept
    {
        auto p = _ptr;
        _ptr = nullptr;
        return p;
    }

  private:
    T* _ptr = nullptr;
};

} // namespace crispy
