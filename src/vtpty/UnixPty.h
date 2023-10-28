// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/Pty.h>
#include <vtpty/UnixUtils.h>

#include <crispy/BufferObject.h>
#include <crispy/file_descriptor.h>
#include <crispy/read_selector.h>

#include <memory>
#include <optional>

#if defined(__APPLE__)
    #include <util.h>
#elif defined(__linux__)
    #include <pty.h>
#endif

namespace vtpty
{

using crispy::file_descriptor;

class UnixPty final: public Pty
{
  private:
    class Slave final: public PtySlave
    {
        file_descriptor _slaveFd;

      public:
        explicit Slave(PtySlaveHandle fd): _slaveFd { file_descriptor::from_native(unbox<int>(fd)) } {}
        ~Slave() override;
        [[nodiscard]] PtySlaveHandle handle() const noexcept;
        void close() override;
        [[nodiscard]] bool isClosed() const noexcept override;
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

    UnixPty(PageSize pageSize, std::optional<ImageSize> pixels);
    ~UnixPty() override;

    PtySlave& slave() noexcept override;

    [[nodiscard]] PtyMasterHandle handle() const noexcept;
    void start() override;
    void close() override;
    void waitForClosed() override;
    [[nodiscard]] bool isClosed() const noexcept override;
    void wakeupReader() noexcept override;
    [[nodiscard]] ReadResult read(crispy::buffer_object<char>& storage,
                                  std::optional<std::chrono::milliseconds> timeout,
                                  size_t size) override;
    int write(std::string_view data) override;
    [[nodiscard]] PageSize pageSize() const noexcept override;
    void resizeScreen(PageSize cells, std::optional<ImageSize> pixels = std::nullopt) override;

    UnixPipe& stdoutFastPipe() noexcept { return _stdoutFastPipe; }

  private:
    std::optional<std::string_view> readSome(int fd, char* target, size_t n) noexcept;

    [[nodiscard]] bool started() const noexcept { return _masterFd != -1; }

    file_descriptor _masterFd;
    UnixPipe _stdoutFastPipe;
    crispy::read_selector _readSelector;
    PageSize _pageSize;
    std::optional<ImageSize> _pixels;
    std::unique_ptr<Slave> _slave;
};

} // namespace vtpty
