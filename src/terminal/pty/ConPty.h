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

#include <Windows.h>
#include <memory>
#include <mutex>
#include <vector>

namespace terminal
{

/// ConPty implementation for newer Windows 10 versions.
class ConPty: public Pty
{
  public:
    explicit ConPty(PageSize const& windowSize);
    ~ConPty() override;

    void close() override;
    bool isClosed() const noexcept override;

    std::optional<std::string_view> read(size_t _size, std::chrono::milliseconds _timeout) override;
    [[nodiscard]] std::optional<std::tuple<std::string_view, bool>> read(crispy::BufferObject& storage,
                                                                         std::chrono::milliseconds timeout,
                                                                         size_t size) override;
    void wakeupReader() override;
    int write(char const* buf, size_t size) override;
    PageSize pageSize() const noexcept override;
    void resizeScreen(PageSize _cells, std::optional<ImageSize> _pixels = std::nullopt) override;

    PtySlave& slave() noexcept override;
    HPCON master() const noexcept { return master_; }

  private:
    std::mutex mutex_; // used to guard close()
    PageSize size_;
    HPCON master_;
    HANDLE input_;
    HANDLE output_;
    std::vector<char> buffer_;
    std::unique_ptr<PtySlave> slave_;
};

} // namespace terminal
