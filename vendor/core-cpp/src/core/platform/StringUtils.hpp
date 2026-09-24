// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>

namespace core::platform
{

/// @brief Trims leading and trailing whitespace from @p str in place.
///
/// Strips ' ', '\t', '\n', '\r'. If @p str is whitespace-only, it is cleared.
inline void trimInPlace(std::string& str)
{
    if (auto const start = str.find_first_not_of(" \t\n\r"); start != std::string::npos)
        str = str.substr(start, str.find_last_not_of(" \t\n\r") - start + 1);
    else
        str.clear();
}

} // namespace core::platform
