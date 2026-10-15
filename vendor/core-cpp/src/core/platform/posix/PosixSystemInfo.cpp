// SPDX-License-Identifier: Apache-2.0
#include <core/platform/SystemInfo.hpp>

#include <array>
#include <string>

#include <unistd.h>

namespace core::platform
{

std::string hostName()
{
    auto buf = std::array<char, 256> {};
    if (gethostname(buf.data(), buf.size()) == 0)
        return { buf.data() };
    return {};
}

} // namespace core::platform
