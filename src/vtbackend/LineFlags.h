// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <crispy/flags.h>

#include <array>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace vtbackend
{

/// Flags associated with a terminal line.
enum class LineFlag : uint8_t
{
    None = 0x0000,
    Wrappable = 0x0001,
    Wrapped = 0x0002,
    Marked = 0x0004,
    OutputStart = 0x0008, ///< Command output begins (from OSC 133;C)
    DoubleWidth = 0x0010,
    DoubleHeightTop = 0x0020,
    DoubleHeightBottom = 0x0040,
    CommandEnd = 0x0080, ///< Command finished (from OSC 133;D)
};

using LineFlags = crispy::flags<LineFlag>;

/// Flags that describe the LOGICAL line, and therefore belong to its first physical line alone.
///
/// A logical line that reflow splits across several physical lines has exactly one head. The semantic
/// marks a shell emits (OSC 133, and Contour's own SETMARK) point at that head — at the line the prompt
/// starts on, at the line the output starts on — never at the chunks a wrap happens to produce. Copying
/// them onto the continuations would turn one prompt into several the moment the window is widened, which
/// is precisely what findMarkerUpwards() and the command-block scan would then walk into.
constexpr inline auto HeadOnlyLineFlags =
    LineFlags { LineFlag::Marked, LineFlag::OutputStart, LineFlag::CommandEnd };

} // namespace vtbackend

template <>
struct std::formatter<vtbackend::LineFlags>: formatter<std::string>
{
    auto format(const vtbackend::LineFlags flags, auto& ctx) const
    {
        static const std::array<std::pair<vtbackend::LineFlags, std::string_view>, 8> nameMap = {
            std::pair { vtbackend::LineFlag::Wrappable, std::string_view("Wrappable") },
            std::pair { vtbackend::LineFlag::Wrapped, std::string_view("Wrapped") },
            std::pair { vtbackend::LineFlag::Marked, std::string_view("Marked") },
            std::pair { vtbackend::LineFlag::OutputStart, std::string_view("OutputStart") },
            std::pair { vtbackend::LineFlag::DoubleWidth, std::string_view("DoubleWidth") },
            std::pair { vtbackend::LineFlag::DoubleHeightTop, std::string_view("DoubleHeightTop") },
            std::pair { vtbackend::LineFlag::DoubleHeightBottom, std::string_view("DoubleHeightBottom") },
            std::pair { vtbackend::LineFlag::CommandEnd, std::string_view("CommandEnd") },
        };
        std::string s;
        for (auto const& mapping: nameMap)
        {
            if ((mapping.first & flags) != vtbackend::LineFlag::None)
            {
                if (!s.empty())
                    s += ",";
                s += mapping.second;
            }
        }
        return formatter<std::string>::format(s, ctx);
    }
};
