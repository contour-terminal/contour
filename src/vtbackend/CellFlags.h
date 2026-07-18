// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <crispy/flags.h>

#include <array>
#include <cstdint>
#include <format>
#include <string_view>

/// The single source for every cell flag: one row each, naming the flag and giving its bit.
///
/// The `CellFlag` enumeration, the enumerable `CellFlagList`, and the `std::formatter` below are all
/// generated from this table, so **adding a flag is adding one row here** and cannot leave any of the
/// three behind.
///
/// That is not a hypothetical. These were three hand-maintained lists until `CharacterProtectedISO`
/// was added, and the third rotted the same day: the VT conformance screen dump kept its own copy of
/// the flags to iterate, silently omitted the new one, and its protected-area goldens could therefore
/// not tell a protected cell from an unprotected one -- the dump recorded two visibly different cells
/// as the same rendition.
#define VTBACKEND_CELL_FLAGS(_)                                                                   \
    /* SGR renditions, in SGR order where there is one. */                                        \
    _(Bold, 0)                                                                                    \
    _(Faint, 1)                                                                                   \
    _(Italic, 2)                                                                                  \
    _(Underline, 3)                                                                               \
    _(Blinking, 4)                                                                                \
    _(Inverse, 5)                                                                                 \
    _(Hidden, 6)                                                                                  \
    _(CrossedOut, 7)                                                                              \
    _(DoublyUnderlined, 8)                                                                        \
    _(CurlyUnderlined, 9)                                                                         \
    _(DottedUnderline, 10)                                                                        \
    _(DashedUnderline, 11)                                                                        \
    _(Framed, 12)                                                                                 \
    _(Encircled, 13)                                                                              \
    _(Overline, 14)                                                                               \
    _(RapidBlinking, 15)                                                                          \
    /* DECSCA (DEC) protection: spared by the SELECTIVE erases (DECSED, DECSEL). */               \
    _(CharacterProtected, 16)                                                                     \
    /* Structural, not a rendition: this cell continues a wide character to its left. */          \
    _(WideCharContinuation, 17)                                                                   \
    /* SPA/EPA (ISO 6429) protection: spared by the REGULAR erases (ED, EL). A separate flag from \
       CharacterProtected because the two are honoured by opposite erase families. */             \
    _(CharacterProtectedISO, 18)                                                                  \
    /* Structural: this cell continues a text-sizing block (OSC 66) on the line ABOVE. Distinct   \
       from WideCharContinuation, which only ever means "to the left" -- a scaled block is the    \
       first thing in this grid that is taller than one line. */                                  \
    _(MulticellContinuation, 19)

namespace vtbackend
{

/// A single visual or structural attribute of a cell. @see VTBACKEND_CELL_FLAGS for the table these
/// enumerators are generated from, and for what each one means.
enum class CellFlag : uint32_t
{
    None = 0,

#define VTBACKEND_CELL_FLAG_ENUMERATOR(Name, Bit) Name = (1U << (Bit)),
    VTBACKEND_CELL_FLAGS(VTBACKEND_CELL_FLAG_ENUMERATOR)
#undef VTBACKEND_CELL_FLAG_ENUMERATOR
};

using CellFlags = crispy::flags<CellFlag>;

/// Every `CellFlag`, in declaration order, excluding `None`.
///
/// The formatter below *names* one flag; this *enumerates* them, which naming cannot do. Any code that
/// must visit every flag reads this rather than writing the list out again.
inline constexpr auto CellFlagList = std::array {
#define VTBACKEND_CELL_FLAG_ROW(Name, Bit) CellFlag::Name,
    VTBACKEND_CELL_FLAGS(VTBACKEND_CELL_FLAG_ROW)
#undef VTBACKEND_CELL_FLAG_ROW
};

/// All underline-variant flags. Grouped here, next to the flag definitions, so a new underline style
/// is added in one place; consumers that treat "any underline" uniformly (e.g. DECATC color mapping)
/// reference this instead of re-enumerating the variants.
inline constexpr CellFlags UnderlineMask = CellFlags { CellFlag::Underline } | CellFlag::DoublyUnderlined
                                           | CellFlag::CurlyUnderlined | CellFlag::DottedUnderline
                                           | CellFlag::DashedUnderline;

} // namespace vtbackend

// {{{
template <>
struct std::formatter<vtbackend::CellFlag>: std::formatter<std::string_view>
{
    auto format(const vtbackend::CellFlag value, auto& ctx) const
    {
        string_view s;

        // clang-format off
        switch (value)
        {
            case vtbackend::CellFlag::None: s = std::string_view("None"); break;
#define VTBACKEND_CELL_FLAG_CASE(Name, Bit) \
            case vtbackend::CellFlag::Name: s = std::string_view(#Name); break;
            VTBACKEND_CELL_FLAGS(VTBACKEND_CELL_FLAG_CASE)
#undef VTBACKEND_CELL_FLAG_CASE
        }
        // clang-format on

        return formatter<string_view>::format(s, ctx);
    }
};
// }}}
