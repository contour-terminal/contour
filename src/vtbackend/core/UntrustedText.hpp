// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

namespace vtbackend
{

/// What text a program or a hook sent, shown to the user, does with one of its code points.
enum class CodePointTreatment : uint8_t
{
    Keep = 0, ///< Shown as it is.
    Space,    ///< A line break: shown as one space, so the words on either side stay apart.
    Drop,     ///< A control or an invisible formatting character: never shown.
};

/// A closed range of code points and what untrusted text does with them.
struct CodePointTreatmentRange
{
    char32_t first;               ///< The first code point.
    char32_t last;                ///< The last code point, inclusive.
    CodePointTreatment treatment; ///< What becomes of them.
};

/// The one table every sanitizer of untrusted display text reads (a pane's status text in the daemon, a
/// tab badge in the GUI), so that what one shows the other does too. The first row a code point falls in
/// decides; a code point in none is kept.
///
/// Dropped: the C0 and C1 controls, whose ESC and BEL would introduce a sequence; every
/// Default_Ignorable_Code_Point (DerivedCoreProperties.txt) -- zero-width characters, bidi marks,
/// embeddings, overrides and isolates, invisible operators, Hangul fillers, the byte-order mark, tag
/// characters -- which can reorder or hide what a reader sees; and the invisible format characters that are
/// not default-ignorable (prepended concatenation marks, Egyptian hieroglyph and musical symbol format
/// controls, interlinear annotation). Kept although default-ignorable: the variation selectors (U+180B..180D,
/// U+180F, U+FE00..FE0F, U+E0100..E01EF), which only choose how a character looks, an emoji's colour form
/// among them, and can hide or reorder nothing.
inline constexpr auto UntrustedTextTreatments = std::array {
    // Line breaks first: they are controls too, but the words they separate must not run together.
    CodePointTreatmentRange { .first = 0x000A, .last = 0x000D, .treatment = CodePointTreatment::Space },
    CodePointTreatmentRange { .first = 0x0085, .last = 0x0085, .treatment = CodePointTreatment::Space },
    CodePointTreatmentRange { .first = 0x2028, .last = 0x2029, .treatment = CodePointTreatment::Space },
    // Controls.
    CodePointTreatmentRange { .first = 0x0000, .last = 0x001F, .treatment = CodePointTreatment::Drop },
    CodePointTreatmentRange { .first = 0x007F, .last = 0x009F, .treatment = CodePointTreatment::Drop },
    // Default ignorables, variation selectors aside.
    CodePointTreatmentRange { .first = 0x00AD, .last = 0x00AD, .treatment = CodePointTreatment::Drop },
    CodePointTreatmentRange { .first = 0x034F, .last = 0x034F, .treatment = CodePointTreatment::Drop },
    CodePointTreatmentRange { .first = 0x061C, .last = 0x061C, .treatment = CodePointTreatment::Drop },
    CodePointTreatmentRange { .first = 0x115F, .last = 0x1160, .treatment = CodePointTreatment::Drop },
    CodePointTreatmentRange { .first = 0x17B4, .last = 0x17B5, .treatment = CodePointTreatment::Drop },
    CodePointTreatmentRange { .first = 0x180E, .last = 0x180E, .treatment = CodePointTreatment::Drop },
    CodePointTreatmentRange { .first = 0x200B, .last = 0x200F, .treatment = CodePointTreatment::Drop },
    CodePointTreatmentRange { .first = 0x202A, .last = 0x202E, .treatment = CodePointTreatment::Drop },
    CodePointTreatmentRange { .first = 0x2060, .last = 0x206F, .treatment = CodePointTreatment::Drop },
    CodePointTreatmentRange { .first = 0x3164, .last = 0x3164, .treatment = CodePointTreatment::Drop },
    CodePointTreatmentRange { .first = 0xFEFF, .last = 0xFEFF, .treatment = CodePointTreatment::Drop },
    CodePointTreatmentRange { .first = 0xFFA0, .last = 0xFFA0, .treatment = CodePointTreatment::Drop },
    CodePointTreatmentRange { .first = 0xFFF0, .last = 0xFFF8, .treatment = CodePointTreatment::Drop },
    CodePointTreatmentRange { .first = 0x1BCA0, .last = 0x1BCA3, .treatment = CodePointTreatment::Drop },
    CodePointTreatmentRange { .first = 0x1D173, .last = 0x1D17A, .treatment = CodePointTreatment::Drop },
    CodePointTreatmentRange { .first = 0xE0000, .last = 0xE00FF, .treatment = CodePointTreatment::Drop },
    CodePointTreatmentRange { .first = 0xE01F0, .last = 0xE0FFF, .treatment = CodePointTreatment::Drop },
    // Invisible format characters that are not default-ignorable.
    CodePointTreatmentRange { .first = 0x0600, .last = 0x0605, .treatment = CodePointTreatment::Drop },
    CodePointTreatmentRange { .first = 0x06DD, .last = 0x06DD, .treatment = CodePointTreatment::Drop },
    CodePointTreatmentRange { .first = 0x070F, .last = 0x070F, .treatment = CodePointTreatment::Drop },
    CodePointTreatmentRange { .first = 0x0890, .last = 0x0891, .treatment = CodePointTreatment::Drop },
    CodePointTreatmentRange { .first = 0x08E2, .last = 0x08E2, .treatment = CodePointTreatment::Drop },
    CodePointTreatmentRange { .first = 0xFFF9, .last = 0xFFFB, .treatment = CodePointTreatment::Drop },
    CodePointTreatmentRange { .first = 0x110BD, .last = 0x110BD, .treatment = CodePointTreatment::Drop },
    CodePointTreatmentRange { .first = 0x110CD, .last = 0x110CD, .treatment = CodePointTreatment::Drop },
    CodePointTreatmentRange { .first = 0x13430, .last = 0x1343F, .treatment = CodePointTreatment::Drop },
};

/// @param codePoint A code point of untrusted display text.
/// @return What showing it does with @p codePoint, as the first row of UntrustedTextTreatments that holds it
///         says; Keep when none does.
[[nodiscard]] constexpr CodePointTreatment treatmentOf(char32_t codePoint) noexcept
{
    auto const row = std::ranges::find_if(UntrustedTextTreatments, [codePoint](auto const& range) {
        return codePoint >= range.first && codePoint <= range.last;
    });
    return row != UntrustedTextTreatments.end() ? row->treatment : CodePointTreatment::Keep;
}

/// How one code point of untrusted text is shown, given the code points before it.
enum class ShownAs : uint8_t
{
    Nothing = 0, ///< Not shown: dropped, or a line break not (yet) between two shown code points.
    Itself,      ///< Shown as it is.
    AfterSpace,  ///< Shown as it is, after the one space the run of line breaks before it becomes.
};

/// The one rule every sanitizer of untrusted display text folds its code points by, after
/// UntrustedTextTreatments: what is dropped is not shown, and a run of line breaks becomes one space only
/// between two shown code points, so there is none at either end.
class UntrustedTextFilter
{
  public:
    /// @param treatment The next code point's treatment (treatmentOf).
    /// @return How that code point is shown.
    [[nodiscard]] constexpr ShownAs next(CodePointTreatment treatment) noexcept
    {
        switch (treatment)
        {
            case CodePointTreatment::Drop: return ShownAs::Nothing;
            case CodePointTreatment::Space:
                if (_state == State::Shown)
                    _state = State::BreakPending;
                return ShownAs::Nothing;
            case CodePointTreatment::Keep: break;
        }
        auto const shown = _state == State::BreakPending ? ShownAs::AfterSpace : ShownAs::Itself;
        _state = State::Shown;
        return shown;
    }

  private:
    /// Where in the text the filter is.
    enum class State : uint8_t
    {
        Empty = 0,    ///< Nothing shown yet: a line break here is dropped.
        Shown,        ///< The last code point not dropped was shown.
        BreakPending, ///< Line breaks followed a shown code point; a space goes before the next one shown.
    };

    State _state = State::Empty;
};

} // namespace vtbackend
