// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <format>
#include <string_view>

namespace vtbackend
{

/// Which lines a buffer capture (XTCAPTURE, `CSI > Ps ; Ps , t`) counts and returns.
enum class CaptureLineMode : uint8_t
{
    Physical = 0, ///< Rows as drawn.
    Logical,      ///< Wrapped rows joined.
};

} // namespace vtbackend

template <>
// NOLINTNEXTLINE(readability-identifier-naming)
struct std::formatter<vtbackend::CaptureLineMode>: std::formatter<std::string_view>
{
    auto format(vtbackend::CaptureLineMode value, auto& ctx) const
    {
        auto output = std::string_view {};
        switch (value)
        {
            case vtbackend::CaptureLineMode::Physical: output = "physical"; break;
            case vtbackend::CaptureLineMode::Logical: output = "logical"; break;
        }
        return formatter<string_view>::format(output, ctx);
    }
};
