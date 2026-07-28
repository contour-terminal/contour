// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/CellFlags.h>
#include <vtbackend/Color.h>
#include <vtbackend/GraphicsAttributes.h>

#include <array>
#include <format>
#include <string>

namespace vtbackend
{

/// One SGR rendition flag: the CellFlag and the SGR parameter that selects it.
struct SgrFlagCode
{
    CellFlag flag = CellFlag::None;
    int code = 0;
    /// The ECMA-48 SUB-parameter, emitted as `code:sub`; 0 means the plain `code` alone.
    ///
    /// The underline styles exist only in this form — there is no standalone parameter for a curly
    /// underline — so a table of single ints cannot spell them, and every one of them collapsed onto
    /// plain `4`. That is what this field is for.
    int sub = 0;
};

/// The renditions a terminal can reproduce, in SGR order — the SINGLE source of truth for every
/// path that turns cell flags back into a sequence, so they cannot drift (an added attribute renders
/// on all of them or none).
///
/// The underline variants must come AFTER plain `Underline`: SGR parameters apply left to right, so a
/// cell carrying both would end up plain if the variant were emitted first.
///
/// `DoublyUnderlined` is `4:2` rather than the bare `21`: `21` is read as "bold off" by a number of
/// terminals, while the sub-parameter form is unambiguous — and it is what this terminal's own DECRQSS
/// report has always answered. `CellFlag::Encircled` is absent because vtbackend does not implement it.
inline constexpr auto SgrFlagCodes = std::array {
    SgrFlagCode { .flag = CellFlag::Bold, .code = 1 },
    SgrFlagCode { .flag = CellFlag::Faint, .code = 2 },
    SgrFlagCode { .flag = CellFlag::Italic, .code = 3 },
    SgrFlagCode { .flag = CellFlag::Underline, .code = 4 },
    SgrFlagCode { .flag = CellFlag::DoublyUnderlined, .code = 4, .sub = 2 },
    SgrFlagCode { .flag = CellFlag::CurlyUnderlined, .code = 4, .sub = 3 },
    SgrFlagCode { .flag = CellFlag::DottedUnderline, .code = 4, .sub = 4 },
    SgrFlagCode { .flag = CellFlag::DashedUnderline, .code = 4, .sub = 5 },
    SgrFlagCode { .flag = CellFlag::Blinking, .code = 5 },
    SgrFlagCode { .flag = CellFlag::RapidBlinking, .code = 6 },
    SgrFlagCode { .flag = CellFlag::Inverse, .code = 7 },
    SgrFlagCode { .flag = CellFlag::Hidden, .code = 8 },
    SgrFlagCode { .flag = CellFlag::CrossedOut, .code = 9 },
    SgrFlagCode { .flag = CellFlag::Framed, .code = 51 },
    SgrFlagCode { .flag = CellFlag::Overline, .code = 53 },
};

/// The SGR parameter text selecting @p entry — `"4"`, or `"4:3"` where it has a sub-parameter.
/// @param entry The table row to render.
/// @return The parameter, without any separator.
[[nodiscard]] inline std::string sgrParameterOf(SgrFlagCode const& entry)
{
    return entry.sub != 0 ? std::format("{}:{}", entry.code, entry.sub) : std::format("{}", entry.code);
}

/// Which of a cell's three colours a run of SGR parameters selects.
enum class SgrColorTarget : uint8_t
{
    Foreground = 0, ///< The aixterm short forms 30..37 / 90..97, or the extended introducer 38.
    Background = 1, ///< The aixterm short forms 40..47 / 100..107, or the extended introducer 48.

