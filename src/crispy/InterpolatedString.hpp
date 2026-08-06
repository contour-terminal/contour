// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <map>
#include <set>
#include <string_view>
#include <variant>
#include <vector>

namespace crispy
{

struct StringInterpolation
{
    std::string_view name;
    std::set<std::string_view> flags;
    std::map<std::string_view, std::string_view> attributes;

    /// The exact original source slice this interpolation was parsed from, including the surrounding
    /// braces (e.g. "{Clock:Bold}"). A view into the parsed input (zero-copy), so it stays valid only as
    /// long as that input does. Lets a consumer that does not recognize @ref name emit the placeholder
    /// verbatim instead of dropping it, without lossily re-serializing the (order-normalized) flags and
    /// attributes. Empty for an interpolation built directly via parseInterpolation() (no brace context).
    std::string_view whole;

    bool operator==(StringInterpolation const& rhs) const noexcept
    {
        return name == rhs.name && flags == rhs.flags && attributes == rhs.attributes;
    }

    bool operator!=(StringInterpolation const& rhs) const noexcept { return !(*this == rhs); }
};

using InterpolatedStringFragment = std::variant<StringInterpolation, std::string_view>;
using InterpolatedString = std::vector<InterpolatedStringFragment>;

StringInterpolation parseInterpolation(std::string_view text);
InterpolatedString parseInterpolatedString(std::string_view text);

} // namespace crispy
