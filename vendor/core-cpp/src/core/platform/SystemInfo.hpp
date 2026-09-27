// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>

namespace core::platform
{

/// @brief Returns the local machine's hostname.
///
/// Encapsulates the per-platform system call (`GetComputerNameA` on Windows,
/// `gethostname` on POSIX) so business logic stays free of platform `#ifdef`s.
///
/// @return The hostname, or an empty string if it could not be determined.
[[nodiscard]] std::string hostName();

/// @brief Returns the local machine's hostname, resolved once per process.
///
/// hostName() issues a system call on every call. A caller that asks on every redraw of a status
/// line, or once per entry of a listing, wants this instead; a machine's hostname does not change
/// under a running process.
///
/// @return The hostname, or an empty string if it could not be determined.
[[nodiscard]] std::string const& cachedHostName();

} // namespace core::platform
