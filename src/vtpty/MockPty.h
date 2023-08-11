/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <vtpty/Pty.h>

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
    [[nodiscard]] ReadResult read(crispy::BufferObject<char>& storage,
                                  std::chrono::milliseconds timeout,
                                  size_t size) override;
    void wakeupReader() override;
    int write(std::string_view data, bool blocking) override;
    [[nodiscard]] PageSize pageSize() const noexcept override;
    void resizeScreen(PageSize cells, std::optional<crispy::ImageSize> pixels = std::nullopt) override;

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
    std::optional<crispy::ImageSize> _pixelSize;
    std::string _inputBuffer;
    std::string _outputBuffer;
    std::size_t _outputReadOffset = 0;
    bool _closed = false;
    PtySlaveDummy _slave;
};

} // namespace terminal
