// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/VTType.h>

#include <crispy/defines.h>
#include <crispy/escape.h>
#include <crispy/sort.h>

#include <fmt/format.h>

#include <gsl/pointers>
#include <gsl/span>

#include <array>
#include <optional>
#include <string>

namespace vtbackend
{

enum class FunctionCategory : uint8_t
{
    C0 = 0,
    ESC = 1,
    CSI = 2,
    OSC = 3,
    DCS = 4,
};

// VT sequence documentation in markdown format
struct FunctionDocumentation
{
    std::string_view mnemonic {};
    std::string_view comment {};
    std::string_view parameters {};
    std::string_view description {};
    std::string_view notes {};
    std::string_view examples {};
};

// {{{ documentation
// clang-format off
namespace documentation
{

// CSI
constexpr inline auto ANSIDSR = FunctionDocumentation { .mnemonic = "DSR", .comment = "Device Status Report (ANSI)" };
constexpr inline auto ANSISYSSC = FunctionDocumentation { .mnemonic = "ANSISYSSC", .comment = "Save Cursor (ANSI.SYS)" };
constexpr inline auto CBT = FunctionDocumentation { .mnemonic = "CBT", .comment = "Cursor Backward Tabulation" };
constexpr inline auto CHA = FunctionDocumentation { .mnemonic = "CHA", .comment = "Move cursor to column" };
constexpr inline auto CHT = FunctionDocumentation { .mnemonic = "CHT", .comment = "Cursor Horizontal Forward Tabulation" };
constexpr inline auto CNL = FunctionDocumentation { .mnemonic = "CNL", .comment = "Move cursor to next line" };
constexpr inline auto CPL = FunctionDocumentation { .mnemonic = "CPL", .comment = "Move cursor to previous line" };
constexpr inline auto CSIUENHCE = FunctionDocumentation { .mnemonic = "CSIUENHCE", .comment = "Request enhancement to extended keyboard mode" };
constexpr inline auto CSIUENTER = FunctionDocumentation { .mnemonic = "CSIUENTER", .comment = "Enter Extended keyboard mode" };
constexpr inline auto CSIULEAVE = FunctionDocumentation { .mnemonic = "CSIULEAVE", .comment = "Leave Extended keyboard mode" };
constexpr inline auto CSIUQUERY = FunctionDocumentation { .mnemonic = "CSIUQUERY", .comment = "Query Extended keyboard mode" };
constexpr inline auto CUB = FunctionDocumentation { .mnemonic = "CUB", .comment = "Move cursor backward" };
constexpr inline auto CUD = FunctionDocumentation { .mnemonic = "CUD", .comment = "Move cursor down" };
constexpr inline auto CUF = FunctionDocumentation { .mnemonic = "CUF", .comment = "Move cursor forward" };
constexpr inline auto CUP = FunctionDocumentation { .mnemonic = "CUP", .comment = "Move cursor to position", .parameters = "row ; column", .description = "This control function moves the cursor to the specified line and column. The " "starting point for lines and columns depends on the setting of origin mode (DECOM). " "CUP applies only to the current page.", .notes = "The CUP sequence is supported by all terminals. The home position is 1,1.", };
constexpr inline auto CUU = FunctionDocumentation { .mnemonic = "CUU", .comment = "Move cursor up" };
constexpr inline auto DA1 = FunctionDocumentation { .mnemonic = "DA1", .comment = "Primary Device Attributes" };
constexpr inline auto DA2 = FunctionDocumentation { .mnemonic = "DA2", .comment = "Secondary Device Attributes" };
constexpr inline auto DA3 = FunctionDocumentation { .mnemonic = "DA3", .comment = "Tertiary Device Attributes" };
constexpr inline auto DCH = FunctionDocumentation { .mnemonic = "DCH", .comment = "Delete characters" };
constexpr inline auto DECCARA = FunctionDocumentation { .mnemonic = "DECCARA", .comment = "Change Attributes in Rectangular Area" };
constexpr inline auto DECCRA = FunctionDocumentation { .mnemonic = "DECCRA", .comment = "Copy rectangular area" };
constexpr inline auto DECDC = FunctionDocumentation { .mnemonic = "DECDC", .comment = "Delete column" };
constexpr inline auto DECERA = FunctionDocumentation { .mnemonic = "DECERA", .comment = "Erase rectangular area" };
constexpr inline auto DECFRA = FunctionDocumentation { .mnemonic = "DECFRA", .comment = "Fill rectangular area" };
constexpr inline auto DECIC = FunctionDocumentation { .mnemonic = "DECIC", .comment = "Insert column" };
constexpr inline auto DECPS = FunctionDocumentation { .mnemonic = "DECPS", .comment = "Controls the sound frequency or notes" };
constexpr inline auto DECRM = FunctionDocumentation { .mnemonic = "DECRM", .comment = "Reset DEC-mode" };
constexpr inline auto DECRQM = FunctionDocumentation { .mnemonic = "DECRQM", .comment = "Request DEC-mode" };
constexpr inline auto DECRQM_ANSI = FunctionDocumentation { .mnemonic = "DECRQM_ANSI", .comment = "Request ANSI-mode" }; // NOLINT
constexpr inline auto DECRQPSR = FunctionDocumentation { .mnemonic = "DECRQPSR", .comment = "Request presentation state report" };
constexpr inline auto DECSASD = FunctionDocumentation { .mnemonic = "DECSASD", .comment = "Select Active Status Display" };
constexpr inline auto DECSCA = FunctionDocumentation { .mnemonic = "DECSCA", .comment = "Select Character Protection Attribute" };
constexpr inline auto DECSCL = FunctionDocumentation { .mnemonic = "DECSCL", .comment = "Set conformance level (DECSCL), VT220 and up." };
constexpr inline auto DECSCPP = FunctionDocumentation { .mnemonic = "DECSCPP", .comment = "Select 80 or 132 Columns per Page" };
constexpr inline auto DECSCUSR = FunctionDocumentation { .mnemonic = "DECSCUSR", .comment = "Set Cursor Style" };
constexpr inline auto DECSED = FunctionDocumentation { .mnemonic = "DECSED", .comment = "Selective Erase in Display" };
constexpr inline auto DECSEL = FunctionDocumentation { .mnemonic = "DECSEL", .comment = "Selective Erase in Line" };
constexpr inline auto DECSERA = FunctionDocumentation { .mnemonic = "DECSERA", .comment = "Selective Erase in Rectangular Area" };
constexpr inline auto DECSLRM = FunctionDocumentation { .mnemonic = "DECSLRM", .comment = "Set left/right margin" };
constexpr inline auto DECSM = FunctionDocumentation { .mnemonic = "DECSM", .comment = "Set DEC-mode" };
constexpr inline auto DECSNLS = FunctionDocumentation { .mnemonic = "DECSNLS", .comment = "Select number of lines per screen." };
constexpr inline auto DECSPP = FunctionDocumentation { .mnemonic = "DECSPP", .comment = "Set port parameter" };
constexpr inline auto DECSSCLS = FunctionDocumentation { .mnemonic = "DECSSCLS", .comment = "Set Scroll Speed." };
constexpr inline auto DECSSDT = FunctionDocumentation { .mnemonic = "DECSSDT", .comment = "Select Status Display (Line) Type" };
constexpr inline auto DECSTBM = FunctionDocumentation { .mnemonic = "DECSTBM", .comment = "Set top/bottom margin" };
constexpr inline auto DECSTR = FunctionDocumentation { .mnemonic = "DECSTR", .comment = "Soft terminal reset" };
constexpr inline auto DECXCPR = FunctionDocumentation { .mnemonic = "DECXCPR", .comment = "Report cursor position" };
constexpr inline auto DL = FunctionDocumentation { .mnemonic = "DL", .comment = "Delete lines" };
constexpr inline auto DSR = FunctionDocumentation { .mnemonic = "DSR", .comment = "Device Status Report (DEC)" };
constexpr inline auto ECH = FunctionDocumentation { .mnemonic = "ECH", .comment = "Erase characters" };
constexpr inline auto ED = FunctionDocumentation { .mnemonic = "ED", .comment = "Erase in Display" };
constexpr inline auto EL = FunctionDocumentation { .mnemonic = "EL", .comment = "Erase in Line" };
constexpr inline auto HPA = FunctionDocumentation { .mnemonic = "HPA", .comment = "Horizontal Position Absolute" };
constexpr inline auto HPR = FunctionDocumentation { .mnemonic = "HPR", .comment = "Horizontal Position Relative" };
constexpr inline auto HVP = FunctionDocumentation { .mnemonic = "HVP", .comment = "Horizontal and Vertical Position" };
constexpr inline auto ICH = FunctionDocumentation { .mnemonic = "ICH", .comment = "Insert characters" };
constexpr inline auto IL = FunctionDocumentation { .mnemonic = "IL", .comment = "Insert lines" };
constexpr inline auto REP = FunctionDocumentation { .mnemonic = "REP", .comment = "Repeat last character" };
constexpr inline auto RM = FunctionDocumentation { .mnemonic = "RM", .comment = "Reset Mode" };
constexpr inline auto SCOSC = FunctionDocumentation { .mnemonic = "SCOSC", .comment = "Save Cursor (available only when DECLRMM is disabled)" };
constexpr inline auto SD = FunctionDocumentation { .mnemonic = "SD", .comment = "Scroll Down" };
constexpr inline auto SETMARK = FunctionDocumentation { .mnemonic = "SETMARK", .comment = "Set Mark" };
constexpr inline auto SGR = FunctionDocumentation { .mnemonic = "SGR", .comment = "Select Graphic Rendition" };
constexpr inline auto SM = FunctionDocumentation { .mnemonic = "SM", .comment = "Set Mode" };
constexpr inline auto SU = FunctionDocumentation { .mnemonic = "SU", .comment = "Scroll Up" };
constexpr inline auto TBC = FunctionDocumentation { .mnemonic = "TBC", .comment = "Horizontal Tab Clear" };
constexpr inline auto VPA = FunctionDocumentation { .mnemonic = "VPA", .comment = "Vertical Position Absolute" };
constexpr inline auto WINMANIP = FunctionDocumentation { .mnemonic = "WINMANIP", .comment = "Window Manipulation" };
constexpr inline auto XTCAPTURE = FunctionDocumentation { .mnemonic = "XTCAPTURE", .comment = "Report screen buffer capture." };
constexpr inline auto XTPOPCOLORS = FunctionDocumentation { .mnemonic = "XTPOPCOLORS", .comment = "Pops the color palette from the palette's saved-stack." };
constexpr inline auto XTPUSHCOLORS = FunctionDocumentation { .mnemonic = "XTPUSHCOLORS", .comment = "Pushes the color palette onto the palette's saved-stack." };
constexpr inline auto XTREPORTCOLORS = FunctionDocumentation { .mnemonic = "XTREPORTCOLORS", .comment = "Reports number of color palettes on the stack." };
constexpr inline auto XTRESTORE = FunctionDocumentation { .mnemonic = "XTRESTORE", .comment = "Restore DEC private modes." };
constexpr inline auto XTSAVE = FunctionDocumentation { .mnemonic = "XTSAVE", .comment = "Save DEC private modes." };
constexpr inline auto XTSHIFTESCAPE = FunctionDocumentation { .mnemonic = "XTSHIFTESCAPE", .comment = "Set/reset shift-escape options" };
constexpr inline auto XTSMGRAPHICS = FunctionDocumentation { .mnemonic = "XTSMGRAPHICS", .comment = "Set/request graphics attribute" };
constexpr inline auto XTVERSION = FunctionDocumentation { .mnemonic = "XTVERSION", .comment = "Report xterm version" };

// DCS
constexpr inline auto DECRQSS = FunctionDocumentation { .mnemonic = "DECRQSS", .comment = "Request Status String" };
constexpr inline auto DECSIXEL = FunctionDocumentation { .mnemonic = "DECSIXEL", .comment = "Sixel Graphics Image" };
constexpr inline auto STP = FunctionDocumentation { .mnemonic = "STP", .comment = "Set Terminal Profile" };
constexpr inline auto XTGETTCAP = FunctionDocumentation { .mnemonic = "XTGETTCAP", .comment = "Request Termcap/Terminfo String" };

// OSC
constexpr inline auto CLIPBOARD = FunctionDocumentation { .mnemonic = "CLIPBOARD", .comment = "Clipboard management." };
constexpr inline auto COLORBG = FunctionDocumentation { .mnemonic = "COLORBG", .comment = "Change or request text background color." };
constexpr inline auto COLORCURSOR = FunctionDocumentation { .mnemonic = "COLORCURSOR", .comment = "Change text cursor color to Pt." };
constexpr inline auto COLORFG = FunctionDocumentation { .mnemonic = "COLORFG", .comment = "Change or request text foreground color." };
constexpr inline auto COLORMOUSEBG = FunctionDocumentation { .mnemonic = "COLORMOUSEBG", .comment = "Change mouse background color." };
constexpr inline auto COLORMOUSEFG = FunctionDocumentation { .mnemonic = "COLORMOUSEFG", .comment = "Change mouse foreground color." };
constexpr inline auto COLORSPECIAL = FunctionDocumentation { .mnemonic = "COLORSPECIAL", .comment = "Enable/disable Special Color Number c." };
constexpr inline auto DUMPSTATE = FunctionDocumentation { .mnemonic = "DUMPSTATE", .comment = "Dumps internal state to debug stream." };
constexpr inline auto HYPERLINK = FunctionDocumentation { .mnemonic = "HYPERLINK", .comment = "Hyperlinked Text" };
constexpr inline auto NOTIFY = FunctionDocumentation { .mnemonic = "NOTIFY", .comment = "Send Notification." };
constexpr inline auto RCOLORBG = FunctionDocumentation { .mnemonic = "RCOLORBG", .comment = "Reset VT100 text background color." };
constexpr inline auto RCOLORCURSOR = FunctionDocumentation { .mnemonic = "RCOLORCURSOR", .comment = "Reset text cursor color." };
constexpr inline auto RCOLORFG = FunctionDocumentation { .mnemonic = "RCOLORFG", .comment = "Reset VT100 text foreground color." };
constexpr inline auto RCOLORHIGHLIGHTBG = FunctionDocumentation { .mnemonic = "RCOLORHIGHLIGHTBG", .comment = "Reset highlight background color." };
constexpr inline auto RCOLORHIGHLIGHTFG = FunctionDocumentation { .mnemonic = "RCOLORHIGHLIGHTFG", .comment = "Reset highlight foreground color." };
constexpr inline auto RCOLORMOUSEBG = FunctionDocumentation { .mnemonic = "RCOLORMOUSEBG", .comment = "Reset mouse background color." };
constexpr inline auto RCOLORMOUSEFG = FunctionDocumentation { .mnemonic = "RCOLORMOUSEFG", .comment = "Reset mouse foreground color." };
constexpr inline auto RCOLPAL = FunctionDocumentation { .mnemonic = "RCOLPAL", .comment = "Reset color full palette or entry" };
constexpr inline auto SETCOLPAL = FunctionDocumentation { .mnemonic = "SETCOLPAL", .comment = "Set/Query color palette" };
constexpr inline auto SETCWD = FunctionDocumentation { .mnemonic = "SETCWD", .comment = "Set current working directory" };
constexpr inline auto SETFONT = FunctionDocumentation { .mnemonic = "SETFONT", .comment = "Get or set font." };
constexpr inline auto SETFONTALL = FunctionDocumentation { .mnemonic = "SETFONTALL", .comment = "Get or set all font faces, styles, size." };
constexpr inline auto SETICON = FunctionDocumentation { .mnemonic = "SETICON", .comment = "Change Icon Title" };
constexpr inline auto SETTITLE = FunctionDocumentation { .mnemonic = "SETTITLE", .comment = "Change Window & Icon Title" };
constexpr inline auto SETWINTITLE = FunctionDocumentation { .mnemonic = "SETWINTITLE", .comment = "Change Window Title" };
constexpr inline auto SETXPROP = FunctionDocumentation { .mnemonic = "SETXPROP", .comment = "Set X11 property" };

} // namespace documentation

// clang-format on
// }}}

/// Defines a function with all its syntax requirements plus some additional meta information.
struct FunctionDefinition // TODO: rename Function
{
    FunctionCategory category;  // (3 bits) C0, ESC, CSI, OSC, DCS
    char leader;                // (3 bits) 0x3C..0x3F (one of: < = > ?, or 0x00 for none)
    char intermediate;          // (4 bits) 0x20..0x2F (intermediates, usually just one, or 0x00 if none)
    char finalSymbol;           // (7 bits) 0x30..0x7E (final character)
    uint8_t minimumParameters;  // (4 bits) 0..7
    uint16_t maximumParameters; // (10 bits) 0..1024 for integer value (OSC function parameter)

