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

namespace terminal
{

struct UnixPipe
{
    int pfd[2];

    UnixPipe();
    UnixPipe(UnixPipe&&) noexcept;
    UnixPipe& operator=(UnixPipe&&) noexcept;
    UnixPipe(UnixPipe const&) = delete;
    UnixPipe& operator=(UnixPipe const&) = delete;
    ~UnixPipe();

    [[nodiscard]] bool good() const noexcept { return pfd[0] != -1 && pfd[1] != -1; }

    [[nodiscard]] int reader() noexcept { return pfd[0]; }
    [[nodiscard]] int writer() noexcept { return pfd[1]; }

    void closeReader() noexcept;
    void closeWriter() noexcept;

    void close();
};

class UnixPty: public Pty
{
  private:
    class Slave: public PtySlave
    {
      public:
        int _slaveFd;
        explicit Slave(PtySlaveHandle fd): _slaveFd { unbox<int>(fd) } {}
        ~Slave() override;
        PtySlaveHandle handle() const noexcept;
        void close() override;
        bool isClosed() const noexcept override;
        bool configure() noexcept override;
        bool login() override;
        int write(std::string_view) noexcept override;
    };

  public:
    struct PtyHandles
    {
        PtyMasterHandle master;
        PtySlaveHandle slave;
    };

    UnixPty(PageSize const& _windowSize, std::optional<ImageSize> _pixels);
    UnixPty(PtyHandles handles, PageSize size);
    ~UnixPty() override;

    PtySlave& slave() noexcept override;

    PtyMasterHandle handle() const noexcept;
    void close() override;
    bool isClosed() const noexcept override;
    void wakeupReader() noexcept override;
    std::optional<std::string_view> read(size_t _size, std::chrono::milliseconds _timeout) override;
    [[nodiscard]] ReadResult read(crispy::BufferObject& storage,
                                  std::chrono::milliseconds timeout,
                                  size_t size) override;
    int write(char const* buf, size_t size) override;
    PageSize pageSize() const noexcept override;
    void resizeScreen(PageSize _cells, std::optional<ImageSize> _pixels = std::nullopt) override;

    UnixPipe& stdoutFastPipe() noexcept { return _stdoutFastPipe; }

  private:
    std::optional<std::string_view> readSome(int fd, char* target, size_t n) noexcept;

    int _masterFd;
    std::array<int, 2> _pipe;
    UnixPipe _stdoutFastPipe;
    std::vector<char> _buffer;
    PageSize _pageSize;
    Slave _slave;
};

} // namespace terminal
