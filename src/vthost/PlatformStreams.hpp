// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The operating system's blocking byte streams: this process's stdio, a blocking AF_UNIX connect, and
/// a connected socket handle.

#include <core/platform/Types.hpp>

#include <expected>
#include <filesystem>
#include <memory>
#include <string>

#include <vthost/ByteStream.hpp>

namespace vthost
{

/// This process's stdin (read) and stdout (write), binary-safe: raw fds 0/1 on POSIX, the raw
/// GetStdHandle handles on Windows (never the CRT's text-mode streams).
///
/// The stream takes stdout for itself: the descriptor it writes is a private copy of stdout, and stdout
/// itself is pointed at stderr from then on. Every byte on stdout belongs to the protocol, so a stray
/// write to std::cout -- a log sink, a library's diagnostic -- reaches stderr instead of corrupting a
/// frame. Called once, before anything else is written.
/// @return The stream; its closeWrite() closes the real stdout.
[[nodiscard]] std::shared_ptr<IByteStream> makeProcessStdioStream();

/// Connects to the AF_UNIX stream socket at @p path, blocking until it is connected.
/// @return The connected socket as a stream, or why the connect failed.
[[nodiscard]] std::expected<std::shared_ptr<IByteStream>, std::string> connectUnixStream(
    std::filesystem::path const& path);

/// A connected stream socket as a blocking stream; closeWrite() shuts down its sending side.
/// @param handle The socket: a descriptor on POSIX, a `SOCKET` on Windows. Owned by the stream.
/// @return The stream.
[[nodiscard]] std::shared_ptr<IByteStream> makeSocketStream(core::platform::NativeHandle handle);

} // namespace vthost
