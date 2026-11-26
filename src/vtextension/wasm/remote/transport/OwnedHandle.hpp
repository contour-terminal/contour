// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// A handle this process owns, closed when its owner is destroyed: a descriptor on POSIX, a HANDLE on
/// Windows. What a shared memory object is made as, and what crosses to the extension host process.

#include <utility>

#include <vtextension/wasm/remote/transport/Channel.hpp>

namespace vtextension::wasm::remote::transport
{

#ifdef _WIN32
/// The value no handle has: null, which no section or pipe handle is.
inline constexpr auto NoNativeHandle = nullptr;
#else
/// The value no descriptor has.
inline constexpr NativeHandle NoNativeHandle = -1;
#endif

/// Owns one handle, and closes it when destroyed. Move-only; a moved-from one owns nothing.
class OwnedHandle
{
  public:
    /// Owns nothing.
    OwnedHandle() noexcept = default;

    /// Takes ownership of @p handle.
    /// @param handle The handle; NoNativeHandle for none.
    explicit OwnedHandle(NativeHandle handle) noexcept: _handle { handle } {}

    OwnedHandle(OwnedHandle&& other) noexcept: _handle { other.release() } {}

    OwnedHandle& operator=(OwnedHandle&& other) noexcept
    {
        if (this != &other)
            reset(other.release());
        return *this;
    }

    OwnedHandle(OwnedHandle const&) = delete;
    OwnedHandle& operator=(OwnedHandle const&) = delete;

    ~OwnedHandle() { reset(NoNativeHandle); }

    /// @return The handle, still owned; NoNativeHandle if none.
    [[nodiscard]] NativeHandle get() const noexcept { return _handle; }

    /// Gives the handle up, unclosed.
    /// @return The handle, now the caller's; NoNativeHandle if none.
    [[nodiscard]] NativeHandle release() noexcept { return std::exchange(_handle, NoNativeHandle); }

    /// @return Whether it owns a handle.
    [[nodiscard]] explicit operator bool() const noexcept { return _handle != NoNativeHandle; }

  private:
    /// Closes the handle owned, if any, and owns @p handle instead.
    void reset(NativeHandle handle) noexcept
    {
        if (auto const previous = std::exchange(_handle, handle); previous != NoNativeHandle)
            closeNativeHandle(previous);
    }

    NativeHandle _handle = NoNativeHandle;
};

} // namespace vtextension::wasm::remote::transport
