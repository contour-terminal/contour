// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/Pty.h>

#include <memory>
#include <mutex>
#include <vector>

#include <Windows.h>

#include "crispy/BufferObject.h"

namespace terminal
{

/// ConPty implementation for newer Windows 10 versions.
class ConPty: public Pty
{
  public:
    explicit ConPty(PageSize const& windowSize);
    ~ConPty() override;

    void start() override;
    void close() override;
    [[nodiscard]] bool isClosed() const noexcept override;

    [[nodiscard]] ReadResult read(crispy::buffer_object<char>& storage,
                                  std::chrono::milliseconds timeout,
                                  size_t size) override;
    void wakeupReader() override;
    int write(std::string_view data) override;
    [[nodiscard]] PageSize pageSize() const noexcept override;
    void resizeScreen(PageSize cells, std::optional<crispy::image_size> pixels = std::nullopt) override;

    PtySlave& slave() noexcept override;
    HPCON master() const noexcept { return _master; }

  private:
    std::mutex _mutex; // used to guard close()
    PageSize _size;
    HPCON _master;
    HANDLE _input;
    HANDLE _output;
    std::vector<char> _buffer;
    std::unique_ptr<PtySlave> _slave;
};

} // namespace terminal
