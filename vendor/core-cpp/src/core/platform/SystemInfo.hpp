// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>

namespace core::platform
{

/// @brief Returns the local machine's hostname, as the rest of the system spells it.
///
/// A caller compares it with the name another source gives the machine -- Qt's
/// `QHostInfo::localHostName()`, the host of a shell's OSC 7 `file://host/path` URL -- so it is the
/// name those use. Encapsulates the per-platform system call so business logic stays free of
/// platform `#ifdef`s:
///
/// - POSIX: `gethostname()`.
/// - Windows: the DNS host name, `GetComputerNameExW(ComputerNameDnsHostname)` in UTF-8, which is
///   what Qt asks for. Not the NetBIOS name (`GetComputerNameA`): that one is upper-cased and cut to
///   15 characters, so it matches neither source on a machine whose name is longer or has a
///   lower-case letter.
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
