// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/ImageSize.hpp>
#include <vtpty/InheritedPty.hpp>
#include <vtpty/InputEcho.hpp>
#include <vtpty/PageSize.hpp>

#include <crispy/BufferObject.hpp>

#include <core/log/LogStore.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>

#include <boxed-cpp/boxed.hpp>

namespace vtpty
{

namespace detail
{
    // clang-format off
    struct PtyMasterHandle {};
    struct PtySlaveHandle {};
    // clang-format on
} // namespace detail

using PtyMasterHandle = boxed::boxed<std::uintptr_t, detail::PtyMasterHandle>;
using PtySlaveHandle = boxed::boxed<std::uintptr_t, detail::PtySlaveHandle>;
using PtyHandle = std::uintptr_t;

/// Line endings a device expects in pasted input.
enum class PasteLineEndings : std::uint8_t
{
    Preserve,       ///< Send line endings as supplied.
    CarriageReturn, ///< Send one CR per line break, like an Enter key.
};

class PtySlave
{
  public:
    virtual ~PtySlave() = default;
    virtual void close() = 0;
    [[nodiscard]] virtual bool isClosed() const noexcept = 0;
    [[nodiscard]] virtual bool configure() noexcept = 0;
    [[nodiscard]] virtual int write(std::string_view text) noexcept = 0;
};

class PtySlaveDummy: public PtySlave
{
  public:
    void close() override {}
    [[nodiscard]] bool isClosed() const noexcept override { return false; }
    bool configure() noexcept override { return true; }
    int write(std::string_view) noexcept override { return 0; }
};

/// Why a PTY, or the child process attached to it, failed to start.
enum class StartError : std::uint8_t
{
    PtyAllocationFailed, ///< The pseudo terminal itself could not be created.
    SpawnFailed,         ///< The child process could not be created.
    ConnectFailed,       ///< A remote session (SSH) could not be established.
};

/// A start() failure: the machine-readable reason plus the platform's own diagnostic text.
///
/// Failing to start a shell is an expected, recoverable outcome — a working directory that has since
/// been removed, a `shell:` that is not on PATH — not a programmer error, so it is reported rather
/// than thrown. It used to be thrown, and on Windows (where a CreateProcess() failure surfaces in the
/// parent, there being no fork()) that exception unwound out of a Qt event handler and left a
/// half-constructed display behind. See issue #1711.
struct StartFailure
{
    StartError error {};
    std::string detail; ///< The platform's own message, for the user to read.
};

/// What a successful start() has to report.
///
/// @c diagnostic is empty unless the start had to deviate from what was asked for — a working
/// directory that could not be used and was dropped, a fallback shell — in which case the caller
/// shows it to the user. It is not an error: the child IS running.
struct StartOutcome
{
    std::string diagnostic;
};

/// The result of starting a PTY and whatever is attached to it.
using StartResult = std::expected<StartOutcome, StartFailure>;

/// Where the output a pty last handed its reader came from.
enum class ReadOrigin : std::uint8_t
{
    Live = 0, ///< The program on the other end wrote it.
    Replay,   ///< A mirror refilled the pane with what its program wrote before: not news.
};

/// What the bytes a terminal writes to its device carry, as far as the terminal knows.
enum class InputKind : std::uint8_t
{
    Other = 0, ///< Nothing the user typed: replies, focus and mouse reports.
    Keyboard,  ///< The user's key events, paste or raw input sent by a user's action are among them.
};

class Pty
{
  public:
    struct ReadResult
    {
        std::string_view data {};
        bool fromStdoutFastPipe = false;
    };

    virtual ~Pty() = default;

    /// Starts the PTY instance.
    ///
    /// @return a possibly-empty diagnostic on success, or why the start failed.
    [[nodiscard]] virtual StartResult start() = 0;

    virtual PtySlave& slave() noexcept = 0;

    /// Releases this PTY early.
    ///
    /// This is automatically invoked when the destructor is called.
    virtual void close() = 0;

    /// Blocks until the underlying PTY is closed.
    virtual void waitForClosed() = 0;

    /// Returns true if the underlying PTY is closed, otherwise false.
    [[nodiscard]] virtual bool isClosed() const noexcept = 0;

