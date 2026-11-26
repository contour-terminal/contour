// SPDX-License-Identifier: Apache-2.0
//
// Test-only: whether a test runs on wine, whose Windows differs from Windows where a case says so.
// Header-only, so that every test binary, whatever it links, asks it alike.
#pragma once

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <Windows.h>
#endif

namespace vtextension::wasm::testing
{

/// @return Whether this process runs on wine (ntdll exports wine_get_version()). False off Windows.
[[nodiscard]] inline bool runsOnWine() noexcept
{
#ifdef _WIN32
    auto* const ntdll = ::GetModuleHandleW(L"ntdll.dll");
    return ntdll != nullptr && ::GetProcAddress(ntdll, "wine_get_version") != nullptr;
#else
    return false;
#endif
}

} // namespace vtextension::wasm::testing
