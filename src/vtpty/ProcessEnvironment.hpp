// SPDX-License-Identifier: Apache-2.0
//
// This process's environment, as the commands and panes it starts inherit it.
#pragma once

#include <string>
#include <vector>

namespace vtpty
{

/// @return This process's environment, each variable as `NAME=value` in UTF-8, in the order the platform
///         keeps it. On Windows the hidden per-drive directories (`=C:=C:\…`) are left out: their names begin
///         with `=`, so they are no variables a child could be meant to inherit by name.
[[nodiscard]] std::vector<std::string> processEnvironmentEntries();

} // namespace vtpty
