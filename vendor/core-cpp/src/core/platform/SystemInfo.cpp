// SPDX-License-Identifier: Apache-2.0
#include <core/platform/SystemInfo.hpp>

#include <array>

#ifdef _WIN32
    #include <windows.h>
#else
    #include <unistd.h>
#endif

namespace core::platform
{

std::string hostName()
{
    auto buf = std::array<char, 256> {};
#ifdef _WIN32
    auto bufLen = static_cast<DWORD>(buf.size());
    if (GetComputerNameA(buf.data(), &bufLen))
        return { buf.data(), bufLen };
#else
    if (gethostname(buf.data(), buf.size()) == 0)
        return { buf.data() };
#endif
    return {};
}

std::string const& cachedHostName()
{
    static std::string const cached = hostName();
    return cached;
}

} // namespace core::platform