    /// Reads from the terminal whatever has been written to from the other side
    /// of the terminal.
    ///
    /// @param storage Target buffer to store the read data to.
    /// @param timeout Wait only for up to given timeout before giving up the blocking read attempt.
    ///                A zero timeout never blocks: the read returns what is waiting, or reports that
    ///                nothing is (see below). Terminal::processInputOnce() relies on that to read
    ///                ahead while another thread holds the terminal.
    /// @param size   The number of bytes to read at most, even if the storage has more bytes available.
    ///
    /// @returns A view to the consumed buffer. The boolean in the ReadResult
    ///          indicates whether or not this data was coming through
    ///          the stdout-fastpipe.
    ///          An empty result means end of file and nothing else: callers close the PTY on it.
    ///          Nothing to read within the timeout, a zero one included, is std::nullopt with
    ///          `errno == EAGAIN`; a read that was interrupted (wakeupReader()) is std::nullopt with
    ///          `errno` being `EAGAIN` or `EINTR`, and callers treat the two alike. Any other
    ///          std::nullopt is a failure or a closed PTY, with `errno` saying which.
    [[nodiscard]] virtual std::optional<ReadResult> read(crispy::BufferObject<char>& storage,
                                                         std::optional<std::chrono::milliseconds> timeout,
                                                         size_t size) = 0;

    /// Inerrupts the read() operation on this PTY if a read() is currently in progress.
    ///
    /// If no read() is currently being in progress, then this call
    /// will have no effect.
    ///
    /// @notice This is typically implemented using non-blocking I/O.
    virtual void wakeupReader() = 0;

    /// Writes to the PTY device, so the other end can read from it.
    ///
    /// @param buf      Buffer of data to be written.
    ///
    /// @returns Number of bytes written or -1 on error. Less than asked, with `errno` EINTR, where
    ///          interruptWrites() had the write stop rather than wait for the other end to read.
    [[nodiscard]] virtual int write(std::string_view buf) = 0;

    /// write(), told what the bytes carry. A device with no use for that (every one with a real process on
    /// the other end) drops it; one that forwards input to a peer (vtpty::ChannelPty) passes it on.
    /// @param buf  Buffer of data to be written.
    /// @param kind What @p buf carries.
    /// @returns As write().
    [[nodiscard]] virtual int writeInput(std::string_view buf, InputKind kind)
    {
        std::ignore = kind;
        return write(buf);
    }

    /// Returns the line ending convention for paste payloads.
    [[nodiscard]] virtual PasteLineEndings pasteLineEndings() const noexcept
    {
        return PasteLineEndings::Preserve;
    }

    /// Has a write() in progress, and every later one until allowWrites(), return what it wrote so far
    /// rather than wait for the other end to read: a live upgrade stops the writer of a program that reads
    /// nothing this way. Any thread. A device whose writes never wait for the other end does nothing.
    virtual void interruptWrites() {}

    /// Ends interruptWrites(): writes wait for the other end to read again. Called with no write in
    /// progress.
    virtual void allowWrites() {}

    /// What of this device crosses to the process that takes over a live upgrade, while this one keeps it:
    /// for a capture that may yet roll back, which must not have to undo anything. Adopted again with
    /// adoptPty().
    /// @return On POSIX the master's descriptors, on Windows the pseudoconsole's handles, with the terminal's
    ///         size; or why it cannot cross: AdoptError::Unsupported for a device that cannot be handed over
    ///         at all, as this default says.
    [[nodiscard]] virtual std::expected<InheritedPty, AdoptError> handover() const
    {
        return std::unexpected(AdoptError::Unsupported);
    }

    /// Gives up what handover() names without closing any of it, and leaves this PTY closed: it then belongs
    /// to whoever adopts it (adoptPty()) -- in this process, or in the one it crosses to.
    /// @return What handover() returned; or why nothing was given up, this PTY then left as it was:
    ///         AdoptError::Unsupported for a device that cannot be handed over at all, as this default says.
    [[nodiscard]] virtual std::expected<InheritedPty, AdoptError> releaseHandover()
    {
        return std::unexpected(AdoptError::Unsupported);
    }

