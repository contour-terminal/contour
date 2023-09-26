// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/PageSize.h>

#include <crispy/BufferObject.h>
#include <crispy/ImageSize.h>
#include <crispy/logstore.h>

#include <chrono>
#include <optional>
#include <string_view>

#include <boxed-cpp/boxed.hpp>

namespace terminal
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

class Pty
{
  public:
    using ReadResult = std::optional<std::tuple<std::string_view, bool>>;

    virtual ~Pty() = default;

    /// Starts the PTY instance.
    virtual void start() = 0;

    virtual PtySlave& slave() noexcept = 0;

    /// Releases this PTY early.
    ///
    /// This is automatically invoked when the destructor is called.
    virtual void close() = 0;

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
    [[nodiscard]] virtual ReadResult read(crispy::buffer_object<char>& storage,
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
    virtual void resizeScreen(PageSize cells, std::optional<crispy::image_size> pixels = std::nullopt) = 0;
};

[[nodiscard]] std::unique_ptr<Pty> createPty(PageSize pageSize, std::optional<crispy::image_size> viewSize);

auto const inline ptyLog = logstore::category("pty", "Logs general PTY informations.");
auto const inline ptyInLog = logstore::category("pty.input", "Logs PTY raw input.");
auto const inline ptyOutLog = logstore::category("pty.output", "Logs PTY raw output.");

} // namespace terminal
