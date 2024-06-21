// SPDX-License-Identifier: Apache-2.0

#include <vtpty/MockViewPty.h>

#include <crispy/BufferObject.h>

namespace vtpty
{

void MockViewPty::setReadData(std::string_view data)
{
    assert(_outputBuffer.empty());
    _outputBuffer = data;
}

PtySlave& MockViewPty::slave() noexcept
{
    return _slave;
}

std::optional<Pty::ReadResult> MockViewPty::read(crispy::buffer_object<char>& storage,
                                                 std::optional<std::chrono::milliseconds> /*timeout*/,
                                                 size_t size)
{
    auto const n = std::min(std::min(_outputBuffer.size(), storage.bytesAvailable()), size);
    auto result = storage.writeAtEnd(_outputBuffer.substr(0, n));
    _outputBuffer.remove_prefix(n);
    return ReadResult { .data = std::string_view(result.data(), result.size()), .fromStdoutFastPipe = false };
}

void MockViewPty::wakeupReader()
{
    // No-op. as we're a mock-pty.
}

int MockViewPty::write(std::string_view data)
{
    // Writing into stdin.
    _inputBuffer += std::string_view(data.data(), data.size());
    return static_cast<int>(data.size());
}

PageSize MockViewPty::pageSize() const noexcept
{
    return _pageSize;
}

void MockViewPty::resizeScreen(PageSize cells, std::optional<ImageSize> pixels)
{
    _pageSize = cells;
    _pixelSize = pixels;
}

void MockViewPty::start()
{
    _closed = false;
}

void MockViewPty::close()
{
    _closed = true;
}

void MockViewPty::waitForClosed()
{
    // No-op. as we're a mock-pty.
}

bool MockViewPty::isClosed() const noexcept
{
    return _closed;
}

} // namespace vtpty
