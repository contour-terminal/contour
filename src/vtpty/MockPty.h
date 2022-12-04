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
    int write(char const* buf, size_t size) override;
    [[nodiscard]] PageSize pageSize() const noexcept override;
    void resizeScreen(PageSize _cells, std::optional<crispy::ImageSize> _pixels = std::nullopt) override;

    void start() override;
    void close() override;
    [[nodiscard]] bool isClosed() const noexcept override;

    std::string& stdinBuffer() noexcept { return inputBuffer_; }

    [[nodiscard]] bool isStdoutDataAvailable() const noexcept
    {
        return outputReadOffset_ < outputBuffer_.size();
    }

    void appendStdOutBuffer(std::string_view _that)
    {
        if (outputReadOffset_ == outputBuffer_.size())
        {
            outputReadOffset_ = 0;
            outputBuffer_ = _that;
        }
        else
        {
            outputBuffer_ += _that;
        }
    }

  private:
    PageSize pageSize_;
    std::optional<crispy::ImageSize> pixelSize_;
    std::string inputBuffer_;
    std::string outputBuffer_;
    std::size_t outputReadOffset_ = 0;
    bool closed_ = false;
    PtySlaveDummy slave_;
};

} // namespace terminal
