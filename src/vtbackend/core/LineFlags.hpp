// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/Flags.hpp>

#include <array>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>

/// The single source for every line flag: one row each, naming the flag, giving its bit and saying which
/// LineFlagRole it plays.
///
/// The `LineFlag` enumeration, the enumerable `LineFlagList`, the `std::formatter` below and the flag sets
/// readers test against -- HeadOnlyLineFlags, ShellMarkLineFlags, NavigationMarkFlags -- are all generated
/// from this table, so **adding a flag is adding one row here** and cannot leave any of them behind. This
/// mirrors VTBACKEND_CELL_FLAGS in CellFlags.h, which was converted for exactly this reason after a
/// hand-maintained copy of the flags rotted the day a flag was added.
#define VTBACKEND_LINE_FLAGS(_)                                                                   \
    /* Structural: whether this line may reflow, and whether it continues the one above. */       \
    _(Wrappable, 0, Physical)                                                                     \
    _(Wrapped, 1, Physical)                                                                       \
    /* Semantic marks. A prompt starts here (OSC 133;A, or the deprecated SETMARK). */            \
    _(Marked, 2, ShellMark)                                                                       \
    /* Command output begins (OSC 133;C). */                                                      \
    _(OutputStart, 3, ShellMark)                                                                  \
    /* Double-width / double-height renditions (DECDWL, DECDHL). */                               \
    _(DoubleWidth, 4, Rendition)                                                                  \
    _(DoubleHeightTop, 5, Rendition)                                                              \
    _(DoubleHeightBottom, 6, Rendition)                                                           \
    /* Command finished (OSC 133;D); the column it stopped at is Line::commandEndOffset(). */     \
    _(CommandEnd, 7, ShellMark)                                                                   \
    /* The shell's prompt finished printing and user input begins (OSC 133;B); the column it */   \
    /* ended at is Line::promptEndOffset(). */                                                    \
    _(PromptEnd, 8, ShellMark)                                                                    \
    /* The user's own bookmark (vi `mm`): a navigation target, never a command-block boundary. */ \
    _(UserMark, 9, UserMark)

namespace vtbackend
{

/// Flags associated with a terminal line. @see VTBACKEND_LINE_FLAGS for the table these enumerators are
/// generated from, and for what each one means.
enum class LineFlag : uint16_t
{
    None = 0,

#define VTBACKEND_LINE_FLAG_ENUMERATOR(Name, Bit, Role) Name = (1U << (Bit)),
    VTBACKEND_LINE_FLAGS(VTBACKEND_LINE_FLAG_ENUMERATOR)
#undef VTBACKEND_LINE_FLAG_ENUMERATOR
};

using LineFlags = core::Flags<LineFlag>;

/// Every `LineFlag`, in declaration order, excluding `None`.
///
/// The formatter below *names* one flag; this *enumerates* them, which naming cannot do. Any code that
/// must visit every flag reads this rather than writing the list out again.
inline constexpr auto LineFlagList = std::array {
#define VTBACKEND_LINE_FLAG_ROW(Name, Bit, Role) LineFlag::Name,
    VTBACKEND_LINE_FLAGS(VTBACKEND_LINE_FLAG_ROW)
#undef VTBACKEND_LINE_FLAG_ROW
};

/// What a line flag is for: the third column of VTBACKEND_LINE_FLAGS.
enum class LineFlagRole : uint8_t
{
    Physical = 0, ///< Describes the physical row -- how it reflows -- whichever piece it is.
    Rendition,    ///< How the physical row is drawn: DECDWL/DECDHL, which a whole-row ED resets.
    ShellMark, ///< A mark the shell emits (OSC 133, SETMARK, OSC 3008 standing in): command-block structure.
    UserMark,  ///< A mark the user sets (vi `mm`): somewhere to come back to, never structure.
};

/// Every `LineFlag` with its role, in declaration order.
inline constexpr auto LineFlagRoles = std::array {
#define VTBACKEND_LINE_FLAG_ROLE(Name, Bit, Role) std::pair { LineFlag::Name, LineFlagRole::Role },
    VTBACKEND_LINE_FLAGS(VTBACKEND_LINE_FLAG_ROLE)
#undef VTBACKEND_LINE_FLAG_ROLE
};

/// @param role A role.
/// @return Every flag the table gives @p role.
[[nodiscard]] constexpr LineFlags lineFlagsOf(LineFlagRole role) noexcept
{
    auto flags = LineFlags {};
    for (auto const& [flag, flagRole]: LineFlagRoles)
        if (flagRole == role)
            flags.enable(flag);
    return flags;
}

/// The marks a shell emits: the command blocks' STRUCTURE. A reader that means "the shell spoke here" --
/// the fold-range cache, the live-prompt scan -- reads these, never the user's own marks.
constexpr inline auto ShellMarkLineFlags = lineFlagsOf(LineFlagRole::ShellMark);

/// The marks a command block's head is found by: where its prompt starts (OSC 133;A) and, for a block an
/// OSC 133;C began, where its output starts.
constexpr inline auto BlockHeadLineFlags = LineFlags { LineFlag::Marked, LineFlag::OutputStart };

/// The line size renditions (DECDWL, DECDHL): what DECSWL and an ED that erases the row whole reset.
constexpr inline auto LineRenditionFlags = lineFlagsOf(LineFlagRole::Rendition);

/// Flags that describe the LOGICAL line, and therefore belong to its first physical line alone.
///
/// A logical line that reflow splits across several physical lines has exactly one head. The marks -- the
/// ones a shell emits (OSC 133, and Contour's own SETMARK) and the user's own bookmark (vi `mm`) -- point at
/// that head: at the line the prompt starts on, at the line the output starts on, at the line the user
/// marked; never at the chunks a wrap happens to produce. Copying them onto the continuations would turn one
/// prompt into several the moment the window is widened, which is precisely what findMarkerUpwards() and the
/// command-block scan would then walk into.
constexpr inline auto HeadOnlyLineFlags = ShellMarkLineFlags | lineFlagsOf(LineFlagRole::UserMark);

/// The marks a user NAVIGATES between: prompts (OSC 133;A, OSC 3008's stand-in) and the user's own
/// bookmarks (vi `mm`). `[m`/`]m`, the `im`/`am` text objects, ScrollMarkUp/ScrollMarkDown and
/// CopyPreviousMarkRange read this; block STRUCTURE -- block-atomic eviction, folding, the command-block
/// and live-prompt scans -- reads LineFlag::Marked alone, so a bookmark never splits a command.
constexpr inline auto NavigationMarkFlags =
    LineFlags { LineFlag::Marked } | lineFlagsOf(LineFlagRole::UserMark);

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

#define VTBACKEND_LINE_FLAG_APPEND(Name, Bit, Role) append(vtbackend::LineFlag::Name, #Name);
        VTBACKEND_LINE_FLAGS(VTBACKEND_LINE_FLAG_APPEND)
#undef VTBACKEND_LINE_FLAG_APPEND

        return formatter<std::string>::format(s, ctx);
    }
};
