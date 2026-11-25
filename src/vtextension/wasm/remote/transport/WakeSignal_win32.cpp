// SPDX-License-Identifier: Apache-2.0
//
// The wake signal on Windows: a manual-reset event, which SetEvent() sets without ever blocking. Manual, not
// auto-reset, so that the sender can tell a wake already queued (the event is set) from one it queues: the
// receiver resets it once its wait returned, and before its caller scans the page, so that a flag raised
// before a signal() that found the event set is still seen by that scan. The receiver waits on it beside a
// stop event of its own. A gone sender cannot be told on Windows; the job ends a helper whose parent is gone.
#include <array>
#include <cstdint>
#include <expected>
#include <utility>

#include <vtextension/wasm/remote/transport/WakeSignal.hpp>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace vtextension::wasm::remote::transport
{

namespace
{
    /// Closes @p handle if it is open.
    void closeHandle(HANDLE handle) noexcept
    {
        if (handle != nullptr)
            ::CloseHandle(handle);
    }

    /// @return A new, unset, uninheritable manual-reset event; null if the system refused.
    [[nodiscard]] HANDLE makeEvent() noexcept
    {
        return ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }
} // namespace

WakeSender::WakeSender(NativeHandle handle) noexcept: _handle { handle }
{
}

WakeSender::WakeSender(WakeSender&& other) noexcept: _handle { std::exchange(other._handle, nullptr) }
{
}

WakeSender& WakeSender::operator=(WakeSender&& other) noexcept
{
    if (this != &other)
    {
        closeHandle(_handle);
        _handle = std::exchange(other._handle, nullptr);
    }
    return *this;
}

WakeSender::~WakeSender()
{
    closeHandle(_handle);
}

WakeResult WakeSender::signal() const noexcept
{
    if (::WaitForSingleObject(_handle, 0) == WAIT_OBJECT_0)
        return WakeResult::Pending;
    ::SetEvent(_handle);
    return WakeResult::Sent;
}

NativeHandle WakeSender::nativeHandle() const noexcept
{
    return _handle;
}

WakeReceiver::WakeReceiver(NativeHandle handle, void* stopEvent) noexcept:
    _handle { handle }, _stopEvent { stopEvent }
{
}

std::expected<WakeReceiver, ChannelError> WakeReceiver::adopt(NativeHandle handle)
{
    // Waiting with a zero timeout tells an event (or any waitable object) from a handle that is not open.
    if (handle == nullptr || ::WaitForSingleObject(handle, 0) == WAIT_FAILED)
        return std::unexpected(ChannelError::Io);
    if (!::SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0))
        return std::unexpected(ChannelError::Io);
    auto* const stop = makeEvent();
    if (stop == nullptr)
        return std::unexpected(ChannelError::Io);
    return WakeReceiver { handle, stop };
}

WakeReceiver::WakeReceiver(WakeReceiver&& other) noexcept:
    _handle { std::exchange(other._handle, nullptr) }, _stopEvent { std::exchange(other._stopEvent, nullptr) }
{
}

WakeReceiver& WakeReceiver::operator=(WakeReceiver&& other) noexcept
{
    if (this != &other)
    {
        release();
        _handle = std::exchange(other._handle, nullptr);
        _stopEvent = std::exchange(other._stopEvent, nullptr);
    }
    return *this;
}

WakeReceiver::~WakeReceiver()
{
    release();
}

void WakeReceiver::release() noexcept
{
    closeHandle(_handle);
    closeHandle(_stopEvent);
    _handle = nullptr;
    _stopEvent = nullptr;
}

WakeWait WakeReceiver::wait() noexcept
{
    // The stop event first: WaitForMultipleObjects reports the lowest index signalled, so a stop wins.
    auto const handles = std::array<HANDLE, 2> { _stopEvent, _handle };
    switch (::WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE, INFINITE))
    {
        case WAIT_OBJECT_0: return WakeWait::Stopped;
        case WAIT_OBJECT_0 + 1: ::ResetEvent(_handle); return WakeWait::Woken;
        default: return WakeWait::PeerGone;
    }
}

void WakeReceiver::stop() const noexcept
{
    ::SetEvent(_stopEvent);
}

NativeHandle WakeReceiver::nativeHandle() const noexcept
{
    return _handle;
}

std::expected<WakePair, ChannelError> makeWakePair()
{
    auto* const wake = makeEvent();
    if (wake == nullptr)
        return std::unexpected(ChannelError::Io);
    auto* receiverEnd = HANDLE { nullptr };
    if (!::DuplicateHandle(::GetCurrentProcess(),
                           wake,
                           ::GetCurrentProcess(),
                           &receiverEnd,
                           0,
                           FALSE,
                           DUPLICATE_SAME_ACCESS))
    {
        ::CloseHandle(wake);
        return std::unexpected(ChannelError::Io);
    }
    auto sender = WakeSender { wake };
    auto* const stop = makeEvent();
    if (stop == nullptr)
    {
        ::CloseHandle(receiverEnd);
        return std::unexpected(ChannelError::Io);
    }
    return WakePair { .parent = std::move(sender), .helper = WakeReceiver { receiverEnd, stop } };
}

} // namespace vtextension::wasm::remote::transport
