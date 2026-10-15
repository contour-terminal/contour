// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/Pty.hpp>

#include <crispy/BufferObject.hpp>

#include <memory>
#include <mutex>

#include <Windows.h>

namespace vtpty
{

/// ConPty implementation for newer Windows 10 versions.
class ConPty final: public Pty
{
  public:
    explicit ConPty(PageSize const& windowSize);
    ~ConPty() override;

    ConPty(ConPty const&) = delete;
    ConPty& operator=(ConPty const&) = delete;
    ConPty(ConPty&&) = delete;
    ConPty& operator=(ConPty&&) = delete;

    [[nodiscard]] StartResult start() override;
    void close() override;
    void waitForClosed() override;
    [[nodiscard]] bool isClosed() const noexcept override;

    /// Reads what the pseudoconsole wrote, waiting for it at most @p timeout, and no longer once
    /// wakeupReader() or close() is called.
    /// @return What was read; std::nullopt with errno EAGAIN when @p timeout passed first, EINTR when
    ///         woken, and EPIPE once the pseudoconsole's output has ended or close() has begun.
    [[nodiscard]] std::optional<ReadResult> read(crispy::BufferObject<char>& storage,
                                                 std::optional<std::chrono::milliseconds> timeout,
                                                 size_t size) override;
    void wakeupReader() override;
    int write(std::string_view data) override;
    [[nodiscard]] PageSize pageSize() const noexcept override;
    void resizeScreen(PageSize cells, std::optional<ImageSize> pixels = std::nullopt) override;

    PtySlave& slave() noexcept override;
    [[nodiscard]] HPCON master() const noexcept { return _master; }

  private:
    // Private class for ConPTY API implementation details
    class ConptyApiImpl;

    std::mutex _mutex; // used to guard close()
    /// Held by read() for as long as it uses _input, so that close() closes that only once no read does.
    std::mutex _readMutex;
    /// Held by write() for as long as it uses _output, so that close() closes that only once no write does.
    std::mutex _writeMutex;
    PageSize _size;
    HPCON _master = INVALID_HANDLE_VALUE;
    /// Our end of the pipe the pseudoconsole writes to, open for overlapped reads.
    HANDLE _input = INVALID_HANDLE_VALUE;
    HANDLE _output = INVALID_HANDLE_VALUE;
    /// Signalled when an overlapped read on _input completes.
    HANDLE _readCompleted;
    /// Signalled by wakeupReader(), to end a read's wait early.
    HANDLE _wakeup;
    /// Set by close() as it begins, and left set: it ends the read waiting then, and no read starts after it.
    /// A wakeup would be consumed by the read it ends, and a reader that reads again at once would wait anew.
    HANDLE _closing;
    /// Set by close(), and left set: what waitForClosed() waits on.
    HANDLE _closed;
    /// What slave() answers, from construction on: a slave end this process cannot write to.
    std::unique_ptr<PtySlave> _slave;
    std::unique_ptr<ConptyApiImpl> _conptyApi; // ConPTY API implementation (dynamically loaded)
};

} // namespace vtpty
