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

#include <terminal/pty/Pty.h>

#include <array>
#include <optional>
#include <vector>

#if defined(__APPLE__)
#include <util.h>
#elif defined(__linux__)
#include <pty.h>
#endif

namespace terminal {

class UnixPty : public Pty
{
  public:
    explicit UnixPty(PageSize const& windowSize, std::optional<ImageSize> _pixels = std::nullopt);
    ~UnixPty() override;

    std::optional<std::string_view> read(size_t _size, std::chrono::milliseconds _timeout) override;
    void wakeupReader() override;
    int write(char const* buf, size_t size) override;
    PageSize screenSize() const noexcept override;
    void resizeScreen(PageSize _cells, std::optional<ImageSize> _pixels = std::nullopt) override;

    void prepareParentProcess() override;
    void prepareChildProcess() override;
    void close() override;
    bool isClosed() const override;
    [[nodiscard]] constexpr int masterFd() const noexcept { return master_; };

  private:
    PageSize size_;
    int master_;
    int slave_;
    std::array<int, 2> pipe_;
    std::vector<char> buffer_;
};

}  // namespace terminal
