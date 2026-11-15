// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace orchestrator::json
{

/// The entries of a JSON array that was cut off midway, as an array of its own.
///
/// Only the syntax that decides where an entry ends is followed: strings, their escapes, and the nesting of
/// objects and arrays. Whether the result is valid JSON is left to the reader, which refuses it if it is not.
/// @param text A JSON array, such as a list command's output cut at the host's limit.
/// @return @p text up to the end of its last entry that a comma closes, with a `]` appended; the array alone
///         if it closes after all; std::nullopt if @p text is no array or no entry of it is closed.
[[nodiscard]] std::optional<std::string> closedArrayPrefix(std::string_view text);

} // namespace orchestrator::json