    // Conformance level and extension are mutually exclusive.
    // But it is unclear to me whether or not it is guaranteed to always have a constexpr-aware std::variant.
    // So keep it the classic way (for now).
    VTType conformanceLevel;
    VTExtension extension = VTExtension::None;

    FunctionDocumentation documentation;

    template <typename... Args>
    std::string operator()(Args&&... parameters) const
    {
        assert(static_cast<size_t>(minimumParameters) <= sizeof...(Args));
        assert(sizeof...(Args) <= static_cast<size_t>(maximumParameters));
        std::string result;
        result.reserve(8);
        switch (category)
        {
            case FunctionCategory::C0: break;
            case FunctionCategory::ESC: result += "\033"; break;
            case FunctionCategory::CSI: result += "\033["; break;
            case FunctionCategory::OSC: result += "\033]"; break;
            case FunctionCategory::DCS: result += "\033P"; break;
        }
        if (leader)
            result += leader;
        if constexpr (sizeof...(Args) > 0)
        {
            result.reserve(sizeof...(Args) * 4);
            ((result += fmt::format("{};", std::forward<Args>(parameters))), ...);
            result.pop_back(); // remove trailing ';'
        }
        if (intermediate)
            result += intermediate;
        result += finalSymbol;
        return result;
    }

    using id_type = uint32_t;

