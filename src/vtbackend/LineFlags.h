// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <crispy/flags.h>

#include <array>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

/// The single source for every line flag: one row each, naming the flag and giving its bit.
///
/// The `LineFlag` enumeration, the enumerable `LineFlagList`, and the `std::formatter` below are all
/// generated from this table, so **adding a flag is adding one row here** and cannot leave any of the
/// three behind. This mirrors VTBACKEND_CELL_FLAGS in CellFlags.h, which was converted for exactly this
/// reason after a hand-maintained copy of the flags rotted the day a flag was added.
#define VTBACKEND_LINE_FLAGS(_)                                                                    \
    /* Structural: whether this line may reflow, and whether it continues the one above. */        \
    _(Wrappable, 0)                                                                                \
    _(Wrapped, 1)                                                                                  \
    /* Semantic marks. A prompt starts here (OSC 133;A, or the deprecated SETMARK). */             \
    _(Marked, 2)                                                                                   \
    /* Command output begins (OSC 133;C). */                                                       \
    _(OutputStart, 3)                                                                              \
    /* Double-width / double-height renditions (DECDWL, DECDHL). */                                \
    _(DoubleWidth, 4)                                                                              \
    _(DoubleHeightTop, 5)                                                                          \
    _(DoubleHeightBottom, 6)                                                                       \
    /* Command finished (OSC 133;D); the column it stopped at is Line::commandEndOffset(). */      \
    _(CommandEnd, 7)                                                                               \
    /* The shell's prompt finished printing and user input begins (OSC 133;B); the column it ended \
       at is Line::promptEndOffset(). */                                                           \
    _(PromptEnd, 8)

namespace vtbackend
{

/// Flags associated with a terminal line. @see VTBACKEND_LINE_FLAGS for the table these enumerators are
/// generated from, and for what each one means.
enum class LineFlag : uint16_t
{
    None = 0,

#define VTBACKEND_LINE_FLAG_ENUMERATOR(Name, Bit) Name = (1U << (Bit)),
    VTBACKEND_LINE_FLAGS(VTBACKEND_LINE_FLAG_ENUMERATOR)
#undef VTBACKEND_LINE_FLAG_ENUMERATOR
};

using LineFlags = crispy::flags<LineFlag>;

/// Every `LineFlag`, in declaration order, excluding `None`.
///
/// The formatter below *names* one flag; this *enumerates* them, which naming cannot do. Any code that
/// must visit every flag reads this rather than writing the list out again.
inline constexpr auto LineFlagList = std::array {
#define VTBACKEND_LINE_FLAG_ROW(Name, Bit) LineFlag::Name,
    VTBACKEND_LINE_FLAGS(VTBACKEND_LINE_FLAG_ROW)
#undef VTBACKEND_LINE_FLAG_ROW
};

/// Flags that describe the LOGICAL line, and therefore belong to its first physical line alone.
///
/// A logical line that reflow splits across several physical lines has exactly one head. The semantic
/// marks a shell emits (OSC 133, and Contour's own SETMARK) point at that head — at the line the prompt
/// starts on, at the line the output starts on — never at the chunks a wrap happens to produce. Copying
/// them onto the continuations would turn one prompt into several the moment the window is widened, which
/// is precisely what findMarkerUpwards() and the command-block scan would then walk into.
constexpr inline auto HeadOnlyLineFlags =
    LineFlags { LineFlag::Marked, LineFlag::OutputStart, LineFlag::CommandEnd, LineFlag::PromptEnd };

} // namespace vtbackend

template <>
struct std::formatter<vtbackend::LineFlags>: formatter<std::string>
{
    auto format(vtbackend::LineFlags const flags, auto& ctx) const
    {
        std::string s;
        auto const append = [&s, flags](vtbackend::LineFlags flag, std::string_view name) {
            if ((flag & flags) == vtbackend::LineFlag::None)
                return;
            if (!s.empty())
                s += ",";
            s += name;
        };

#define VTBACKEND_LINE_FLAG_APPEND(Name, Bit) append(vtbackend::LineFlag::Name, #Name);
        VTBACKEND_LINE_FLAGS(VTBACKEND_LINE_FLAG_APPEND)
#undef VTBACKEND_LINE_FLAG_APPEND

        return formatter<std::string>::format(s, ctx);
    }
};
