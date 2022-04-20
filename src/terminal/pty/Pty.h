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

#include <terminal/primitives.h>

#include <crispy/BufferObject.h>
#include <crispy/boxed.h>
#include <crispy/logstore.h>

#include <chrono>
#include <optional>
#include <string_view>

namespace terminal
{

namespace detail
{
    // clang-format off
    struct PtyMasterHandle {};
    struct PtySlaveHandle {};
    // clang-format on
} // namespace detail

using PtyMasterHandle = crispy::boxed<std::uintptr_t, detail::PtyMasterHandle>;
using PtySlaveHandle = crispy::boxed<std::uintptr_t, detail::PtySlaveHandle>;
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

    virtual PtySlave& slave() noexcept = 0;

    /// Releases this PTY early.
    ///
    /// This is automatically invoked when the destructor is called.
    virtual void close() = 0;

    /// Returns true if the underlying PTY is closed, otherwise false.
    [[nodiscard]] virtual bool isClosed() const noexcept = 0;

    /// Reads from the terminal whatever has been written to from the other side of the terminal.
    ///
    /// @param _size   Capacity of parameter @p buf. At most @p size bytes will be stored into it.
    ///
    /// @returns view to the consumed buffer.
    [[nodiscard]] virtual std::optional<std::string_view> read(size_t _size,
                                                               std::chrono::milliseconds _timeout) = 0;

    [[nodiscard]] virtual ReadResult read(crispy::BufferObject& storage,
                                          std::chrono::milliseconds timeout,
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
    /// @param buf    Buffer of data to be written.
    /// @param size   Number of bytes in @p buf to write.
    ///
    /// @returns Number of bytes written or -1 on error.
    [[nodiscard]] virtual int write(char const* buf, size_t size) = 0;

    /// @returns current underlying window size in characters width and height.
    [[nodiscard]] virtual PageSize pageSize() const noexcept = 0;

    /// Resizes underlying window buffer by given character width and height.
    virtual void resizeScreen(PageSize _cells, std::optional<ImageSize> _pixels = std::nullopt) = 0;
};

[[nodiscard]] std::unique_ptr<Pty> createPty(PageSize pageSize, std::optional<ImageSize> viewSize);

auto const inline PtyLog = logstore::Category("pty", "Logs general PTY informations.");
auto const inline PtyInLog = logstore::Category("pty.input", "Logs PTY raw input.");
auto const inline PtyOutLog = logstore::Category("pty.output", "Logs PTY raw output.");

} // namespace terminal
