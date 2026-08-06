// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/ImageSize.hpp>
#include <vtpty/PageSize.hpp>

#include <crispy/BufferObject.hpp>
#include <crispy/logstore.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>

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

class PtySlave
{
  public:
    virtual ~PtySlave() = default;
    virtual void close() = 0;
    [[nodiscard]] virtual bool isClosed() const noexcept = 0;
    [[nodiscard]] virtual bool configure() noexcept = 0;
    [[nodiscard]] virtual bool login() = 0;
    [[nodiscard]] virtual int write(std::string_view text) noexcept = 0;
};

class PtySlaveDummy: public PtySlave
{
  public:
    void close() override {}
    [[nodiscard]] bool isClosed() const noexcept override { return false; }
    bool configure() noexcept override { return true; }
    bool login() override { return true; }
    int write(std::string_view) noexcept override { return 0; }
};

/// Why a PTY, or the child process attached to it, failed to start.
enum class StartError : std::uint8_t
{
    PtyAllocationFailed, ///< The pseudo terminal itself could not be created.
    SpawnFailed,         ///< The child process could not be created.
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
    /// @param size    The number of bytes to read at most, even if the storage has more bytes available.
    ///
    /// @returns A view to the consumed buffer. The boolean in the ReadResult
    ///          indicates whether or not this data was coming through
    ///          the stdout-fastpipe.
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
    /// @returns Number of bytes written or -1 on error.
    [[nodiscard]] virtual int write(std::string_view buf) = 0;

    /// @returns current underlying window size in characters width and height.
    [[nodiscard]] virtual PageSize pageSize() const noexcept = 0;

    /// Resizes underlying window buffer by given character width and height.
    virtual void resizeScreen(PageSize cells, std::optional<ImageSize> pixels = std::nullopt) = 0;
};

[[nodiscard]] std::unique_ptr<Pty> createPty(PageSize pageSize, std::optional<ImageSize> viewSize);

auto inline const ptyLog = logstore::Category("pty", "Logs general PTY information.");
auto inline const ptyInLog = logstore::Category("pty.input", "Logs PTY raw input.");
auto inline const ptyOutLog = logstore::Category("pty.output", "Logs PTY raw output.");

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
