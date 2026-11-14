// SPDX-License-Identifier: Apache-2.0
/// @file
/// The process id DaemonBinary.hpp declares. It lives in vthost_image, beside DaemonBinary.cpp which asks for
/// it, and not in ProcessImage_native.cpp: that file is vthost's, and the PTY keeper links vthost_image only.

// clang-format off
#ifdef _WIN32
    #include <windows.h>
#endif
// clang-format on

#include <cstdint>

#include <vthost/DaemonBinary.hpp>

#ifndef _WIN32
    #include <unistd.h>
#endif

namespace vthost
{

std::uint64_t currentProcessId() noexcept
{
#ifdef _WIN32
    return ::GetCurrentProcessId();
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

} // namespace vthost
