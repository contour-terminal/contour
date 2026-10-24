// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/platform/FileSystem.hpp>

#include <optional>
#include <string>

namespace core::platform
{

/// @brief Reads the machine id, the 128-bit identifier systemd keeps in `/etc/machine-id`.
///
/// Two programs that report the same machine id run on the same machine, which is how a terminal
/// decides whether a working directory a shell announces names a path on its own host.
///
/// The file holds 32 hexadecimal digits and a newline. Surrounding whitespace is ignored, and the
/// digits are returned in lowercase.
///
/// @param fileSystem The file system to read `/etc/machine-id` from.
/// @return The 32-digit lowercase id, or `std::nullopt` if the file is missing or unreadable, holds
///         `uninitialized` (a machine that has not finished its first boot), or holds anything but
///         exactly 32 hexadecimal digits.
[[nodiscard]] std::optional<std::string> readMachineId(FileSystem const& fileSystem);

} // namespace core::platform
