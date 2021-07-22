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

#include <chrono>
#include <optional>
#include <string_view>

namespace terminal {

class Pty {
  public:
    virtual ~Pty() = default;

    /// Releases this PTY early.
    ///
    /// This is automatically invoked when the destructor is called.
    virtual void close() = 0;

    /// Prepares PTY for use with a child process, therefore, closing the master end and
    /// doing some more prep work.
    ///
    /// Invoke this function after the respective child process has been fork()'ed.'
    virtual void prepareParentProcess() = 0;

    /// Prepares PTY for use inside a child process.
    ///
    /// That is, the current process must be already the child, i.e. via fork().
    virtual void prepareChildProcess() = 0;

    /// Reads from the terminal whatever has been written to from the other side of the terminal.
    ///
    /// @param _size   Capacity of parameter @p buf. At most @p size bytes will be stored into it.
    ///
    /// @returns view to the consumed buffer.
    virtual std::optional<std::string_view> read(size_t _size, std::chrono::milliseconds _timeout) = 0;

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
    virtual int write(char const* buf, size_t size) = 0;

    /// @returns current underlying window size in characters width and height.
    virtual PageSize screenSize() const noexcept = 0;

    /// Resizes underlying window buffer by given character width and height.
    virtual void resizeScreen(PageSize _cells, std::optional<ImageSize> _pixels = std::nullopt) = 0;
};

}  // namespace terminal
