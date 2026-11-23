// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Owning a Windows handle, without `<Windows.h>`: a header that holds one need not drag the Windows API
/// into every translation unit that includes it.

#ifdef _WIN32

    #include <core/platform/Types.hpp>

    #include <utility>

namespace vtpty
{

/// Owns a Win32 HANDLE and closes it on destruction.
class UniqueHandle
{
  public:
    UniqueHandle() noexcept = default;

    /// @param handle The handle to own; null or invalid for none.
    explicit UniqueHandle(core::platform::NativeHandle handle) noexcept: _handle { handle } {}

    UniqueHandle(UniqueHandle const&) = delete;
    UniqueHandle& operator=(UniqueHandle const&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept: _handle { std::exchange(other._handle, nullptr) } {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            _handle = std::exchange(other._handle, nullptr);
        }
        return *this;
    }
    ~UniqueHandle() { reset(); }

    /// @return The handle, still owned.
    [[nodiscard]] core::platform::NativeHandle get() const noexcept { return _handle; }

    /// @return Whether it owns a handle.
    [[nodiscard]] bool valid() const noexcept
    {
        return _handle != nullptr && _handle != core::platform::InvalidHandle;
    }

    /// Closes the handle, if it owns one.
    void reset() noexcept;

  private:
    core::platform::NativeHandle _handle = nullptr;
};

} // namespace vtpty

#endif
