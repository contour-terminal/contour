// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/InheritedPty.hpp>
#include <vtpty/Pty.hpp>

#include <crispy/BufferObject.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>

#include <Windows.h>

namespace vtpty
{

/// Which implementation of the pseudoconsole API a ConPty runs on.
enum class ConsoleHost : uint8_t
{
    /// Windows' own, in kernel32: what runs when no conpty.dll is found.
    System = 0,
    /// The conpty.dll shipped beside the executable, with its OpenConsole.exe: the one whose layout behind
    /// an HPCON is known, so the only one a live upgrade can hand to another process (ConPty::adopt()).
    Bundled,
    /// A conpty.dll found on the PATH, of whatever version that is.
    OnPath,
};

/// ConPty implementation for newer Windows 10 versions.
class ConPty final: public Pty
{
  public:
    explicit ConPty(PageSize const& windowSize);

    /// Takes over a pseudoconsole another process left behind, whose handles this process inherited: the
    /// successor's side of a Windows daemon's live upgrade. The pseudoconsole is started already, and
    /// serves on as though this process had created it; closing it ends it as for one it created.
    /// @param inherited Its handles (InheritedPty::conpty), from handoverHandles() in the process that hands
    ///        it over, and its size at hand-off.
    /// @return It; or AdoptError::ForeignConsoleHost where this process cannot load the bundled conpty.dll,
    ///         BadDescriptor where it names no handles, or one is not open in this process.
    [[nodiscard]] static std::expected<std::unique_ptr<ConPty>, AdoptError> adopt(
        InheritedPty const& inherited);

    ~ConPty() override;

    ConPty(ConPty const&) = delete;
    ConPty& operator=(ConPty const&) = delete;
    ConPty(ConPty&&) = delete;
    ConPty& operator=(ConPty&&) = delete;

    [[nodiscard]] StartResult start() override;
    /// Closes the pseudoconsole, which ends the program on it -- unless writes are interrupted
    /// (interruptWrites()): a session quiesced for a live upgrade, whose capture names these handles. The
    /// close is then held until allowWrites(), and dropped by releaseHandles(); the destructor closes
    /// regardless.
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
    /// Writes @p data, waiting while the pseudoconsole reads nothing.
    /// @return What it wrote; less than asked, with errno EINTR, where interruptWrites() ended the wait; -1
    ///         with errno EPIPE once the pseudoconsole is gone.
    int write(std::string_view data) override;
    void interruptWrites() override;
    void allowWrites() override;
    [[nodiscard]] PageSize pageSize() const noexcept override;
    void resizeScreen(PageSize cells, std::optional<ImageSize> pixels = std::nullopt) override;

    PtySlave& slave() noexcept override;
    [[nodiscard]] HPCON master() const noexcept { return _master; }

    /// @return The implementation of the pseudoconsole API this one runs on.
    [[nodiscard]] ConsoleHost consoleHost() const noexcept;

    /// @return The terminal's size in pixels as last given to resizeScreen(), or at adoption; nullopt where
    ///         none was. The pseudoconsole takes none: it is kept for a live upgrade to carry.
    [[nodiscard]] std::optional<ImageSize> pixelSize() const noexcept;

    /// Asks whether adopt() would take @p handles, without taking anything: what a successor checks before it
    /// says it can take over.
    /// @param handles The pseudoconsole's handles, from handoverHandles() in the process that hands it over.
    /// @return Nothing; or AdoptError::ForeignConsoleHost where this process cannot load the bundled
    ///         conpty.dll, BadDescriptor where a handle is not open in this process.
    [[nodiscard]] static std::expected<void, AdoptError> canAdopt(InheritedConPty const& handles);

    /// The handles that make up the pseudoconsole, as a successor process inherits them, while this one
    /// keeps them: for a capture that may yet roll back, which must not have to undo anything.
    /// @return They; or AdoptError::ForeignConsoleHost where it does not run on the bundled conpty.dll
    ///         (ConsoleHost::Bundled), BadDescriptor where there is no live pseudoconsole.
    [[nodiscard]] std::expected<InheritedConPty, AdoptError> handoverHandles() const;

