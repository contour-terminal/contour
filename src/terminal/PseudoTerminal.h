/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019 Christian Parpart <christian@parpart.family>
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

#include <map>
#include <string>
#include <variant>
#include <vector>

#if defined(_MSC_VER)
#include <Windows.h>
#else
#include <pty.h>
#endif

namespace terminal {

struct [[nodiscard]] WindowSize {
    unsigned short columns;
	unsigned short rows;
};

WindowSize currentWindowSize();

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
	explicit PseudoTerminal(WindowSize const& windowSize);
	~PseudoTerminal();

	/// Releases this PTY early.
	///
	/// This is automatically invoked when the destructor is called.
	void release();

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

	/// @returns The native master PTY handle.
	PtyHandle master() const noexcept { return master_; }

	/// @returns the native input handle of the master side.
	IOHandle input() const noexcept {
#if defined(__unix__)
		return master_;
#else
		return input_;
#endif
	}

	/// @returns the native output handle of the master side.
	IOHandle output() const noexcept {
#if defined(__unix__)
		return master_;
#else
		return output_;
#endif
	}

#if defined(__unix__)
	/// @returns the native PTY handle of the slave side (not available on Windows).
	int slave() const noexcept { return slave_; }
#endif

private:
	PtyHandle master_;

#if defined(__unix__)
	PtyHandle slave_;
#else
	IOHandle input_;
	IOHandle output_;
#endif
};

}  // namespace terminal
