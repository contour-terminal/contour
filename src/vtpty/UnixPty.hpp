// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/Pty.hpp>
#include <vtpty/UnixUtils.hpp>

#include <crispy/BufferObject.hpp>
#include <crispy/FileDescriptor.hpp>
#include <crispy/ReadSelector.hpp>

#include <memory>
#include <mutex>
#include <optional>

#ifdef __APPLE__
    #include <util.h>
#elifdef __linux__
    #include <pty.h>
#endif

namespace vtpty
{

using crispy::FileDescriptor;

class UnixPty final: public Pty
{
  private:
    class Slave final: public PtySlave
    {
        FileDescriptor _slaveFd;

      public:
        explicit Slave(PtySlaveHandle fd): _slaveFd { FileDescriptor::fromNative(unbox<int>(fd)) } {}
        ~Slave() override;
        [[nodiscard]] PtySlaveHandle handle() const noexcept;
        void close() override;
        [[nodiscard]] bool isClosed() const noexcept override;
        bool configure() noexcept override;
        bool login() override;
        int write(std::string_view) noexcept override;
    };

  public:
    UnixPty(PageSize pageSize, std::optional<ImageSize> pixels);
    ~UnixPty() override;

    PtySlave& slave() noexcept override;

    [[nodiscard]] PtyMasterHandle handle() const noexcept;
    [[nodiscard]] StartResult start() override;
    void close() override;
    void waitForClosed() override;
    [[nodiscard]] bool isClosed() const noexcept override;
    void wakeupReader() noexcept override;
    [[nodiscard]] std::optional<ReadResult> read(crispy::BufferObject<char>& storage,
                                                 std::optional<std::chrono::milliseconds> timeout,
                                                 size_t size) override;
    int write(std::string_view data) override;
    [[nodiscard]] PageSize pageSize() const noexcept override;
    void resizeScreen(PageSize cells, std::optional<ImageSize> pixels = std::nullopt) override;

    UnixPipe& stdoutFastPipe() noexcept { return _stdoutFastPipe; }

  private:
    std::optional<std::string_view> readSome(int fd, char* target, size_t n) noexcept;

    [[nodiscard]] bool started() const noexcept { return _masterFd != -1; }

    FileDescriptor _masterFd;
    UnixPipe _stdoutFastPipe;
    crispy::ReadSelector _readSelector;
    PageSize _pageSize;
    std::optional<ImageSize> _pixels;
    std::unique_ptr<Slave> _slave;
    std::mutex _mutex;
};

} // namespace vtpty
