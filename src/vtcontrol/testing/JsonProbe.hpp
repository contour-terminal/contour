// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

#include <vtcontrol/EnumTable.hpp>

namespace vtcontrol::testing
{
enum class Probe2 : uint8_t
{
    Take = 0,
    Keep
};

struct Probe
{
    std::string name;
    uint32_t count = 7;
    std::optional<std::string> note = std::nullopt;
    Probe2 focus = Probe2::Take;
};
} // namespace vtcontrol::testing

template <>
struct vtcontrol::EnumTable<vtcontrol::testing::Probe2>
{
    static constexpr std::array Rows { EnumName<testing::Probe2> { testing::Probe2::Take, "take" },
                                       EnumName<testing::Probe2> { testing::Probe2::Keep, "keep" } };
};
