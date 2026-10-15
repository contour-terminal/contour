// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Claiming an AF_UNIX socket path on Windows, for `IocpListener::bindUnix`. Private to this
/// module's Windows sources; it was shared with the readiness `WindowsListener` until 0.5.0
/// removed that transport (core-cpp#6).

// winsock2.h MUST precede windows.h / ws2tcpip.h, and afunix.h needs what they declare.
// clang-format off
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#include <afunix.h>
// clang-format on

#include <core/net/NetError.hpp>

#include <expected>
#include <filesystem>
#include <string_view>

namespace core::net::detail
{

/// The socket file a unix socket path names, as the wide API names it.
/// @param path The socket file path, in UTF-8, as AF_UNIX reads `sun_path` on Windows.
/// @return The file; @c NetErrorCode::AddressError where @p path is not UTF-8, which names no file
///         -- a caller's path is text from outside, so that is a value, not an exception.
[[nodiscard]] std::expected<std::filesystem::path, NetError> unixSocketFile(std::string_view path);

/// Builds the AF_UNIX address for @p path and makes the path bindable: nothing is there, or a
/// stale socket file is, which is deleted. A live server keeps its path (@c NetErrorCode::AddressInUse),
/// and a file that is not a socket is never touched.
/// @param path The socket file path, in UTF-8, as AF_UNIX reads `sun_path` on Windows.
/// @param file The file @p path names, as unixSocketFile() answered it: probed and deleted by it.
/// @return The address to bind, or why the path cannot be claimed; @c NetErrorCode::AddressError
///         for a path too long for `sun_path`.
[[nodiscard]] std::expected<sockaddr_un, NetError> claimUnixSocketPath(std::string_view path,
                                                                       std::filesystem::path const& file);

} // namespace core::net::detail
