// SPDX-License-Identifier: Apache-2.0
//
// The Windows half of DescriptorTestSupport.hpp's platform questions. Wine's ntdll exports
// `wine_get_version`, and no Windows ntdll does: that export is wine's own documented way for a
// program to tell, and the one a case uses before it accepts a behaviour only wine has.
#include <core/fs/DescriptorTestSupport.hpp>

#include <windows.h>

namespace core::fs::test
{

bool runsUnderWine() noexcept
{
    auto* const ntdll = GetModuleHandleW(L"ntdll.dll");
    return ntdll != nullptr && GetProcAddress(ntdll, "wine_get_version") != nullptr;
}

} // namespace core::fs::test
