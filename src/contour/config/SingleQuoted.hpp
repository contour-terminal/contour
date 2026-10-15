// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <format>
#include <string>
#include <string_view>

namespace contour::config
{

/// A YAML single-quoted scalar: @ref text between quotes, each quote inside it doubled -- the one escape that
/// style knows. Formatted whole, so a template says `{}` where the scalar goes and cannot write the quotes
/// without the escaping.
struct SingleQuoted
{
    std::string_view text; ///< The value as it reads, unescaped.
};

} // namespace contour::config

template <>
struct std::formatter<contour::config::SingleQuoted>: std::formatter<std::string_view>
{
    auto format(contour::config::SingleQuoted value, auto& ctx) const
    {
        auto quoted = std::string {};
        quoted.reserve(value.text.size() + 2);
        quoted += '\'';
        for (auto const ch: value.text)
        {
            quoted += ch;
            if (ch == '\'')
                quoted += '\'';
        }
        quoted += '\'';
        return std::formatter<std::string_view>::format(quoted, ctx);
    }
};
