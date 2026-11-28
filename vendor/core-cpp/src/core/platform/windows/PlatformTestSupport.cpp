// SPDX-License-Identifier: Apache-2.0
//
// The Windows half of PlatformTestSupport.hpp's platform questions. Wine's ntdll exports
// `wine_get_version`, and no Windows ntdll does: that export is wine's own documented way for a
// program to tell.
#include <core/platform/PlatformTestSupport.hpp>

#include <windows.h>

namespace core::platform::test
{

bool runsUnderWine() noexcept
{
    auto* const ntdll = GetModuleHandleW(L"ntdll.dll");
    return ntdll != nullptr && GetProcAddress(ntdll, "wine_get_version") != nullptr;
}

} // namespace core::platform::test
