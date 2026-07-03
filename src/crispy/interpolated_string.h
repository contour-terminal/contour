// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <map>
#include <set>
#include <string_view>
#include <variant>
#include <vector>

namespace crispy
{

struct string_interpolation
{
    std::string_view name;
    std::set<std::string_view> flags;
    std::map<std::string_view, std::string_view> attributes;

    /// The exact original source slice this interpolation was parsed from, including the surrounding
    /// braces (e.g. "{Clock:Bold}"). A view into the parsed input (zero-copy), so it stays valid only as
    /// long as that input does. Lets a consumer that does not recognize @ref name emit the placeholder
    /// verbatim instead of dropping it, without lossily re-serializing the (order-normalized) flags and
    /// attributes. Empty for an interpolation built directly via parse_interpolation() (no brace context).
    std::string_view whole;

    bool operator==(string_interpolation const& rhs) const noexcept
    {
        return name == rhs.name && flags == rhs.flags && attributes == rhs.attributes;
    }

    bool operator!=(string_interpolation const& rhs) const noexcept { return !(*this == rhs); }
};

using interpolated_string_fragment = std::variant<string_interpolation, std::string_view>;
using interpolated_string = std::vector<interpolated_string_fragment>;

string_interpolation parse_interpolation(std::string_view text);
interpolated_string parse_interpolated_string(std::string_view text);

} // namespace crispy
