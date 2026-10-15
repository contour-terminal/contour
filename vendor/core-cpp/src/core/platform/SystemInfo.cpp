// SPDX-License-Identifier: Apache-2.0
//
// hostName() is per platform: posix/PosixSystemInfo.cpp and windows/WindowsSystemInfo.cpp.
#include <core/platform/SystemInfo.hpp>

#include <string>

namespace core::platform
{

std::string const& cachedHostName()
{
    static std::string const cached = hostName();
    return cached;
}

} // namespace core::platform
