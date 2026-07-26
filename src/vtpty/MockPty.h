// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/Pty.h>

#include <crispy/BufferObject.h>

#include <cstdint>
#include <string>

namespace vtpty
{

/// How MockPty::write() responds, so a test can drive the failure paths a real PTY only reaches when
/// the far end breaks.
enum class PtyWriteBehavior : uint8_t
{
    Accept = 0,  ///< Stores the bytes and reports all of them written.
    FailAgain,   ///< Reports -1/EAGAIN: transient backpressure, the caller should retry.
    FailFatally, ///< Reports -1/EIO: the device is gone, the bytes can never be delivered.
};

/// Mock-PTY, to be used in unit tests.
class MockPty: public Pty
{
  public:
    explicit MockPty(PageSize windowSize);
    ~MockPty() override = default;

    PtySlave& slave() noexcept override;
    [[nodiscard]] std::optional<ReadResult> read(crispy::buffer_object<char>& storage,
                                                 std::optional<std::chrono::milliseconds> timeout,
                                                 size_t size) override;
    void wakeupReader() override;
    int write(std::string_view data) override;
    [[nodiscard]] PageSize pageSize() const noexcept override;
    void resizeScreen(PageSize cells, std::optional<ImageSize> pixels = std::nullopt) override;

    void start() override;
    void close() override;
    void waitForClosed() override;
    [[nodiscard]] bool isClosed() const noexcept override;

    /// Selects how the next write() responds. Runtime state rather than construction-time
    /// configuration: a test flips it mid-run to make a write start failing, which is precisely the
    /// transition under test.
    void setWriteBehavior(PtyWriteBehavior behavior) noexcept { _writeBehavior = behavior; }

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
    std::optional<ImageSize> _pixelSize;
    std::string _inputBuffer;
    std::string _outputBuffer;
    std::size_t _outputReadOffset = 0;
    bool _closed = false;
    PtyWriteBehavior _writeBehavior = PtyWriteBehavior::Accept;
    PtySlaveDummy _slave;
};

} // namespace vtpty