    [[nodiscard]] constexpr id_type id() const noexcept
    {
        // clang-format off
        unsigned constexpr CategoryShift     = 0;
        unsigned constexpr LeaderShift       = 3;
        unsigned constexpr IntermediateShift = 3 + 3;
        unsigned constexpr FinalShift        = 3 + 3 + 4;
        unsigned constexpr MinParamShift     = 3 + 3 + 4 + 7;
        unsigned constexpr MaxParamShift     = 3 + 3 + 4 + 7 + 4;
        // clang-format on

        // if (category == FunctionCategory::C0)
        //     return static_cast<id_type>(category) | finalSymbol << 3;

        auto const maskCat = static_cast<id_type>(category) << CategoryShift;

        // 0x3C..0x3F; (one of: < = > ?, or 0x00 for none)
        auto const maskLeader = !leader ? 0 : (static_cast<id_type>(leader) - 0x3C) << LeaderShift;

        // 0x20..0x2F: (intermediates, usually just one, or 0x00 if none)
        auto const maskInterm =
            !intermediate ? 0 : (static_cast<id_type>(intermediate) - 0x20 + 1) << IntermediateShift;

        // 0x40..0x7E: final character
        auto const maskFinalS = !finalSymbol ? 0 : (static_cast<id_type>(finalSymbol) - 0x40) << FinalShift;
        auto const maskMinPar = static_cast<id_type>(minimumParameters) << MinParamShift;
        auto const maskMaxPar = static_cast<id_type>(maximumParameters) << MaxParamShift;

        return maskCat | maskLeader | maskInterm | maskFinalS | maskMinPar | maskMaxPar;
    }

    constexpr operator id_type() const noexcept { return id(); }
};

constexpr int compare(FunctionDefinition const& a, FunctionDefinition const& b)
{
    if (a.category != b.category)
        return static_cast<int>(a.category) - static_cast<int>(b.category);

    if (a.finalSymbol != b.finalSymbol) // XXX
        return static_cast<int>(a.finalSymbol) - static_cast<int>(b.finalSymbol);

    if (a.leader != b.leader)
        return a.leader - b.leader;

    if (a.intermediate != b.intermediate)
        return static_cast<int>(a.intermediate) - static_cast<int>(b.intermediate);

    if (a.minimumParameters != b.minimumParameters)
        return static_cast<int>(a.minimumParameters) - static_cast<int>(b.minimumParameters);

    return static_cast<int>(a.maximumParameters) - static_cast<int>(b.maximumParameters);
}

// clang-format off
constexpr bool operator==(FunctionDefinition const& a, FunctionDefinition const& b) noexcept { return compare(a, b) == 0; }
constexpr bool operator!=(FunctionDefinition const& a, FunctionDefinition const& b) noexcept { return compare(a, b) != 0; }
constexpr bool operator<=(FunctionDefinition const& a, FunctionDefinition const& b) noexcept { return compare(a, b) <= 0; }
constexpr bool operator>=(FunctionDefinition const& a, FunctionDefinition const& b) noexcept { return compare(a, b) >= 0; }
constexpr bool operator<(FunctionDefinition const& a, FunctionDefinition const& b) noexcept { return compare(a, b) < 0; }
constexpr bool operator>(FunctionDefinition const& a, FunctionDefinition const& b) noexcept { return compare(a, b) > 0; }
// clang-format on

struct FunctionSelector
{
    /// represents the corresponding function category.
    FunctionCategory category;
    /// an optional value between 0x3C .. 0x3F
    char leader;
    /// number of arguments supplied
    int argc;
    /// an optional intermediate character between (0x20 .. 0x2F)
    char intermediate;
    /// between 0x40 .. 0x7F
    char finalSymbol;
};

constexpr int compare(FunctionSelector const& a, FunctionDefinition const& b) noexcept
{
    if (a.category != b.category)
        return static_cast<int>(a.category) - static_cast<int>(b.category);

    if (a.finalSymbol != b.finalSymbol)
        return a.finalSymbol - b.finalSymbol;

    if (a.leader != b.leader)
        return a.leader - b.leader;

    if (a.intermediate != b.intermediate)
        return a.intermediate - b.intermediate;

    if (a.category == FunctionCategory::OSC)
        return static_cast<int>(a.argc) - static_cast<int>(b.maximumParameters);

    if (a.argc < b.minimumParameters)
        return -1;

    if (a.argc > b.maximumParameters)
        return +1;

    return 0;
}

namespace detail // {{{
{
    constexpr auto C0(char finalCharacter,
                      std::string_view mnemonic,
                      std::string_view comment,
                      VTType vt = VTType::VT100) noexcept
    {
        return FunctionDefinition { .category = FunctionCategory::C0,
                                    .leader = 0,
                                    .intermediate = 0,
                                    .finalSymbol = finalCharacter,
                                    .minimumParameters = 0,
                                    .maximumParameters = 0,
                                    .conformanceLevel = vt,
                                    .extension = VTExtension::None,
                                    .documentation =
                                        FunctionDocumentation { .mnemonic = mnemonic, .comment = comment } };
    }

