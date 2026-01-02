// SPDX-License-Identifier: Apache-2.0
#include <vtpty/HandoffPty.h>

#include <crispy/utils.h>

#include <iostream>

using namespace std;

namespace vtpty
{

HandoffPty::HandoffPty(HANDLE hInputWrite,
                       HANDLE hOutputRead,
                       HANDLE hSignal,
                       HANDLE hReference,
                       HANDLE hServer,
                       HANDLE hClient,
                       std::wstring const& title):
    _hInputWrite(hInputWrite),
    _hOutputRead(hOutputRead),
    _hSignal(hSignal),
    _hReference(hReference),
    _hServer(hServer),
    _hClient(hClient),
    _title(title)
{
    _hWakeup = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    _readOverlapped.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    _writeOverlapped.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);

    // Default page size
    _pageSize.lines = LineCount(24);
    _pageSize.columns = ColumnCount(80);
}

HandoffPty::~HandoffPty()
{
    close();
    CloseHandle(_hWakeup);
    CloseHandle(_readOverlapped.hEvent);
    CloseHandle(_writeOverlapped.hEvent);
}

void HandoffPty::start()
{
    // Nothing to do
}

PtySlave& HandoffPty::slave() noexcept
{
    return _slave;
}

void HandoffPty::close()
{
    if (_closed)
        return;
    _closed = true;

    auto const closeAndInvalidate = [](HANDLE& handle) {
        if (handle != INVALID_HANDLE_VALUE)
        {
            CloseHandle(handle);
            handle = INVALID_HANDLE_VALUE;
        }
    };

    closeAndInvalidate(_hInputWrite);

    if (_hOutputRead != INVALID_HANDLE_VALUE)
    {
        // CancelIO might be needed?
        CancelIo(_hOutputRead);
        closeAndInvalidate(_hOutputRead);
    }

    closeAndInvalidate(_hSignal);
    closeAndInvalidate(_hReference);
    closeAndInvalidate(_hServer);
    closeAndInvalidate(_hClient);
}

void HandoffPty::waitForClosed()
{
    // Not strictly implemented, we assume close happens.
}

bool HandoffPty::isClosed() const noexcept
{
    return _closed;
}

std::optional<Pty::ReadResult> HandoffPty::read(crispy::buffer_object<char>& storage,
                                                std::optional<std::chrono::milliseconds> timeout,
                                                size_t size)
{
    if (_closed)
        return std::nullopt;

    DWORD bytesRead = 0;

    // Reset event
    ResetEvent(_readOverlapped.hEvent);

    // Ensure storage has space
    // We assume storage has some capacity or we can write to it directly?
    // buffer_object has .data() and .capacity()?
    // But buffer_object is usually a wrapper around a resource pool.
    // We can use a temporary buffer.

    if (_readBuffer.size() < size)
        _readBuffer.resize(size);

    BOOL res =
        ReadFile(_hOutputRead, _readBuffer.data(), static_cast<DWORD>(size), &bytesRead, &_readOverlapped);
    if (!res)
    {
        DWORD err = GetLastError();
        if (err == ERROR_IO_PENDING)
        {
            HANDLE handles[2] = { _readOverlapped.hEvent, _hWakeup };
            DWORD timeoutMs = timeout ? static_cast<DWORD>(timeout->count()) : INFINITE;

            DWORD waitRes = WaitForMultipleObjects(2, handles, FALSE, timeoutMs);
            if (waitRes == WAIT_OBJECT_0)
            {
                // Read completed
                if (GetOverlappedResult(_hOutputRead, &_readOverlapped, &bytesRead, FALSE))
                {
                    // Success
                }
                else
                {
                    // Error
                    if (GetLastError() == ERROR_BROKEN_PIPE)
                        return std::nullopt; // EOF
                    return std::nullopt;
                }
            }
            else if (waitRes == WAIT_OBJECT_0 + 1)
            {
                // Wakeup
                CancelIo(_hOutputRead);
                return std::nullopt; // Or empty result?
            }
            else // Timeout or failed
            {
                CancelIo(_hOutputRead);
                return std::nullopt;
            }
        }
        else if (err == ERROR_BROKEN_PIPE)
        {
            return std::nullopt; // EOF
        }
        else
        {
            return std::nullopt;
        }
    }

    if (bytesRead > 0)
    {
        // Copy to storage
        auto chunk = storage.advance(bytesRead);
        std::copy(_readBuffer.begin(), _readBuffer.begin() + bytesRead, chunk.data());
        return ReadResult { std::string_view(chunk.data(), chunk.size()), false };
    }

    return std::nullopt;
}

void HandoffPty::wakeupReader()
{
    SetEvent(_hWakeup);
}

int HandoffPty::write(std::string_view buf)
{
    if (_closed)
        return -1;

    DWORD bytesWritten = 0;
    // Handle overlapped write synchronously for now (or wait)
    // Or async? WriteFile usually buffers.

    // Reset event
    ResetEvent(_writeOverlapped.hEvent);

    BOOL res =
        WriteFile(_hInputWrite, buf.data(), static_cast<DWORD>(buf.size()), &bytesWritten, &_writeOverlapped);
    if (!res)
    {
        if (GetLastError() == ERROR_IO_PENDING)
        {
            if (GetOverlappedResult(_hInputWrite, &_writeOverlapped, &bytesWritten, TRUE))
            {
                return static_cast<int>(bytesWritten);
            }
        }
        return -1;
    }
    return static_cast<int>(bytesWritten);
}

PageSize HandoffPty::pageSize() const noexcept
{
    return _pageSize;
}

void HandoffPty::resizeScreen(PageSize cells, std::optional<ImageSize> pixels)
{
    _pageSize = cells;
    // Resizing is not propagated to OpenConsoleProxy via handles.
    // It's likely propagated via VT sequences or other mechanism not exposed here?
    // Or maybe we don't need to propagate.
}

} // namespace vtpty
