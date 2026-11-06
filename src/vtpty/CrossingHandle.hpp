// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The one type a native handle or descriptor crosses a live upgrade as, on every platform.

#include <core/platform/Types.hpp>

#include <compare>
#include <cstdint>

namespace vtpty
{

/// A native handle as it crosses to the process that takes over a live upgrade: a POSIX descriptor, or a
/// Windows HANDLE or SOCKET widened to an integer. The handover codec carries it as a one-field frame
/// holding a signed integer; a missing field reads as none.
///
/// Windows keeps every kernel handle within 32 bits, so a handle's value survives the round trip; a socket is
/// a handle there too.
struct CrossingHandle
{
    std::int64_t value = -1; ///< The number; -1 for none.

    /// @param handle A native handle of this process.
    /// @return @p handle as it crosses; none for an invalid one, and on Windows for a null one too.
    [[nodiscard]] static CrossingHandle of(core::platform::NativeHandle handle) noexcept
    {
#ifdef _WIN32
        if (handle == nullptr)
            return CrossingHandle {};
#endif
        if (handle == core::platform::InvalidHandle)
            return CrossingHandle {};
        return CrossingHandle { core::platform::nativeHandleToNumber(handle) };
    }

    /// @return The native handle it names; core::platform::InvalidHandle for none.
    [[nodiscard]] core::platform::NativeHandle native() const noexcept
    {
        if (isNone())
            return core::platform::InvalidHandle;
#ifdef _WIN32
        return reinterpret_cast<core::platform::NativeHandle>(static_cast<std::intptr_t>(value));
#else
        return static_cast<core::platform::NativeHandle>(value);
#endif
    }

    /// @return Whether it names no handle.
    [[nodiscard]] constexpr bool isNone() const noexcept { return value == -1; }

    /// Orders handles by their number, none (-1) first: what sorting and deduplicating them asks.
    auto operator<=>(CrossingHandle const&) const = default;
};

} // namespace vtpty