    constexpr auto OSC(uint16_t code, VTExtension ext, FunctionDocumentation documentation) noexcept
    {
        return FunctionDefinition { .category = FunctionCategory::OSC,
                                    .leader = 0,
                                    .intermediate = 0,
                                    .finalSymbol = 0,
                                    .minimumParameters = 0,
                                    .maximumParameters = code,
                                    .conformanceLevel = VTType::VT100,
                                    .extension = ext,
                                    .documentation = documentation };
    }

    constexpr auto ESC(std::optional<char> intermediate,
                       char finalCharacter,
                       VTType vt,
                       FunctionDocumentation documentation) noexcept
    {
        return FunctionDefinition { .category = FunctionCategory::ESC,
                                    .leader = 0,
                                    .intermediate = intermediate.value_or(0),
                                    .finalSymbol = finalCharacter,
                                    .minimumParameters = 0,
                                    .maximumParameters = 0,
                                    .conformanceLevel = vt,
                                    .extension = VTExtension::None,
                                    .documentation = documentation };
    }

    constexpr auto CSI(std::optional<char> leader,
                       uint8_t argc0,
                       uint8_t argc1,
                       std::optional<char> intermediate,
                       char finalCharacter,
                       VTType vt,
                       FunctionDocumentation documentation) noexcept
    {
        // TODO: static_assert on leader/intermediate range-or-null
        return FunctionDefinition { .category = FunctionCategory::CSI,
                                    .leader = leader.value_or(0),
                                    .intermediate = intermediate.value_or(0),
                                    .finalSymbol = finalCharacter,
                                    .minimumParameters = argc0,
                                    .maximumParameters = argc1,
                                    .conformanceLevel = vt,
                                    .extension = VTExtension::None,
                                    .documentation = documentation };
    }

    constexpr auto CSI(std::optional<char> leader,
                       uint8_t argc0,
                       uint8_t argc1,
                       std::optional<char> intermediate,
                       char finalCharacter,
                       VTExtension ext,
                       FunctionDocumentation documentation) noexcept
    {
        // TODO: static_assert on leader/intermediate range-or-null
        return FunctionDefinition { .category = FunctionCategory::CSI,
                                    .leader = leader.value_or(0),
                                    .intermediate = intermediate.value_or(0),
                                    .finalSymbol = finalCharacter,
                                    .minimumParameters = argc0,
                                    .maximumParameters = argc1,
                                    .conformanceLevel = VTType::VT100,
                                    .extension = ext,
                                    .documentation = documentation };
    }

    constexpr auto DCS(std::optional<char> leader,
                       uint8_t argc0,
                       uint8_t argc1,
                       std::optional<char> intermediate,
                       char finalCharacter,
                       VTType vt,
                       FunctionDocumentation documentation) noexcept
    {
        // TODO: static_assert on leader/intermediate range-or-null
        return FunctionDefinition { .category = FunctionCategory::DCS,
                                    .leader = leader.value_or(0),
                                    .intermediate = intermediate.value_or(0),
                                    .finalSymbol = finalCharacter,
                                    .minimumParameters = argc0,
                                    .maximumParameters = argc1,
                                    .conformanceLevel = vt,
                                    .extension = VTExtension::None,
                                    .documentation = documentation };
    }

