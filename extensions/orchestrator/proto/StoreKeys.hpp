// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace orchestrator::proto
{

/// The file name a store key is kept under: every byte outside `[A-Za-z0-9_-]`, and a `.`, as `%XX`, so that
/// no key names a directory, a hidden file or a path.
/// @param key Such as `run/<id>`.
/// @return Such as `run%2F<id>`.
[[nodiscard]] std::string escapeKey(std::string_view key);

/// The key a file name stands for.
/// @param fileName A file name escapeKey() made.
/// @return The key; std::nullopt for a name escapeKey() cannot have made, such as a temporary file.
[[nodiscard]] std::optional<std::string> unescapeKey(std::string_view fileName);

} // namespace orchestrator::proto
