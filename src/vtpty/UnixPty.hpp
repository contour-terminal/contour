// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/InheritedPty.hpp>
#include <vtpty/Pty.hpp>
#include <vtpty/UnixUtils.hpp>

#include <crispy/BufferObject.hpp>
#include <crispy/FileDescriptor.hpp>
#include <crispy/ReadSelector.hpp>

#include <atomic>
#include <cstdint>
#include <expected>
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
        Slave() = default; ///< A slave that is closed already: what an adopted PTY has.
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

    /// @return The slave's descriptor, for a spawn to hand to the child; -1 once closed. Only after start().
    [[nodiscard]] PtySlaveHandle slaveHandle() const noexcept;
    [[nodiscard]] StartResult start() override;
    void close() override;
    void waitForClosed() override;
    [[nodiscard]] bool isClosed() const noexcept override;
    void wakeupReader() noexcept override;
    [[nodiscard]] std::optional<ReadResult> read(crispy::BufferObject<char>& storage,
                                                 std::optional<std::chrono::milliseconds> timeout,
                                                 size_t size) override;
    int write(std::string_view data) override;
    void interruptWrites() override;
    void allowWrites() override;
    [[nodiscard]] PageSize pageSize() const noexcept override;
    void resizeScreen(PageSize cells, std::optional<ImageSize> pixels = std::nullopt) override;

    /// Reads the terminal's local flags from the master, under _mutex, so a concurrent close() cannot hand it
    /// a recycled descriptor. Called on the GUI thread.
    /// @return Unknown before start(), after close(), or when tcgetattr() fails; else inputEchoOf() the
    /// flags.
    [[nodiscard]] InputEcho inputEcho() const noexcept override;

    UnixPipe& stdoutFastPipe() noexcept { return _stdoutFastPipe; }

    /// @name Live-upgrade hand-over (exec of the daemon keeps descriptors, not objects)
    /// @{

    /// @return The descriptors that must cross an exec, with the terminal size; AdoptError::BadDescriptor
    ///         where there are none: before start(), or once closed.
    [[nodiscard]] std::expected<InheritedPty, AdoptError> handover() const override;

    /// Gives up the descriptors without closing them and leaves this PTY closed, for an adopter in the
    /// same process (the descriptors then belong to the returned value's adopter).
    /// @return What handover() returned; AdoptError::BadDescriptor, nothing given up, where there are none.
    [[nodiscard]] std::expected<InheritedPty, AdoptError> releaseHandover() override;

    /// Takes over the PTY a previous image left behind: no openpty(), no slave. The descriptors are made
    /// close-on-exec again and the master non-blocking.
    /// @param inherited The descriptors and size from handover().
    /// @return The running PTY; or AdoptError::BadDescriptor where a descriptor is not open or the master
    ///         is no terminal. On error no descriptor is closed.
    [[nodiscard]] static std::expected<std::unique_ptr<UnixPty>, AdoptError> adopt(InheritedPty inherited);

    /// Asks whether adopt() would take @p inherited, without taking anything.
    /// @param inherited The descriptors and size from handover().
    /// @return Nothing; or AdoptError::BadDescriptor where the master or the fast pipe's reader is not open,
    ///         or the master is no terminal.
    [[nodiscard]] static std::expected<void, AdoptError> canAdopt(InheritedPty const& inherited);
    /// @}

  private:
    std::optional<std::string_view> readSome(int fd, char* target, size_t n) noexcept;

    [[nodiscard]] bool started() const noexcept { return _masterFd != -1; }

    /// Writes what the PTY takes of @p data now, without waiting, under _mutex.
    /// @return How much it took; -1, with errno set, where it took nothing.
    int writeAvailable(std::string_view data);

    /// What a write waiting for the application to read was woken by.
    enum class WriteWait : uint8_t
    {
        Writable = 0, ///< The master descriptor takes a write again.
        Closed,       ///< The PTY closed, or the wait failed.
        Interrupted,  ///< interruptWrites().
    };

    /// Waits, with _mutex released, until the master descriptor takes a write again, the PTY closes, or
    /// writes are interrupted.
    /// @return What ended the wait.
    [[nodiscard]] WriteWait awaitWritable();

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
    /// Readable from interruptWrites() until allowWrites() drains it: it wakes a write waiting for the
    /// application to read, and stops every later one from waiting.
    UnixPipe _interruptSignal { O_CLOEXEC | O_NONBLOCK };
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

    /// Whether a write may wait for the application to read.
    enum class WriteInterruption : uint8_t
    {
        Allowed = 0, ///< It may: the default, and after allowWrites().
        Interrupted, ///< It may not: after interruptWrites(), and _interruptSignal is readable.
    };
    /// Raised by interruptWrites(), lowered by allowWrites(): keeps _interruptSignal at one byte at most.
    std::atomic<WriteInterruption> _writeInterruption { WriteInterruption::Allowed };
};

} // namespace vtpty