    constexpr auto DCS(std::optional<char> leader,
                       uint8_t argc0,
                       uint8_t argc1,
                       std::optional<char> intermediate,
                       char finalCharacter,
                       VTExtension ext,
                       FunctionDocumentation documentation) noexcept
    {
        // TODO: static_assert on leader/intermediate range-or-null
        return FunctionDefinition { .category = FunctionCategory::DCS,
                                    .leader = leader.value_or(0),
                                    .intermediate = intermediate.value_or(0),
                                    .finalSymbol = finalCharacter,
                                    .minimumParameters = argc0,
                                    .maximumParameters = argc1,
                                    .conformanceLevel = VTType::VT100,
                                    .extension = ext,
                                    .documentation = documentation };
    }
} // namespace detail
// }}}

// clang-format off

// C0
constexpr inline auto EOT = detail::C0('\x04', "EOT", "End of Transmission");
constexpr inline auto BEL = detail::C0('\x07', "BEL", "Bell");
constexpr inline auto BS  = detail::C0('\x08', "BS", "Backspace");
constexpr inline auto TAB = detail::C0('\x09', "TAB", "Tab");
constexpr inline auto LF  = detail::C0('\x0A', "LF", "Line Feed");
constexpr inline auto VT  = detail::C0('\x0B', "VT", "Vertical Tab"); // Even though VT means Vertical Tab, it seems that xterm is doing an IND instead.
constexpr inline auto FF  = detail::C0('\x0C', "FF", "Form Feed");
constexpr inline auto CR  = detail::C0('\x0D', "CR", "Carriage Return");
constexpr inline auto LS1 = detail::C0('\x0E', "LS1", "Shift Out; Maps G1 into GL.", VTType::VT220);
constexpr inline auto LS0 = detail::C0('\x0F', "LS0", "Shift In; Maps G0 into GL (the default).", VTType::VT220);

// SCS to support (G0, G1, G2, G3)
// A        UK (British), VT100
// B        USASCII, VT100
// 4        Dutch, VT200
// C
// S        Finnish, VT200
// R
// f        French, VT200
// Q
// 9        French Canadian, VT200
// K        VT200
// " >      Greek VT500
// % =      Hebrew VT500
// Y        Italian, VT200
// `
// E
// 6        Norwegian/Danish, VT200
// % 6      Portuguese, VT300
// Z        Spanish, VT200.
// H
// 7        Swedish, VT200.
// =        Swiss, VT200.
// % 2      Turkish, VT500.

// ESC functions
constexpr inline auto DECALN  = detail::ESC('#', '8', VTType::VT100, { .mnemonic = "DECALN", .comment = "Screen Alignment Pattern"});
constexpr inline auto DECBI   = detail::ESC(std::nullopt, '6', VTType::VT100, { .mnemonic = "DECBI", .comment = "Back Index"});
constexpr inline auto DECFI   = detail::ESC(std::nullopt, '9', VTType::VT100, { .mnemonic = "DECFI", .comment = "Forward Index"});
constexpr inline auto DECKPAM = detail::ESC(std::nullopt, '=', VTType::VT100, { .mnemonic = "DECKPAM", .comment = "Keypad Application Mode"});
constexpr inline auto DECKPNM = detail::ESC(std::nullopt, '>', VTType::VT100, { .mnemonic = "DECKPNM", .comment = "Keypad Numeric Mode"});
constexpr inline auto DECRS   = detail::ESC(std::nullopt, '8', VTType::VT100, { .mnemonic = "DECRS", .comment = "Restore Cursor"});
constexpr inline auto DECSC   = detail::ESC(std::nullopt, '7', VTType::VT100, { .mnemonic = "DECSC", .comment = "Save Cursor"});
constexpr inline auto HTS     = detail::ESC(std::nullopt, 'H', VTType::VT100, { .mnemonic = "HTS", .comment = "Horizontal Tab Set"});
constexpr inline auto IND     = detail::ESC(std::nullopt, 'D', VTType::VT100, { .mnemonic = "IND", .comment = "Index"});
constexpr inline auto NEL     = detail::ESC(std::nullopt, 'E', VTType::VT100, { .mnemonic = "NEL", .comment = "Next Line"});
constexpr inline auto RI      = detail::ESC(std::nullopt, 'M', VTType::VT100, { .mnemonic = "RI", .comment = "Reverse Index"});
constexpr inline auto RIS     = detail::ESC(std::nullopt, 'c', VTType::VT100, { .mnemonic = "RIS", .comment = "Reset to Initial State (Hard Reset)"});
constexpr inline auto SCS_G0_SPECIAL = detail::ESC('(', '0', VTType::VT100, { .mnemonic = "SCS_G0_SPECIAL", .comment = "Set G0 to DEC Special Character and Line Drawing Set" });// NOLINT
constexpr inline auto SCS_G0_USASCII = detail::ESC('(', 'B', VTType::VT100, { .mnemonic = "SCS_G0_USASCII", .comment = "Set G0 to USASCII" });// NOLINT
constexpr inline auto SCS_G1_SPECIAL = detail::ESC(')', '0', VTType::VT100, { .mnemonic = "SCS_G1_SPECIAL", .comment = "Set G1 to DEC Special Character and Line Drawing Set" });// NOLINT
constexpr inline auto SCS_G1_USASCII = detail::ESC(')', 'B', VTType::VT100, { .mnemonic = "SCS_G1_USASCII", .comment = "Set G1 to USASCII"});//NOLINT
constexpr inline auto SS2     = detail::ESC(std::nullopt, 'N', VTType::VT220, { .mnemonic = "SS2", .comment = "Single Shift Select (G2 Character Set)"});
constexpr inline auto SS3     = detail::ESC(std::nullopt, 'O', VTType::VT220, { .mnemonic = "SS3", .comment = "Single Shift Select (G3 Character Set)"});

// CSI
constexpr inline auto ArgsMax = 127; // this is the maximum number that fits into 7 bits.

// CSI functions
constexpr inline auto ANSIDSR     = detail::CSI(std::nullopt, 1, 1, std::nullopt, 'n', VTType::VT100, documentation::DSR);
constexpr inline auto ANSISYSSC   = detail::CSI(std::nullopt, 0, 0, std::nullopt, 'u', VTType::VT100, documentation::ANSISYSSC);
constexpr inline auto CBT         = detail::CSI(std::nullopt, 0, 1, std::nullopt, 'Z', VTType::VT100, documentation::CBT);
constexpr inline auto CHA         = detail::CSI(std::nullopt, 0, 1, std::nullopt, 'G', VTType::VT100, documentation::CHA);
constexpr inline auto CHT         = detail::CSI(std::nullopt, 0, 1, std::nullopt, 'I', VTType::VT100, documentation::CHT);
constexpr inline auto CNL         = detail::CSI(std::nullopt, 0, 1, std::nullopt, 'E', VTType::VT100, documentation::CNL);
constexpr inline auto CPL         = detail::CSI(std::nullopt, 0, 1, std::nullopt, 'F', VTType::VT100, documentation::CPL);
constexpr inline auto CSIUENHCE   = detail::CSI('=', 1, 2, std::nullopt, 'u', VTExtension::Unknown, documentation::CSIUENHCE);
constexpr inline auto CSIUENTER   = detail::CSI('>', 0, 1, std::nullopt, 'u', VTExtension::Unknown, documentation::CSIUENTER);
constexpr inline auto CSIULEAVE   = detail::CSI('<', 0, 1, std::nullopt, 'u', VTExtension::Unknown, documentation::CSIULEAVE);
constexpr inline auto CSIUQUERY   = detail::CSI('?', 0, 0, std::nullopt, 'u', VTExtension::Unknown, documentation::CSIUQUERY);
constexpr inline auto CUB         = detail::CSI(std::nullopt, 0, 1, std::nullopt, 'D', VTType::VT100, documentation::CUB);
constexpr inline auto CUD         = detail::CSI(std::nullopt, 0, 1, std::nullopt, 'B', VTType::VT100, documentation::CUD);
constexpr inline auto CUF         = detail::CSI(std::nullopt, 0, 1, std::nullopt, 'C', VTType::VT100, documentation::CUF);
constexpr inline auto CUP         = detail::CSI(std::nullopt, 0, 2, std::nullopt, 'H', VTType::VT100, documentation::CUP);
constexpr inline auto CUU         = detail::CSI(std::nullopt, 0, 1, std::nullopt, 'A', VTType::VT100, documentation::CUU);
constexpr inline auto DA1         = detail::CSI(std::nullopt, 0, 1, std::nullopt, 'c', VTType::VT100, documentation::DA1);
constexpr inline auto DA2         = detail::CSI('>', 0, 1, std::nullopt, 'c', VTType::VT100, documentation::DA2);
constexpr inline auto DA3         = detail::CSI('=', 0, 1, std::nullopt, 'c', VTType::VT100, documentation::DA3);
constexpr inline auto DCH         = detail::CSI(std::nullopt, 0, 1, std::nullopt, 'P', VTType::VT100, documentation::DCH);
constexpr inline auto DECCARA     = detail::CSI(std::nullopt, 5, ArgsMax, '$', 'r', VTType::VT420, documentation::DECCARA);
constexpr inline auto DECCRA      = detail::CSI(std::nullopt, 0, 8, '$', 'v', VTType::VT420, documentation::DECCRA);
constexpr inline auto DECDC       = detail::CSI(std::nullopt, 0, 1, '\'', '~', VTType::VT420, documentation::DECDC);
constexpr inline auto DECERA      = detail::CSI(std::nullopt, 0, 4, '$', 'z', VTType::VT420, documentation::DECERA);
constexpr inline auto DECFRA      = detail::CSI(std::nullopt, 0, 5, '$', 'x', VTType::VT420, documentation::DECFRA);
constexpr inline auto DECIC       = detail::CSI(std::nullopt, 0, 1, '\'', '}', VTType::VT420, documentation::DECIC);
constexpr inline auto DECPS       = detail::CSI(std::nullopt, 3, 18, ',', '~', VTType::VT520, documentation::DECPS);
constexpr inline auto DECRM       = detail::CSI('?', 1, ArgsMax, std::nullopt, 'l', VTType::VT100, documentation::DECRM);
constexpr inline auto DECRQM      = detail::CSI('?', 1, 1, '$', 'p', VTType::VT100, documentation::DECRQM);
constexpr inline auto DECRQM_ANSI = detail::CSI(std::nullopt, 1, 1, '$', 'p', VTType::VT100, documentation::DECRQM_ANSI);// NOLINT
constexpr inline auto DECRQPSR    = detail::CSI(std::nullopt, 1, 1, '$', 'w', VTType::VT320, documentation::DECRQPSR);
constexpr inline auto DECSASD     = detail::CSI(std::nullopt, 0, 1, '$', '}', VTType::VT420, documentation::DECSASD);
constexpr inline auto DECSCA      = detail::CSI(std::nullopt, 0, 1, '"', 'q', VTType::VT240, documentation::DECSCA);
constexpr inline auto DECSCL      = detail::CSI(std::nullopt, 2, 2, '"', 'p', VTType::VT220, documentation::DECSCL);
constexpr inline auto DECSCPP     = detail::CSI(std::nullopt, 0, 1, '$', '|', VTType::VT100, documentation::DECSCPP);
constexpr inline auto DECSCUSR    = detail::CSI(std::nullopt, 0, 1, ' ', 'q', VTType::VT520, documentation::DECSCUSR);
constexpr inline auto DECSED      = detail::CSI('?', 0, 1, std::nullopt, 'J', VTType::VT240, documentation::DECSED);
constexpr inline auto DECSEL      = detail::CSI('?', 0, 1, std::nullopt, 'K', VTType::VT240, documentation::DECSEL);
constexpr inline auto DECSERA     = detail::CSI(std::nullopt, 0, 4, '$', '{', VTType::VT240, documentation::DECSERA);
constexpr inline auto DECSLRM     = detail::CSI(std::nullopt, 0, 2, std::nullopt, 's', VTType::VT420, documentation::DECSLRM);
constexpr inline auto DECSM       = detail::CSI('?', 1, ArgsMax, std::nullopt, 'h', VTType::VT100, documentation::DECSM);
constexpr inline auto DECSNLS     = detail::CSI(std::nullopt, 0, 1, '*', '|', VTType::VT420, documentation::DECSNLS);
constexpr inline auto DECSSCLS    = detail::CSI(std::nullopt, 0, 1, ' ', 'p', VTType::VT510, documentation::DECSSCLS);
constexpr inline auto DECSSDT     = detail::CSI(std::nullopt, 0, 1, '$', '~', VTType::VT320, documentation::DECSSDT);
constexpr inline auto DECSTBM     = detail::CSI(std::nullopt, 0, 2, std::nullopt, 'r', VTType::VT100, documentation::DECSTBM);
constexpr inline auto DECSTR      = detail::CSI(std::nullopt, 0, 0, '!', 'p', VTType::VT100, documentation::DECSTR);
constexpr inline auto DECXCPR     = detail::CSI(std::nullopt, 0, 0, std::nullopt, '6', VTType::VT100, documentation::DECXCPR);
constexpr inline auto DL          = detail::CSI(std::nullopt, 0, 1, std::nullopt, 'M', VTType::VT100, documentation::DL);
constexpr inline auto DSR         = detail::CSI('?', 1, 1, std::nullopt, 'n', VTType::VT100, documentation::DSR);
constexpr inline auto ECH         = detail::CSI(std::nullopt, 0, 1, std::nullopt, 'X', VTType::VT420, documentation::ECH);
constexpr inline auto ED          = detail::CSI(std::nullopt, 0, ArgsMax, std::nullopt, 'J', VTType::VT100, documentation::ED);
constexpr inline auto EL          = detail::CSI(std::nullopt, 0, 1, std::nullopt, 'K', VTType::VT100, documentation::EL);
constexpr inline auto HPA         = detail::CSI(std::nullopt, 1, 1, std::nullopt, '`', VTType::VT100, documentation::HPA);
constexpr inline auto HPR         = detail::CSI(std::nullopt, 1, 1, std::nullopt, 'a', VTType::VT100, documentation::HPR);
constexpr inline auto HVP         = detail::CSI(std::nullopt, 0, 2, std::nullopt, 'f', VTType::VT100, documentation::HVP);
constexpr inline auto ICH         = detail::CSI(std::nullopt, 0, 1, std::nullopt, '@', VTType::VT420, documentation::ICH);
constexpr inline auto IL          = detail::CSI(std::nullopt, 0, 1, std::nullopt, 'L', VTType::VT100, documentation::IL);
constexpr inline auto REP         = detail::CSI(std::nullopt, 1, 1, std::nullopt, 'b', VTType::VT100, documentation::REP);
constexpr inline auto RM          = detail::CSI(std::nullopt, 1, ArgsMax, std::nullopt, 'l', VTType::VT100, documentation::RM);
constexpr inline auto SCOSC       = detail::CSI(std::nullopt, 0, 0, std::nullopt, 's', VTType::VT100, documentation::SCOSC);
constexpr inline auto SD          = detail::CSI(std::nullopt, 0, 1, std::nullopt, 'T', VTType::VT100, documentation::SD);
constexpr inline auto SETMARK     = detail::CSI('>', 0, 0, std::nullopt, 'M', VTExtension::Contour, documentation::SETMARK);
constexpr inline auto SGR         = detail::CSI(std::nullopt, 0, ArgsMax, std::nullopt, 'm', VTType::VT100, documentation::SGR);
constexpr inline auto SM          = detail::CSI(std::nullopt, 1, ArgsMax, std::nullopt, 'h', VTType::VT100, documentation::SM);
constexpr inline auto SU          = detail::CSI(std::nullopt, 0, 1, std::nullopt, 'S', VTType::VT100, documentation::SU);
constexpr inline auto TBC         = detail::CSI(std::nullopt, 0, 1, std::nullopt, 'g', VTType::VT100, documentation::TBC);
constexpr inline auto VPA         = detail::CSI(std::nullopt, 0, 1, std::nullopt, 'd', VTType::VT100, documentation::VPA);
constexpr inline auto WINMANIP    = detail::CSI(std::nullopt, 1, 3, std::nullopt, 't', VTExtension::XTerm, documentation::WINMANIP);
constexpr inline auto XTCAPTURE   = detail::CSI('>', 0, 2, std::nullopt, 't', VTExtension::Contour, documentation::XTCAPTURE);
constexpr inline auto XTPOPCOLORS    = detail::CSI(std::nullopt, 0, ArgsMax, '#', 'Q', VTExtension::XTerm, documentation::XTPOPCOLORS);
constexpr inline auto XTPUSHCOLORS   = detail::CSI(std::nullopt, 0, ArgsMax, '#', 'P', VTExtension::XTerm, documentation::XTPUSHCOLORS);
constexpr inline auto XTREPORTCOLORS = detail::CSI(std::nullopt, 0, 0, '#', 'R', VTExtension::XTerm, documentation::XTREPORTCOLORS);
constexpr inline auto XTRESTORE   = detail::CSI('?', 0, ArgsMax, std::nullopt, 'r', VTExtension::XTerm, documentation::XTRESTORE);
constexpr inline auto XTSAVE      = detail::CSI('?', 0, ArgsMax, std::nullopt, 's', VTExtension::XTerm, documentation::XTSAVE);
constexpr inline auto XTSHIFTESCAPE=detail::CSI('>', 0, 1, std::nullopt, 's', VTExtension::XTerm, documentation::XTSHIFTESCAPE);
constexpr inline auto XTSMGRAPHICS= detail::CSI('?', 2, 4, std::nullopt, 'S', VTExtension::XTerm, documentation::XTSMGRAPHICS);
constexpr inline auto XTVERSION   = detail::CSI('>', 0, 1, std::nullopt, 'q', VTExtension::XTerm, documentation::XTVERSION);

// DCS functions
constexpr inline auto DECRQSS     = detail::DCS(std::nullopt, 0, 0, '$', 'q', VTType::VT420, documentation::DECRQSS);
constexpr inline auto DECSIXEL    = detail::DCS(std::nullopt, 0, 3, std::nullopt, 'q', VTType::VT330, documentation::DECSIXEL);
constexpr inline auto STP         = detail::DCS(std::nullopt, 0, 0, '$', 'p', VTExtension::Contour, documentation::STP);
constexpr inline auto XTGETTCAP   = detail::DCS(std::nullopt, 0, 0, '+', 'q', VTExtension::XTerm, documentation::XTGETTCAP);

// OSC
constexpr inline auto CLIPBOARD         = detail::OSC(52, VTExtension::XTerm, documentation::CLIPBOARD);
constexpr inline auto COLORBG           = detail::OSC(11, VTExtension::XTerm, documentation::COLORBG);
constexpr inline auto COLORCURSOR       = detail::OSC(12, VTExtension::XTerm, documentation::COLORCURSOR);
constexpr inline auto COLORFG           = detail::OSC(10, VTExtension::XTerm, documentation::COLORFG);
constexpr inline auto COLORMOUSEBG      = detail::OSC(14, VTExtension::XTerm, documentation::COLORMOUSEBG);
constexpr inline auto COLORMOUSEFG      = detail::OSC(13, VTExtension::XTerm, documentation::COLORMOUSEFG);
constexpr inline auto COLORSPECIAL      = detail::OSC(106, VTExtension::XTerm, documentation::COLORSPECIAL);
constexpr inline auto DUMPSTATE         = detail::OSC(888, VTExtension::Contour, documentation::DUMPSTATE);
constexpr inline auto HYPERLINK         = detail::OSC(8, VTExtension::Unknown, documentation::HYPERLINK);
constexpr inline auto NOTIFY            = detail::OSC(777, VTExtension::XTerm, documentation::NOTIFY);
constexpr inline auto RCOLORBG          = detail::OSC(111, VTExtension::XTerm, documentation::RCOLORBG);
constexpr inline auto RCOLORCURSOR      = detail::OSC(112, VTExtension::XTerm, documentation::RCOLORCURSOR);
constexpr inline auto RCOLORFG          = detail::OSC(110, VTExtension::XTerm, documentation::RCOLORFG);
constexpr inline auto RCOLORHIGHLIGHTBG = detail::OSC(117, VTExtension::XTerm, documentation::RCOLORHIGHLIGHTBG);
constexpr inline auto RCOLORHIGHLIGHTFG = detail::OSC(119, VTExtension::XTerm, documentation::RCOLORHIGHLIGHTFG);
constexpr inline auto RCOLORMOUSEBG     = detail::OSC(114, VTExtension::XTerm, documentation::RCOLORMOUSEBG);
constexpr inline auto RCOLORMOUSEFG     = detail::OSC(113, VTExtension::XTerm, documentation::RCOLORMOUSEFG);
constexpr inline auto RCOLPAL           = detail::OSC(104, VTExtension::XTerm, documentation::RCOLPAL);
constexpr inline auto SETCOLPAL         = detail::OSC(4, VTExtension::XTerm, documentation::SETCOLPAL);
constexpr inline auto SETCWD            = detail::OSC(7, VTExtension::XTerm, documentation::SETCWD);
constexpr inline auto SETFONT           = detail::OSC(50, VTExtension::XTerm, documentation::SETFONT);
constexpr inline auto SETFONTALL        = detail::OSC(60, VTExtension::Contour, documentation::SETFONTALL);
constexpr inline auto SETICON           = detail::OSC(1, VTExtension::XTerm, documentation::SETICON);
constexpr inline auto SETTITLE          = detail::OSC(0, VTExtension::XTerm, documentation::SETTITLE);
constexpr inline auto SETWINTITLE       = detail::OSC(2, VTExtension::XTerm, documentation::SETWINTITLE);
constexpr inline auto SETXPROP          = detail::OSC(3, VTExtension::XTerm, documentation::SETXPROP);

constexpr inline auto CaptureBufferCode = 314;

// clang-format on

// HACK to get older compiler work (GCC 9.4)
constexpr static auto allFunctionsArray() noexcept
{
    auto funcs = std::array {
        // C0
        EOT,
        BEL,
        BS,
        TAB,
        LF,
        VT,
        FF,
        CR,
        LS0,
        LS1,

        // ESC
        DECALN,
        DECBI,
        DECFI,
        DECKPAM,
        DECKPNM,
        DECRS,
        DECSC,
        HTS,
        IND,
        NEL,
        RI,
        RIS,
        SCS_G0_SPECIAL,
        SCS_G0_USASCII,
        SCS_G1_SPECIAL,
        SCS_G1_USASCII,
        SS2,
        SS3,

        // CSI
        ANSISYSSC,
        XTCAPTURE,
        CBT,
        CHA,
        CHT,
        CNL,
        CPL,
        CUB,
        CUD,
        CUF,
        CUP,
        CUU,
        DA1,
        DA2,
        DA3,
        DCH,
        DECCARA,
        DECCRA,
        DECDC,
        DECERA,
        DECFRA,
        DECIC,
        DECSCA,
        DECSED,
        DECSERA,
        DECSEL,
        ANSIDSR,
        DSR,
        XTRESTORE,
        XTSAVE,
        DECPS,
        CSIUENTER,
        CSIUQUERY,
        CSIUENHCE,
        CSIULEAVE,
        DECRM,
        DECRQM,
        DECRQM_ANSI,
        DECRQPSR,
        DECSASD,
        DECSCL,
        DECSCPP,
        DECSCUSR,
        DECSLRM,
        DECSSCLS,
        DECSM,
        DECSNLS,
        DECSSDT,
        DECSTBM,
        DECSTR,
        DECXCPR,
        DL,
        ECH,
        ED,
        EL,
        HPA,
        HPR,
        HVP,
        ICH,
        IL,
        REP,
        RM,
        SCOSC,
        SD,
        SETMARK,
        SGR,
        SM,
        SU,
        TBC,
        VPA,
        WINMANIP,
        XTPOPCOLORS,
        XTPUSHCOLORS,
        XTREPORTCOLORS,
        XTSHIFTESCAPE,
        XTSMGRAPHICS,
        XTVERSION,

        // DCS
        STP,
        DECRQSS,
        DECSIXEL,
        XTGETTCAP,

        // OSC
        SETICON,
        SETTITLE,
        SETWINTITLE,
        SETXPROP,
        SETCOLPAL,
        SETCWD,
        HYPERLINK,
        COLORFG,
        COLORBG,
        COLORCURSOR,
        COLORMOUSEFG,
        COLORMOUSEBG,
        SETFONT,
        SETFONTALL,
        CLIPBOARD,
        RCOLPAL,
        COLORSPECIAL,
        RCOLORFG,
        RCOLORBG,
        RCOLORCURSOR,
        RCOLORMOUSEFG,
        RCOLORMOUSEBG,
        RCOLORHIGHLIGHTFG,
        RCOLORHIGHLIGHTBG,
        NOTIFY,
        DUMPSTATE,
    };
    return funcs;
}

inline auto allFunctions() noexcept
{
    static auto const funcs = []() constexpr {
        auto funcs = allFunctionsArray();
        crispy::sort(funcs, [](FunctionDefinition const& a, FunctionDefinition const& b) constexpr {
            return compare(a, b);
        });
        return funcs;
    }();

    return funcs;
}

// Class to store all supported VT sequence and support properly enabling/disabling them
// The storage stores all available definition at all time and is partitioned into
// two parts first part contains all active sequences and last part contains all
// disabled sequences
class SupportedSequences
{

