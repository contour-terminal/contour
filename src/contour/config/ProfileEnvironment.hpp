// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string_view>

namespace contour::config
{

/// Returns a diagnostic when a profile variable cannot be represented in a child environment.
[[nodiscard]] constexpr std::string_view profileEnvironmentProblem(std::string_view name,
                                                                   std::string_view value)
{
    if (name.find_first_not_of(" \t\r\n\v\f") == std::string_view::npos || name.contains('='))
        return "Variable names must be nonempty and cannot contain '='.";
    if (name.contains('\0') || value.contains('\0'))
        return "Variable names and values cannot contain null characters.";
    return {};
}

} // namespace contour::config
