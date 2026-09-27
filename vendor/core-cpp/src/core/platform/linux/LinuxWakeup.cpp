// SPDX-License-Identifier: Apache-2.0
#include <core/platform/Wakeup.hpp>

#include <sys/eventfd.h>

#include <cerrno>
#include <cstdint>
#include <stdexcept>

#include <unistd.h>

namespace core::platform
{

Wakeup::Wakeup(): _handle { eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC) }
{
    if (_handle == InvalidHandle)
        throw std::runtime_error("Failed to create eventfd");
}

Wakeup::~Wakeup()
{
    if (_handle != InvalidHandle)
        ::close(_handle);
}

void Wakeup::signal() const
{
    auto const value = uint64_t { 1 };
    // Retry on EINTR; ignore EAGAIN (already signaled).
    while (::write(_handle, &value, sizeof(value)) == -1)
    {
        if (errno == EINTR)
            continue;
        break; // EAGAIN means already signaled — that's fine.
    }
}

void Wakeup::reset() const
{
    auto value = uint64_t {};
    // Drain the eventfd counter. Ignore errors (EAGAIN means not signaled).
    while (::read(_handle, &value, sizeof(value)) == -1)
    {
        if (errno == EINTR)
            continue;
        break;
    }
}

auto Wakeup::nativeHandle() const noexcept -> NativeHandle
{
    return _handle;
}

} // namespace core::platform