  private:
    [[nodiscard]] constexpr auto begin() noexcept { return _supportedSequences.data(); }

    [[nodiscard]] constexpr auto end() noexcept { return begin() + _lastIndex; }

    [[nodiscard]] constexpr auto cbegin() const noexcept { return _supportedSequences.data(); }

    [[nodiscard]] constexpr auto cend() const noexcept { return cbegin() + _lastIndex; }

  public:
    [[nodiscard]] constexpr gsl::span<FunctionDefinition const> allSequences() const noexcept
    {
        return gsl::span<FunctionDefinition const>(cbegin(), _supportedSequences.size());
    }

    [[nodiscard]] constexpr gsl::span<FunctionDefinition const> activeSequences() const noexcept
    {
        return gsl::span<FunctionDefinition const>(cbegin(), _lastIndex);
    }

    CRISPY_CONSTEXPR void reset(VTType vt) noexcept
    {
        // Partition the array such that first half contains all sequences with VTType less than or
        // equal to given VTTYpe.
        auto* itr = std::partition(
            begin(),
            _supportedSequences.data() + _supportedSequences.size(),
            [vt](const FunctionDefinition& value) noexcept { return value.conformanceLevel <= vt; });

        _lastIndex = std::distance(begin(), itr);
        gsl::span<FunctionDefinition> availableDefinition(begin(), _lastIndex);
        crispy::sort(
            availableDefinition,
            [](FunctionDefinition const& a, FunctionDefinition const& b) constexpr { return compare(a, b); });
    }

