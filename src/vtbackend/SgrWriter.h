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

/// One SGR rendition flag: the CellFlag and the SGR parameter that selects it on a plain terminal.
struct SgrFlagCode
{
    CellFlag flag;
    int code;
};

/// The renditions a plain terminal can reproduce, in SGR order — the SINGLE source of truth shared by
/// makeSgrSequence (capture-pane) and the native-attach mirror's TtyRenderer, so the two can never
/// drift out of step (an added attribute renders on both paths or neither). The underline variants
/// (curly/dotted/dashed) collapse onto plain underline (4); rapid blink onto 6.
inline constexpr auto SgrFlagCodes = std::array {
    SgrFlagCode { CellFlag::Bold, 1 },
    SgrFlagCode { CellFlag::Faint, 2 },
    SgrFlagCode { CellFlag::Italic, 3 },
    SgrFlagCode { CellFlag::Underline, 4 },
    SgrFlagCode { CellFlag::DoublyUnderlined, 21 },
    SgrFlagCode { CellFlag::CurlyUnderlined, 4 },
    SgrFlagCode { CellFlag::DottedUnderline, 4 },
    SgrFlagCode { CellFlag::DashedUnderline, 4 },
    SgrFlagCode { CellFlag::Blinking, 5 },
    SgrFlagCode { CellFlag::RapidBlinking, 6 },
    SgrFlagCode { CellFlag::Inverse, 7 },
    SgrFlagCode { CellFlag::Hidden, 8 },
    SgrFlagCode { CellFlag::CrossedOut, 9 },
    SgrFlagCode { CellFlag::Overline, 53 },
};

/// Appends the SGR parameters that select @p color as a foreground (@p foreground true) or background
/// colour to @p params (a semicolon-separated CSI parameter list, without the leading/trailing punct).
/// The default colour contributes nothing (a leading `0` reset already selects it). Bright and indexed
/// 0..7 use the 30/40/90/100 aixterm forms; indexed 8..255 use `38;5;n`/`48;5;n`; RGB uses
/// `38;2;r;g;b`/`48;2;r;g;b`.
/// @param params The parameter list being built (semicolon-joined; empty means "just started").
/// @param color The cell colour to encode.
/// @param foreground Whether @p color is the foreground (else background).
inline void appendSgrColor(std::string& params, Color color, bool foreground)
{
    auto const sep = [&] {
        return params.empty() ? "" : ";";
    };
    switch (color.type())
    {
        case ColorType::Default:
        case ColorType::Undefined: return; // reset already selects the default
        case ColorType::Indexed: {
            auto const index = int { color.index() };
            if (index < 8)
                params += std::format("{}{}", sep(), (foreground ? 30 : 40) + index);
            else
                params += std::format("{}{};5;{}", sep(), foreground ? 38 : 48, index);
            return;
        }
        case ColorType::Bright:
            params += std::format("{}{}", sep(), (foreground ? 90 : 100) + int { color.index() });
            return;
        case ColorType::RGB: {
            auto const rgb = color.rgb();
            params += std::format("{}{};2;{};{};{}",
                                  sep(),
                                  foreground ? 38 : 48,
                                  int { rgb.red },
                                  int { rgb.green },
                                  int { rgb.blue });
            return;
        }
    }
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
    for (auto const& [flag, code]: SgrFlagCodes)
        if (attrs.flags.contains(flag))
            params += std::format(";{}", code);
    appendSgrColor(params, attrs.foregroundColor, /*foreground=*/true);
    appendSgrColor(params, attrs.backgroundColor, /*foreground=*/false);
    return std::format("\033[{}m", params);
}

} // namespace vtbackend
