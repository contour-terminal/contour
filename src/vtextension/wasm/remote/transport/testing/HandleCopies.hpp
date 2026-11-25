// SPDX-License-Identifier: Apache-2.0
//
// Test-only: second handles on what a transport end names, as the extension host process inherits them.
#pragma once

#include <optional>

#include <vtextension/wasm/remote/transport/Channel.hpp>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <Windows.h>
#else
    #include <unistd.h>
#endif

namespace vtextension::wasm::remote::transport::testing
{

/// @return A second handle on what @p handle names, as an inherited one is: not close-on-exec (POSIX);
/// nothing
///         if it failed.
[[nodiscard]] inline std::optional<NativeHandle> duplicate(NativeHandle handle) noexcept
{
#ifdef _WIN32
    auto copy = HANDLE { nullptr };
    if (!::DuplicateHandle(
            ::GetCurrentProcess(), handle, ::GetCurrentProcess(), &copy, 0, FALSE, DUPLICATE_SAME_ACCESS))
        return std::nullopt;
    return copy;
#else
    auto const copy = ::dup(handle);
    if (copy == -1)
        return std::nullopt;
    return copy;
#endif
}

/// Closes @p handle.
inline void closeHandle(NativeHandle handle) noexcept
{
#ifdef _WIN32
    ::CloseHandle(handle);
#else
    ::close(handle);
#endif
}

} // namespace vtextension::wasm::remote::transport::testing
