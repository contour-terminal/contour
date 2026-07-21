// SPDX-License-Identifier: Apache-2.0
#include <vtpty/ChannelPty.h>

#include <algorithm>
#include <cerrno>
#include <utility>

namespace vtpty
{

void ChannelPty::setWriteSink(WriteSink sink)
{
    auto const lock = std::lock_guard { _mutex };
    _writeSink = std::move(sink);
}

void ChannelPty::setResizeSink(ResizeSink sink)
{
    auto const lock = std::lock_guard { _mutex };
    _resizeSink = std::move(sink);
}

void ChannelPty::feed(std::string_view data)
{
    {
        auto const lock = std::lock_guard { _mutex };
        if (_outputReadOffset == _outputBuffer.size())
        {
            _outputReadOffset = 0;
            _outputBuffer.assign(data);
        }
        else
            _outputBuffer.append(data);
    }
    _wakeup.notify_all();
}

std::string ChannelPty::stdinSnapshot() const
{
    auto const lock = std::lock_guard { _mutex };
    return _inputBuffer;
}

bool ChannelPty::isStdoutPending() const
{
    auto const lock = std::lock_guard { _mutex };
    return _outputReadOffset < _outputBuffer.size();
}

std::optional<Pty::ReadResult> ChannelPty::read(crispy::buffer_object<char>& storage,
                                                std::optional<std::chrono::milliseconds> timeout,
                                                size_t size)
{
    auto lock = std::unique_lock { _mutex };
    auto const wake = [this]() {
        return _closed || _woken || _outputReadOffset < _outputBuffer.size();
    };
    if (timeout.has_value())
        _wakeup.wait_for(lock, *timeout, wake);
    else
        _wakeup.wait(lock, wake);

    if (_outputReadOffset == _outputBuffer.size())
    {
        if (_closed)
            // Drained and closed: report EOF (empty read), as a real PTY does.
            return ReadResult { .data = std::string_view {}, .fromStdoutFastPipe = false };
        // A bare wakeupReader() (teardown wake, no data) or a genuine timeout:
        // return EAGAIN so the caller re-checks its terminating flag, exactly
        // as UnixPty's break-pipe wake does.
        _woken = false;
        errno = EAGAIN;
        return std::nullopt;
    }

    auto const n = std::min({ size, _outputBuffer.size() - _outputReadOffset, storage.bytesAvailable() });
    auto const chunk = std::string_view { _outputBuffer.data() + _outputReadOffset, n };
    _outputReadOffset += n;
    auto const pooled = storage.writeAtEnd(chunk);
    return ReadResult { .data = std::string_view(pooled.data(), pooled.size()), .fromStdoutFastPipe = false };
}

void ChannelPty::wakeupReader()
{
    {
        auto const lock = std::lock_guard { _mutex };
        _woken = true;
    }
    _wakeup.notify_all();
}

int ChannelPty::write(std::string_view data)
{
    auto sink = WriteSink {};
    {
        auto const lock = std::lock_guard { _mutex };
        if (_writeSink)
            sink = _writeSink;
        else
            _inputBuffer.append(data);
    }
    // Invoked outside the lock: the sink typically marshals onto an event
    // loop, and holding the pty lock across foreign code invites deadlocks.
    if (sink)
        sink(data);
    return static_cast<int>(data.size());
}

PageSize ChannelPty::pageSize() const noexcept
{
    auto const lock = std::lock_guard { _mutex };
    return _pageSize;
}

void ChannelPty::resizeScreen(PageSize cells, std::optional<ImageSize> pixels)
{
    auto sink = ResizeSink {};
    {
        auto const lock = std::lock_guard { _mutex };
        _pageSize = cells;
        _pixelSize = pixels;
        sink = _resizeSink;
    }
    if (sink)
        sink(cells, pixels);
}

void ChannelPty::close()
{
    {
        auto const lock = std::lock_guard { _mutex };
        _closed = true;
    }
    _wakeup.notify_all();
}

void ChannelPty::waitForClosed()
{
    auto lock = std::unique_lock { _mutex };
    _wakeup.wait(lock, [this]() { return _closed; });
}

bool ChannelPty::isClosed() const noexcept
{
    auto const lock = std::lock_guard { _mutex };
    return _closed;
}

} // namespace vtpty
