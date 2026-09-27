// SPDX-License-Identifier: Apache-2.0
#include <core/platform/Wakeup.hpp>

#include <stdexcept>

#include <windows.h>

namespace core::platform
{

Wakeup::Wakeup():
    // Manual-reset event: stays signaled until ResetEvent() is called.
    _handle(CreateEvent(nullptr, TRUE, FALSE, nullptr))
{
    if (_handle == nullptr || _handle == INVALID_HANDLE_VALUE)
        throw std::runtime_error("Failed to create Wakeup event");
}

Wakeup::~Wakeup()
{
    if (_handle != InvalidHandle)
        CloseHandle(_handle);
}

void Wakeup::signal() const
{
    SetEvent(_handle);
}

void Wakeup::reset() const
{
    ResetEvent(_handle);
}

auto Wakeup::nativeHandle() const noexcept -> NativeHandle
{
    return _handle;
}

} // namespace core::platform
