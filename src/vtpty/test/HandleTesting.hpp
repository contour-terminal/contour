// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Whether a handle or descriptor is open in this process: what the handover tests of vtpty and vthost ask
/// of what a live upgrade keeps, closes or hands on.

#include <vtpty/CrossingHandle.hpp>

#include <core/platform/Types.hpp>

#ifdef _WIN32
    #include <Windows.h>
#else
    #include <fcntl.h>
#endif

namespace vtpty::testing
{

/// @param handle A native handle (Windows) or descriptor (POSIX) of this process.
/// @return Whether @p handle is open in this process. Allocates nothing, so a forked child may ask too.
[[nodiscard]] inline bool isOpen(core::platform::NativeHandle handle) noexcept
{
#ifdef _WIN32
    auto flags = DWORD {};
    return GetHandleInformation(handle, &flags) != FALSE;
#else
    return ::fcntl(handle, F_GETFD) != -1;
#endif
}

/// @param handle A handle or descriptor, as it crosses a live upgrade.
/// @return Whether @p handle is open in this process.
[[nodiscard]] inline bool isOpen(CrossingHandle handle) noexcept
{
    return isOpen(handle.native());
}

} // namespace vtpty::testing
