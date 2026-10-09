// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vtpty
{

/// Builds a child's environment as "NAME=VALUE" entries.
/// @param inherited The parent's entries ("NAME=VALUE").
/// @param overrides Variables to add, or to replace in @p inherited.
/// @param removed Names dropped from @p inherited; an override of the same name still applies.
/// @return The entries, in no particular order.
[[nodiscard]] std::vector<std::string> buildEnvironmentBlock(
    std::span<std::string_view const> inherited,
    std::map<std::string, std::string> const& overrides,
    std::span<std::string const> removed);

} // namespace vtpty
