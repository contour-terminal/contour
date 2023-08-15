#include <vtpty/MockPty.h>

#include <crispy/BufferObject.h>

using namespace std::chrono;
using std::min;
using std::nullopt;
using std::optional;
using std::string_view;
using std::tuple;

namespace terminal
{

MockPty::MockPty(PageSize size): _pageSize { size }
{
}

PtySlave& MockPty::slave() noexcept
{
    return _slave;
}

Pty::ReadResult MockPty::read(crispy::buffer_object<char>& storage,
                              std::chrono::milliseconds /*timeout*/,
                              size_t size)
{
    auto const n = min(size, min(_outputBuffer.size() - _outputReadOffset, storage.bytesAvailable()));
    auto const chunk = string_view { _outputBuffer.data() + _outputReadOffset, n };
    _outputReadOffset += n;
    auto const pooled = storage.writeAtEnd(chunk);
    return { tuple { string_view(pooled.data(), pooled.size()), false } };
}

void MockPty::wakeupReader()
{
    // No-op. as we're a mock-pty.
}

int MockPty::write(std::string_view data)
{
    // Writing into stdin.
    _inputBuffer += std::string_view(data.data(), data.size());
    return static_cast<int>(data.size());
}

PageSize MockPty::pageSize() const noexcept
{
    return _pageSize;
}

void MockPty::resizeScreen(PageSize cells, std::optional<crispy::image_size> pixels)
{
    _pageSize = cells;
    _pixelSize = pixels;
}

void MockPty::start()
{
    _closed = false;
}

void MockPty::close()
{
    _closed = true;
}

bool MockPty::isClosed() const noexcept
{
    return _closed;
}

} // namespace terminal
