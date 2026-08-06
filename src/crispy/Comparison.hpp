// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>

namespace crispy
{

enum class Comparison : uint8_t
{
    Less,
    Equal,
    Greater
};

template <typename T>
constexpr Comparison strongCompare(T const& a, T const& b)
{
    if (a < b)
        return Comparison::Less;
    else if (a == b)
        return Comparison::Equal;
    else
        return Comparison::Greater;
}

} // namespace crispy