    /// SGR 58 — the mintty/kitty/libvte underline-colour extension this terminal parses (@see
    /// Screen's SGR handler). It has no short form at all, so an indexed or bright colour goes out
    /// as `58;5;n` where a foreground would have used a single parameter.
    Underline = 2,
};

/// The extended-colour introducer @p target is selected by: 38, 48 or 58.
/// @param target The colour being encoded.
/// @return The SGR parameter that introduces a `;5;n` or `;2;r;g;b` run.
[[nodiscard]] constexpr int sgrExtendedColorCode(SgrColorTarget target) noexcept
{
    switch (target)
    {
        case SgrColorTarget::Foreground: return 38;
        case SgrColorTarget::Background: return 48;
        case SgrColorTarget::Underline: return 58;
    }
    return 38;
}

/// Appends the SGR parameters that select @p color for @p target to @p params (a semicolon-separated
/// CSI parameter list, without the leading/trailing punctuation).
///
/// The default colour contributes nothing — a leading `0` reset already selects it. Foreground and
/// background use the 30/40/90/100 aixterm forms for indexed 0..7 and bright, `38;5;n`/`48;5;n` for
/// indexed 8..255, and `38;2;r;g;b`/`48;2;r;g;b` for RGB. The underline colour has no short forms, so
/// every non-RGB value goes out as `58;5;n` (a bright colour being palette index 8 + its own index).
/// @param params The parameter list being built (semicolon-joined; empty means "just started").
/// @param color The cell colour to encode.
/// @param target Which colour of the cell @p color is.
inline void appendSgrColor(std::string& params, Color color, SgrColorTarget target)
{
    auto const sep = [&] {
        return params.empty() ? "" : ";";
    };
    auto const extended = sgrExtendedColorCode(target);
    // SGR 58 has NO short form, so an underline colour always takes the `;5;n` / `;2;r;g;b` branch;
    // the two other targets keep the aixterm spellings a terminal is likeliest to understand.
    auto const hasShortForm = target != SgrColorTarget::Underline;
    switch (color.type())
    {
        case ColorType::Default:
        case ColorType::Undefined: return; // reset already selects the default
        case ColorType::Indexed: {
            auto const index = int { color.index() };
            if (hasShortForm && index < 8)
                params +=
                    std::format("{}{}", sep(), (target == SgrColorTarget::Foreground ? 30 : 40) + index);
            else
                params += std::format("{}{};5;{}", sep(), extended, index);
            return;
        }
        case ColorType::Bright: {
            // Bright IS palette entries 8..15 under another name, which is how SGR 58 must spell it.
            auto const index = int { color.index() };
            if (hasShortForm)
                params +=
                    std::format("{}{}", sep(), (target == SgrColorTarget::Foreground ? 90 : 100) + index);
            else
                params += std::format("{}{};5;{}", sep(), extended, 8 + index);
            return;
        }
        case ColorType::RGB: {
            auto const rgb = color.rgb();
            params += std::format(
                "{}{};2;{};{};{}", sep(), extended, int { rgb.red }, int { rgb.green }, int { rgb.blue });
            return;
        }
    }
}

/// Appends every non-default attribute of @p attrs to @p params: the style flags in SGR order, then
/// the foreground, background and underline colours.
///
/// The SINGLE encoder for a full rendition, so no consumer can carry a private copy that quietly
/// omits an attribute — which is exactly how `capture-pane -e` came to drop every underline colour
/// that DECRQSS reported correctly.
/// @param params The parameter list being built (semicolon-joined; empty means "just started").
/// @param attrs The cell's graphics attributes.
inline void appendSgrParameters(std::string& params, GraphicsAttributes const& attrs)
{
    auto const sep = [&] {
        return params.empty() ? "" : ";";
    };
    for (auto const& entry: SgrFlagCodes)
        if (attrs.flags.contains(entry.flag))
            params += std::format("{}{}", sep(), sgrParameterOf(entry));
    appendSgrColor(params, attrs.foregroundColor, SgrColorTarget::Foreground);
    appendSgrColor(params, attrs.backgroundColor, SgrColorTarget::Background);
    appendSgrColor(params, attrs.underlineColor, SgrColorTarget::Underline);
}

/// The SGR sequence that sets a cell's full rendition from a clean slate: a leading `0` (reset) plus
/// every non-default attribute of @p attrs (styles then colours), e.g. `\033[0;1;38;5;9m`. Emitting
/// reset-first makes each sequence self-contained, so a consumer that writes it on every rendition
/// CHANGE never leaks attributes between cells. All-default attributes yield `\033[0m`.
/// @param attrs The cell's graphics attributes.
/// @return The CSI SGR sequence, terminator included.
[[nodiscard]] inline std::string makeSgrSequence(GraphicsAttributes const& attrs)
{
    auto params = std::string { "0" };
    appendSgrParameters(params, attrs);
    return std::format("\033[{}m", params);
}

} // namespace vtbackend
