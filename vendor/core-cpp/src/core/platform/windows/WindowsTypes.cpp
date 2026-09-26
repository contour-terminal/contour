// SPDX-License-Identifier: Apache-2.0
#include <core/platform/Types.hpp>

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <Windows.h>

// Types.hpp spells these without <Windows.h>; this is where the two spellings are held equal.
static_assert(std::is_same_v<core::platform::NativeHandle, HANDLE>);
static_assert(std::is_same_v<core::platform::ProcessId, DWORD>);

namespace core::platform
{

NativeHandle standardInput() noexcept
{
    return GetStdHandle(STD_INPUT_HANDLE);
}

NativeHandle standardOutput() noexcept
{
    return GetStdHandle(STD_OUTPUT_HANDLE);
}

NativeHandle standardError() noexcept
{
    return GetStdHandle(STD_ERROR_HANDLE);
}

void platformClose(NativeHandle h) noexcept
{
    if (h != InvalidHandle)
        CloseHandle(h);
}

std::intptr_t platformWrite(NativeHandle h, void const* data, std::size_t size)
{
    DWORD written = 0;
    if (!WriteFile(h, data, static_cast<DWORD>(size), &written, nullptr))
        return -1;
    return static_cast<std::intptr_t>(written);
}

std::intptr_t platformRead(NativeHandle h, void* data, std::size_t size)
{
    DWORD bytesRead = 0;
    if (!ReadFile(h, data, static_cast<DWORD>(size), &bytesRead, nullptr))
    {
        // ERROR_BROKEN_PIPE / ERROR_HANDLE_EOF are stream terminators, not
        // errors — normalize them to POSIX-style EOF so the cross-platform
        // abstraction is uniform for callers that distinguish EOF from error.
        auto const err = GetLastError();
        if (err == ERROR_BROKEN_PIPE || err == ERROR_HANDLE_EOF)
            return 0;
        return -1;
    }
    return static_cast<std::intptr_t>(bytesRead);
}

bool isTerminal(NativeHandle h) noexcept
{
    DWORD mode = 0;
    return GetConsoleMode(h, &mode) != 0;
}

} // namespace core::platform
