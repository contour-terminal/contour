// SPDX-License-Identifier: Apache-2.0
#include <vtpty/UniqueHandle.hpp>

#include <Windows.h>

namespace vtpty
{

void UniqueHandle::reset() noexcept
{
    if (valid())
        CloseHandle(_handle);
    _handle = nullptr;
}

} // namespace vtpty
