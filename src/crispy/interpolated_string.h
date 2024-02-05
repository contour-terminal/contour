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