    /// Gives up the pseudoconsole without closing it or any of its handles, and leaves this ConPty closed:
    /// the handles then belong to whoever adopts them (adopt()) -- in this process, or in one that inherited
    /// them, once this process closed its own. Only on the bundled conpty.dll (Require), whose layout alone
    /// is known.
    /// @return What handoverHandles() returned.
    [[nodiscard]] std::expected<InheritedConPty, AdoptError> releaseHandles();

  private:
    // Private class for ConPTY API implementation details
    class ConptyApiImpl;

    /// handoverHandles(), under _mutex.
    [[nodiscard]] std::expected<InheritedConPty, AdoptError> handoverHandlesLocked() const;
    /// Closes the pseudoconsole now, under _mutex: close() without its deferral.
    void closeLocked();
    /// interruptWrites(), under _mutex.
    void interruptWritesLocked();

    /// Guards close() and everything that reads the pseudoconsole's handles against it.
    mutable std::mutex _mutex;
    /// Held by read() for as long as it uses _input, so that close() closes that only once no read does.
    std::mutex _readMutex;
    /// Held by write() for as long as it uses _output, so that close() closes that only once no write does.
    std::mutex _writeMutex;
    PageSize _size;
    /// The terminal's size in pixels, which the pseudoconsole is not told (@see pixelSize()).
    std::optional<ImageSize> _pixels;
    HPCON _master = INVALID_HANDLE_VALUE;
    /// Our end of the pipe the pseudoconsole writes to, open for overlapped reads.
    HANDLE _input = INVALID_HANDLE_VALUE;
    /// Our end of the pipe the pseudoconsole reads from, open for overlapped writes, which alone can be given
    /// up on while the pseudoconsole reads nothing (interruptWrites()).
    HANDLE _output = INVALID_HANDLE_VALUE;
    /// Signalled when an overlapped read on _input completes.
    HANDLE _readCompleted;
    /// Signalled when an overlapped write on _output completes.
    HANDLE _writeCompleted;
    /// Set by interruptWrites(), reset by allowWrites(): a level, which ends every write's wait while it is
    /// set.
    HANDLE _writesInterrupted;
    /// Whether writes may wait for the pseudoconsole to read.
    enum class WriteInterruption : uint8_t
    {
        Allowed = 0, ///< They may: the default, and after allowWrites().
        Interrupted, ///< They may not: after interruptWrites(), with _writesInterrupted set.
    };
    /// _writesInterrupted's level, asked without a call into the kernel by every write before it starts.
    std::atomic<WriteInterruption> _writeInterruption { WriteInterruption::Allowed };
    /// Whether a close() waits for writes to be allowed again (@see close()).
    enum class CloseDeferral : uint8_t
    {
        None = 0, ///< No close was asked for while writes were interrupted.
        Pending,  ///< One was: allowWrites() closes.
    };
    /// Under _mutex.
    CloseDeferral _closeDeferral = CloseDeferral::None;
    /// Bytes the pipe to the pseudoconsole is known to take without waiting: what it last said it had room
    /// for, less what was written since. Under _writeMutex.
    std::size_t _knownRoom = 0;
    /// Signalled by wakeupReader(), to end a read's wait early.
    HANDLE _wakeup;
    /// Set by close() as it begins, and left set: it ends the read waiting then, and no read starts after it.
    /// A wakeup would be consumed by the read it ends, and a reader that reads again at once would wait anew.
    HANDLE _closing;
    /// Set by close(), and left set: what waitForClosed() waits on.
    HANDLE _closed;
    /// What slave() answers, from construction on: a slave end this process cannot write to.
    std::unique_ptr<PtySlave> _slave;
    /// The pseudoconsole API, this process's one (dynamically loaded, once).
    ConptyApiImpl const* _conptyApi;
};

} // namespace vtpty