    /// What a program spawned onto this device is handed: its terminal, and the writer of the stdout fast
    /// pipe where there is one.
    struct SpawnDescriptors
    {
        int terminal;                            ///< The slave end, to be the program's terminal.
        std::optional<int> stdoutFastPipeWriter; ///< Where the program's fast output goes, if anywhere.
    };

    /// @return What a program spawned onto this device takes, while this process holds it; nothing for a
    ///         device no program is spawned onto by descriptor (anything but a Unix PTY), as this default
    ///         says.
    [[nodiscard]] virtual std::optional<SpawnDescriptors> spawnDescriptors() const noexcept
    {
        return std::nullopt;
    }

    /// Closes this process's copies of spawnDescriptors() once the program holds its own: until then the
    /// master would not see the program's side close.
    virtual void closeSpawnDescriptors() {}

    /// @returns current underlying window size in characters width and height.
    [[nodiscard]] virtual PageSize pageSize() const noexcept = 0;

    /// Resizes underlying window buffer by given character width and height.
    virtual void resizeScreen(PageSize cells, std::optional<ImageSize> pixels = std::nullopt) = 0;

    /// Where the output the last read() returned came from. Asked by the reader, after the read.
    /// @return Live, unless the device is fed by a mirror that marks its refills
    /// (vtpty::ChannelPty::feedReplay).
    [[nodiscard]] virtual ReadOrigin lastReadOrigin() const { return ReadOrigin::Live; }

    /// The name of the program leading the device's foreground process group.
    /// @return The name; none for a device with no local process to ask, as a mirrored pane's.
    [[nodiscard]] virtual std::optional<std::string> foregroundProcessName() const { return std::nullopt; }

    /// What the terminal does with typed text now. GUI-thread safe; a device that cannot tell answers
    /// Unknown.
    /// @return The echo state, as the program in the terminal last set it.
    [[nodiscard]] virtual InputEcho inputEcho() const noexcept { return InputEcho::Unknown; }

    /// Called when a device that learns of its echo state from elsewhere (a mirrored pane, told by its
    /// daemon) is told a different one. Runs on the thread that learned it, never under the terminal lock; it
    /// only posts to its owner, and must not call back into the device.
    using InputEchoObserver = std::function<void()>;

    /// Installs @p observer, replacing any earlier one; an empty one removes it. The owner removes its own
    /// before it goes: once this returns with an empty observer, none is running. A device that cannot be
    /// told of a change (a local PTY, which is only ever asked) never calls it.
    /// @param observer What to run on a change of inputEcho().
    virtual void setInputEchoObserver(InputEchoObserver const& /*observer*/) {}
};

[[nodiscard]] std::unique_ptr<Pty> createPty(PageSize pageSize, std::optional<ImageSize> viewSize);

auto inline const ptyLog = core::log::Category("pty", "Logs general PTY information.");
auto inline const ptyInLog = core::log::Category("pty.input", "Logs PTY raw input.");
auto inline const ptyOutLog = core::log::Category("pty.output", "Logs PTY raw output.");
auto inline const ptyPlacementLog =
    core::log::Category("pty.placement", "Logs moving session processes into their own systemd scopes.");

} // namespace vtpty

template <>
struct std::formatter<vtpty::StartError>: std::formatter<std::string_view>
{
    auto format(vtpty::StartError value, auto& ctx) const
    {
        // A switch rather than a table: switch exhaustiveness names this spot when an enumerator
        // is added, which a lookup table would silently mis-answer.
        auto const text = [value]() -> std::string_view {
            switch (value)
            {
                case vtpty::StartError::PtyAllocationFailed: return "PTY allocation failed";
                case vtpty::StartError::SpawnFailed: return "process creation failed";
                case vtpty::StartError::ConnectFailed: return "connection failed";
            }
            return "unknown error";
        }();
        return std::formatter<std::string_view>::format(text, ctx);
    }
};

template <>
struct std::formatter<vtpty::StartFailure>: std::formatter<std::string>
{
    auto format(vtpty::StartFailure const& value, auto& ctx) const
    {
        return std::formatter<std::string>::format(value.detail.empty()
                                                       ? std::format("{}", value.error)
                                                       : std::format("{}: {}", value.error, value.detail),
                                                   ctx);
    }
};
