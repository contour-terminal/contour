// SPDX-License-Identifier: Apache-2.0
//
// Test-only: second handles on what a transport end names, as the extension host process inherits them.
#pragma once

#include <cstddef>
#include <optional>
#include <ranges>

#include <vtextension/wasm/remote/transport/Channel.hpp>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <Windows.h>
#else
    #include <sys/stat.h>
    #include <sys/types.h>

    #include <fcntl.h>
    #include <unistd.h>
#endif

namespace vtextension::wasm::remote::transport::testing
{

/// @return A second handle on what @p handle names, as an inherited one is: not close-on-exec (POSIX);
///         nothing if it failed.
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

#ifndef _WIN32
/// @return The inode @p descriptor names; 0 if it names nothing.
[[nodiscard]] inline ino_t inodeOf(NativeHandle descriptor) noexcept
{
    struct stat status {};
    return ::fstat(descriptor, &status) == 0 ? status.st_ino : 0;
}

/// @return How many descriptors this process has open, below 4096: asked of each number rather than listed
///         from /dev/fd, whose listing holds a descriptor of its own.
[[nodiscard]] inline std::size_t openDescriptorCount() noexcept
{
    auto count = std::size_t { 0 };
    for (auto const descriptor: std::views::iota(0, 4096))
        if (::fcntl(descriptor, F_GETFD) != -1)
            ++count;
    return count;
}
#endif

} // namespace vtextension::wasm::remote::transport::testing
