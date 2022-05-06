/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2021 Christian Parpart <christian@parpart.family>
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

#include <terminal/pty/Pty.h>

namespace terminal
{

class MockViewPty: public Pty
{
  public:
    explicit MockViewPty(PageSize _windowSize): pageSize_ { _windowSize } {}

    void setReadData(std::string_view _data);

    PtySlave& slave() noexcept override;
    [[nodiscard]] std::optional<std::tuple<std::string_view, bool>> read(crispy::BufferObject& storage,
                                                                         std::chrono::milliseconds timeout,
                                                                         size_t size) override;
    void wakeupReader() override;
    int write(char const* buf, size_t size) override;
    [[nodiscard]] PageSize pageSize() const noexcept override;
    void resizeScreen(PageSize _cells, std::optional<ImageSize> _pixels = std::nullopt) override;

    void close() override;
    [[nodiscard]] bool isClosed() const noexcept override;

    [[nodiscard]] std::string& stdinBuffer() noexcept { return inputBuffer_; }
    [[nodiscard]] std::string_view& stdoutBuffer() noexcept { return outputBuffer_; }

  private:
    PageSize pageSize_;
    std::optional<ImageSize> pixelSize_;
    std::string inputBuffer_;
    std::string_view outputBuffer_;
    bool closed_ = false;
    PtySlaveDummy slave_;
};

} // namespace terminal
