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
#include <terminal/pty/UnixPty.h> // UnixPipe (TODO: move somewhere else)

#include <array>
#include <optional>
#include <vector>

#include <pty.h>

namespace terminal
{

class LinuxPty: public Pty
{
  private:
    class Slave: public PtySlave
    {
      public:
        int _slaveFd;
        explicit Slave(PtySlaveHandle fd): _slaveFd { unbox<int>(fd) } {}
        ~Slave() override;
        [[nodiscard]] PtySlaveHandle handle() const noexcept;
        void close() override;
        [[nodiscard]] bool isClosed() const noexcept override;
        [[nodiscard]] bool configure() noexcept override;
        [[nodiscard]] bool login() override;
        [[nodiscard]] int write(std::string_view) noexcept override;
    };

  public:
    struct PtyHandles
    {
        PtyMasterHandle master;
        PtySlaveHandle slave;
    };

    LinuxPty(PageSize const& _windowSize, std::optional<ImageSize> _pixels);
    LinuxPty(PtyHandles handles, PageSize size);
    ~LinuxPty() override;

    PtySlave& slave() noexcept override;

    [[nodiscard]] PtyMasterHandle handle() const noexcept;
    void close() override;
    [[nodiscard]] bool isClosed() const noexcept override;
    void wakeupReader() noexcept override;
    std::optional<std::string_view> read(size_t _size, std::chrono::milliseconds _timeout) override;
    [[nodiscard]] ReadResult read(crispy::BufferObject& storage,
                                  std::chrono::milliseconds timeout,
                                  size_t size) override;
    int write(char const* buf, size_t size) override;
    [[nodiscard]] PageSize pageSize() const noexcept override;
    void resizeScreen(PageSize _cells, std::optional<ImageSize> _pixels = std::nullopt) override;

    UnixPipe& stdoutFastPipe() noexcept { return _stdoutFastPipe; }

  private:
    std::optional<std::string_view> readSome(int fd, char* target, size_t n) noexcept;
    int waitForReadable(std::chrono::milliseconds timeout) noexcept;

    int _masterFd;
    int _epollFd;
    int _eventFd;
    UnixPipe _stdoutFastPipe;
    PageSize _pageSize;
    Slave _slave;
};

} // namespace terminal
