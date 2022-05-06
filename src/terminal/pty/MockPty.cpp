#include <terminal/pty/MockPty.h>

using namespace std::chrono;
using std::min;
using std::nullopt;
using std::optional;
using std::string_view;
using std::tuple;

namespace terminal
{

MockPty::MockPty(PageSize _size): pageSize_ { _size }
{
}

MockPty::~MockPty()
{
}

PtySlave& MockPty::slave() noexcept
{
    return slave_;
}

Pty::ReadResult MockPty::read(crispy::BufferObject& storage,
                              std::chrono::milliseconds /*timeout*/,
                              size_t size)
{
    auto const n = min(size, min(outputBuffer_.size() - outputReadOffset_, storage.bytesAvailable()));
    auto const chunk = string_view { outputBuffer_.data() + outputReadOffset_, n };
    outputReadOffset_ += n;
    return { tuple { storage.writeAtEnd(chunk), false } };
}

void MockPty::wakeupReader()
{
    // No-op. as we're a mock-pty.
}

int MockPty::write(char const* buf, size_t size)
{
    // Writing into stdin.
    inputBuffer_ += std::string_view(buf, size);
    return static_cast<int>(size);
}

PageSize MockPty::pageSize() const noexcept
{
    return pageSize_;
}

void MockPty::resizeScreen(PageSize _cells, std::optional<ImageSize> _pixels)
{
    pageSize_ = _cells;
    pixelSize_ = _pixels;
}

void MockPty::close()
{
    closed_ = true;
}

bool MockPty::isClosed() const noexcept
{
    return closed_;
}

} // namespace terminal
