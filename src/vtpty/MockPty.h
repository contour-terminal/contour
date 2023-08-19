// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/Pty.h>

#include <crispy/BufferObject.h>

#include <string>

namespace terminal
{

/// Mock-PTY, to be used in unit tests.
class MockPty: public Pty
{
  public:
    explicit MockPty(PageSize windowSize);
    ~MockPty() override = default;

    PtySlave& slave() noexcept override;
    [[nodiscard]] ReadResult read(crispy::buffer_object<char>& storage,
                                  std::chrono::milliseconds timeout,
                                  size_t size) override;
    void wakeupReader() override;
    int write(std::string_view data) override;
    [[nodiscard]] PageSize pageSize() const noexcept override;
    void resizeScreen(PageSize cells, std::optional<crispy::image_size> pixels = std::nullopt) override;

    void start() override;
    void close() override;
    [[nodiscard]] bool isClosed() const noexcept override;

    [[nodiscard]] std::string& stdinBuffer() noexcept { return _inputBuffer; }
    [[nodiscard]] std::string const& stdinBuffer() const noexcept { return _inputBuffer; }

    [[nodiscard]] bool isStdoutDataAvailable() const noexcept
    {
        return _outputReadOffset < _outputBuffer.size();
    }

    void appendStdOutBuffer(std::string_view that)
    {
        if (_outputReadOffset == _outputBuffer.size())
        {
            _outputReadOffset = 0;
            _outputBuffer = that;
        }
        else
        {
            _outputBuffer += that;
        }
    }

  private:
    PageSize _pageSize;
    std::optional<crispy::image_size> _pixelSize;
    std::string _inputBuffer;
    std::string _outputBuffer;
    std::size_t _outputReadOffset = 0;
    bool _closed = false;
    PtySlaveDummy _slave;
};

} // namespace terminal
