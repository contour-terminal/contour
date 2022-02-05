#include <terminal/pty/MockPty.h>

using namespace std::chrono;
using std::optional;
using std::string_view;

namespace terminal
{

MockPty::MockPty(PageSize _size): pageSize_ { _size }
{
}

MockPty::~MockPty()
{
}

optional<string_view> MockPty::read(size_t _size, std::chrono::milliseconds)
{
    auto const n = std::min(outputBuffer_.size() - outputReadOffset_, _size);
    auto const result = string_view { outputBuffer_.data() + outputReadOffset_, n };
    outputReadOffset_ += n;
    return result;
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

void MockPty::prepareChildProcess()
{
}

void MockPty::prepareParentProcess()
{
}

void MockPty::close()
{
    closed_ = true;
}

bool MockPty::isClosed() const
{
    return closed_;
}

} // namespace terminal
