// SPDX-License-Identifier: Apache-2.0
#include <core/Assert.hpp>
#include <core/SystemRandomSource.hpp>

// clang-format off
#include <windows.h>
#include <bcrypt.h>
// clang-format on

#include <algorithm>
#include <limits>

namespace core::detail
{

void fillFromSystemRandom(std::span<std::byte> out)
{
    while (!out.empty())
    {
        // BCryptGenRandom takes a ULONG count; a larger request goes in pieces.
        auto const chunk = std::min<std::size_t>(out.size(), std::numeric_limits<ULONG>::max());
        auto const status = ::BCryptGenRandom(nullptr,
                                              reinterpret_cast<PUCHAR>(out.data()),
                                              static_cast<ULONG>(chunk),
                                              BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (status != 0) // STATUS_SUCCESS
            core::detail::fail("BCryptGenRandom()", "The system random source failed.", __FILE__, __LINE__);
        out = out.subspan(chunk);
    }
}

} // namespace core::detail
