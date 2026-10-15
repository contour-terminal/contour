// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/Pty.hpp>
#include <vtpty/UnixUtils.hpp>

#include <crispy/BufferObject.hpp>
#include <crispy/FileDescriptor.hpp>
#include <crispy/ReadSelector.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>

#ifdef __APPLE__
    #include <util.h>
#elifdef __linux__
    #include <pty.h>
#endif

#include <fcntl.h>

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

    /// Writes what the PTY takes of @p data now, without waiting, under _mutex.
    /// @return How much it took; -1, with errno set, where it took nothing.
    int writeAvailable(std::string_view data);

    /// Waits, with _mutex released, until the master descriptor takes a write again or the PTY closes.
    /// @return False once the PTY is closed.
    [[nodiscard]] bool awaitWritable();

    /// Whether the master descriptor is open, published by start() and close() for isClosed().
    enum class MasterState : uint8_t
    {
        Closed = 0, ///< Not started yet, or closed.
        Open,       ///< Started, and not closed.
    };

    FileDescriptor _masterFd;
    UnixPipe _stdoutFastPipe;
    /// Written once by close(), and never read: it wakes a write waiting for the application to read.
    UnixPipe _closeSignal { O_CLOEXEC | O_NONBLOCK };
    crispy::ReadSelector _readSelector;
    PageSize _pageSize;
    std::optional<ImageSize> _pixels;
    std::unique_ptr<Slave> _slave;
    /// Guards the master descriptor against a concurrent close(), for readers and writers alike. Never held
    /// across a wait for the application: close() must not wait for a program that reads nothing.
    mutable std::mutex _mutex;
    /// Admits one write at a time, from its first byte to its last, so that two never interleave.
    std::mutex _writeMutex;
    /// @see MasterState
    std::atomic<MasterState> _masterState { MasterState::Closed };
};

} // namespace vtpty
