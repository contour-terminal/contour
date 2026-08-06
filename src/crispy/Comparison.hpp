// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>

namespace crispy
{

enum class comparison : uint8_t
{
    Less,
    Equal,
    Greater
};

template <typename T>
constexpr comparison strongCompare(T const& a, T const& b)
{
    if (a < b)
        return comparison::Less;
    else if (a == b)
        return comparison::Equal;
    else
        return comparison::Greater;
}

} // namespace crispy