    CRISPY_CONSTEXPR void disableSequence(FunctionDefinition seq) noexcept
    {
        auto* seqIter = std::find(begin(), end(), seq);
        if (seqIter != end())
        {
            // Move the disabled sequence to the end of array, keep the rest of active sequences sorted
            std::rotate(seqIter, seqIter + 1, _supportedSequences.data() + _supportedSequences.size());
            --_lastIndex;
        }
    }

    CRISPY_CONSTEXPR void enableSequence(FunctionDefinition seq) noexcept
    {
        auto* const endArray = _supportedSequences.data() + _supportedSequences.size();
        auto* seqIter = std::find(end(), endArray, seq);
        if (seqIter != endArray)
        {
            // Maybe could be done better since rest of the data is sorted
            std::iter_swap(end(), seqIter);
            ++_lastIndex;
            gsl::span<FunctionDefinition> arr(begin(), end());
            crispy::sort(arr, [](FunctionDefinition const& a, FunctionDefinition const& b) constexpr {
                return compare(a, b);
            });
        }
    }

  private:
    std::array<FunctionDefinition, allFunctionsArray().size()> _supportedSequences = allFunctions();
    size_t _lastIndex = allFunctions().size(); // No of total active sequences
};

/// Selects a FunctionDefinition based on a FunctionSelector.
///
/// @return the matching FunctionDefinition or nullptr if none matched.
FunctionDefinition const* select(FunctionSelector const& selector,
                                 gsl::span<FunctionDefinition const> availableDefinition) noexcept;

/// Selects a FunctionDefinition based on given input Escape sequence fields.
///
/// @p intermediate an optional intermediate character between (0x20 .. 0x2F)
/// @p finalCharacter between 0x40 .. 0x7F
///
/// @notice multi-character intermediates are intentionally not supported.
///
/// @return the matching FunctionDefinition or nullptr if none matched.
inline FunctionDefinition const* selectEscape(char intermediate,
                                              char finalCharacter,
                                              gsl::span<FunctionDefinition const> availableDefinition)
{
    return select({ FunctionCategory::ESC, 0, 0, intermediate, finalCharacter }, availableDefinition);
}

/// Selects a FunctionDefinition based on given input control sequence fields.
///
/// @p leader an optional value between 0x3C .. 0x3F
/// @p argc number of arguments supplied
/// @p intermediate an optional intermediate character between (0x20 .. 0x2F)
/// @p finalCharacter between 0x40 .. 0x7F
///
/// @notice multi-character intermediates are intentionally not supported.
///
/// @return the matching FunctionDefinition or nullptr if none matched.
inline FunctionDefinition const* selectControl(char leader,
                                               int argc,
                                               char intermediate,
                                               char finalCharacter,
                                               gsl::span<FunctionDefinition const> availableDefinition)
{
    return select({ FunctionCategory::CSI, leader, argc, intermediate, finalCharacter }, availableDefinition);
}

/// Selects a FunctionDefinition based on given input control sequence fields.
///
/// @p id leading numeric identifier (such as 8 for hyperlink)
///
/// @notice multi-character intermediates are intentionally not supported.
///
/// @return the matching FunctionDefinition or nullptr if none matched.
inline FunctionDefinition const* selectOSCommand(int id,
                                                 gsl::span<FunctionDefinition const> availableDefinition)
{
    return select({ FunctionCategory::OSC, 0, id, 0, 0 }, availableDefinition);
}

} // namespace vtbackend

