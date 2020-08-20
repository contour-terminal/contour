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

#include <terminal/Size.h>

#include <map>
#include <string>
#include <variant>
#include <vector>
#include <cerrno>

#if defined(_MSC_VER)
#include <Windows.h>
#elif defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

namespace terminal {

Size currentWindowSize();

class PseudoTerminal {
public:
#if defined(_MSC_VER)
	using ssize_t = SSIZE_T;
	using PtyHandle = HPCON;
	using IOHandle = HANDLE;
#else
	using PtyHandle = int;
	using IOHandle = int;
#endif

	/**
	 * Constructs a pseudo terminal and sets its initial window size.
	 *
	 * @see Process.
	 */
	explicit PseudoTerminal(Size const& windowSize);
	virtual ~PseudoTerminal();

	/// Releases this PTY early.
	///
	/// This is automatically invoked when the destructor is called.
	void close();

	/// Reads from the terminal whatever has been written to from the other side of the terminal.
	///
	/// @param buf    Target buffer to store the received data to.
	/// @param size	  Capacity of parameter @p buf. At most @p size bytes will be stored into it.
	///
	/// @returns number of bytes stored in @p buf or -1 on error.
	auto read(char* buf, size_t size) -> ssize_t;

	/// Writes to the PTY device, so the other end can read from it.
	///
	/// @param buf    Buffer of data to be written.
	/// @param size   Number of bytes in @p buf to write.
	///
	/// @returns Number of bytes written or -1 on error.
	auto write(char const* buf, size_t size) -> ssize_t;

    /// @returns current underlying window size in characters width and height.
    Size screenSize() const noexcept;

    /// Resizes underlying window buffer by given character width and height.
    virtual void resizeScreen(Size const& _newSize);

	/// @returns The native master PTY handle.
	PtyHandle master() const noexcept { return master_; }

#if defined(__unix__) || defined(__APPLE__)
	/// @returns the native PTY handle of the slave side (not available on Windows).
	int slave() const noexcept { return slave_; }
#endif

private:
	PtyHandle master_;
    Size size_;

#if defined(__unix__) || defined(__APPLE__)
	PtyHandle slave_;
#else
	IOHandle input_;
	IOHandle output_;
#endif
};

}  // namespace terminal
