// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string_view>

namespace core::platform
{

/// Checks whether a path string contains shell glob metacharacters (*, ?, [).
[[nodiscard]] constexpr bool containsGlobChars(std::string_view path) noexcept
{
    return path.find_first_of("*?[") != std::string_view::npos;
}

/// Matches a filename against a shell glob pattern (supports *, ?, and [...] bracket expressions).
[[nodiscard]] bool globMatchFilename(std::string_view filename, std::string_view pattern);

} // namespace core::platform