template <>
struct std::hash<vtbackend::FunctionDefinition>
{
    /// This is actually perfect hashing.
    constexpr uint32_t operator()(vtbackend::FunctionDefinition const& fun) const noexcept
    {
        return fun.id();
    }
};

// {{{ fmtlib support
template <>
struct fmt::formatter<vtbackend::FunctionCategory>: fmt::formatter<std::string_view>
{
    auto format(const vtbackend::FunctionCategory value, format_context& ctx) -> format_context::iterator
    {
        using vtbackend::FunctionCategory;
        string_view name;
        switch (value)
        {
            case FunctionCategory::C0:
                name = "C0";
                break;
                ;
            case FunctionCategory::ESC:
                name = "ESC";
                break;
                ;
            case FunctionCategory::CSI:
                name = "CSI";
                break;
                ;
            case FunctionCategory::OSC:
                name = "OSC";
                break;
                ;
            case FunctionCategory::DCS:
                name = "DCS";
                break;
                ;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct fmt::formatter<vtbackend::FunctionDefinition>: fmt::formatter<std::string>
{
    auto format(const vtbackend::FunctionDefinition f, format_context& ctx) -> format_context::iterator
    {
        std::string value;
        switch (f.category)
        {
            case vtbackend::FunctionCategory::C0:
                value = fmt::format("{}", crispy::escape(static_cast<uint8_t>(f.finalSymbol)));
                break;
            case vtbackend::FunctionCategory::ESC:
                value = fmt::format("{} {} {}",
                                    f.category,
                                    f.intermediate ? f.intermediate : ' ',
                                    f.finalSymbol ? f.finalSymbol : ' ');
                break;
            case vtbackend::FunctionCategory::OSC:
                value = fmt::format("{} {}", f.category, f.maximumParameters);
                break;
            case vtbackend::FunctionCategory::DCS:
            case vtbackend::FunctionCategory::CSI:
                if (f.minimumParameters == f.maximumParameters)
                    value = fmt::format("{} {} {}    {} {}",
                                        f.category,
                                        f.leader ? f.leader : ' ',
                                        f.minimumParameters,
                                        f.intermediate ? f.intermediate : ' ',
                                        f.finalSymbol);
                else if (f.maximumParameters == vtbackend::ArgsMax)
                    value = fmt::format("{} {} {}..  {} {}",
                                        f.category,
                                        f.leader ? f.leader : ' ',
                                        f.minimumParameters,
                                        f.intermediate ? f.intermediate : ' ',
                                        f.finalSymbol);
                else
                    value = fmt::format("{} {} {}..{} {} {}",
                                        f.category,
                                        f.leader ? f.leader : ' ',
                                        f.minimumParameters,
                                        f.maximumParameters,
                                        f.intermediate ? f.intermediate : ' ',
                                        f.finalSymbol);
                break;
        }
        return formatter<std::string>::format(value, ctx);
    }
};

template <>
struct fmt::formatter<vtbackend::FunctionSelector>: fmt::formatter<std::string>
{
    auto format(const vtbackend::FunctionSelector f, format_context& ctx) -> format_context::iterator
    {
        std::string value;
        // clang-format off
        switch (f.category)
        {
            case vtbackend::FunctionCategory::OSC:
                value = fmt::format("{} {}", f.category, f.argc);
                break;
            default:
                value = fmt::format("{} {} {} {} {}",
                                    f.category,
                                    f.leader ? f.leader : ' ',
                                    f.argc,
                                    f.intermediate ? f.intermediate : ' ',
                                    f.finalSymbol ? f.finalSymbol : ' ');
                break;
        }
        // clang-format on
        return formatter<std::string>::format(value, ctx);
    }
};
// }}}
