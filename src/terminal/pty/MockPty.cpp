#include <terminal/pty/MockPty.h>

using namespace std::chrono;

namespace terminal
{

MockPty::MockPty(PageSize _size):
    screenSize_{ _size }
{
}

MockPty::~MockPty()
{
}

int MockPty::read(char* _buf, size_t _size, std::chrono::milliseconds _timeout)
{
    // Reading from stdout.
    (void) _timeout;

    auto const n = std::min(outputBuffer_.size(), _size);
    std::copy(begin(outputBuffer_), next(begin(outputBuffer_), static_cast<int>(n)), _buf);
    outputBuffer_.erase(0, n);
    return static_cast<int>(n);
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

PageSize MockPty::screenSize() const noexcept
{
    return screenSize_;
}

void MockPty::resizeScreen(PageSize _cells, std::optional<ImageSize> _pixels)
{
    screenSize_ = _cells;
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

}
