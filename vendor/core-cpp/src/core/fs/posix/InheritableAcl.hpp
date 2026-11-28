// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// How a test plants, through the host, an ACL a new object inherits from its directory and that
/// grants others access: one command per OS, defined by a CMake-selected
/// `*/InheritableAcl_test.cpp` (`setfacl -d` on Linux, `chmod +a` on macOS, an NFSv4 `setfacl` on
/// FreeBSD). Test-only; never part of the module.

#include <filesystem>
#include <string>
#include <vector>

namespace core::fs::test
{

/// @param directory An existing directory, by host path.
/// @return The command, as its argument vector, that gives @p directory an inheritable ACL granting
///         others read and write.
[[nodiscard]] std::vector<std::string> inheritableAclCommand(std::filesystem::path const& directory);

} // namespace core::fs::test
