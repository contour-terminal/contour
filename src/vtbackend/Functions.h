/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <vtbackend/VTType.h>

#include <crispy/escape.h>
#include <crispy/sort.h>

#include <fmt/format.h>

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace terminal
{

enum class function_category : uint8_t
{
    C0 = 0,
    ESC = 1,
    CSI = 2,
    OSC = 3,
    DCS = 4,
};

/// Defines a function with all its syntax requirements plus some additional meta information.
struct function_definition // TODO: rename Function
{
    function_category category; // (3 bits) C0, ESC, CSI, OSC, DCS
    char leader;                // (3 bits) 0x3C..0x3F (one of: < = > ?, or 0x00 for none)
    char intermediate;          // (4 bits) 0x20..0x2F (intermediates, usually just one, or 0x00 if none)
    char finalSymbol;           // (7 bits) 0x30..0x7E (final character)
    uint8_t minimumParameters;  // (4 bits) 0..7
    uint16_t maximumParameters; // (10 bits) 0..1024 for integer value (OSC function parameter)

    // Conformance level and extension are mutually exclusive.
    // But it is unclear to me whether or not it is guaranteed to always have a constexpr-aware std::variant.
    // So keep it the classic way (for now).
    vt_type conformanceLevel;
    vt_extension extension = vt_extension::None;

    std::string_view mnemonic;
    std::string_view comment;

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

constexpr int compare(function_definition const& a, function_definition const& b)
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
constexpr bool operator==(function_definition const& a, function_definition const& b) noexcept { return compare(a, b) == 0; }
constexpr bool operator!=(function_definition const& a, function_definition const& b) noexcept { return compare(a, b) != 0; }
constexpr bool operator<=(function_definition const& a, function_definition const& b) noexcept { return compare(a, b) <= 0; }
constexpr bool operator>=(function_definition const& a, function_definition const& b) noexcept { return compare(a, b) >= 0; }
constexpr bool operator<(function_definition const& a, function_definition const& b) noexcept { return compare(a, b) < 0; }
constexpr bool operator>(function_definition const& a, function_definition const& b) noexcept { return compare(a, b) > 0; }
// clang-format on

struct function_selector
{
    /// represents the corresponding function category.
    function_category category;
    /// an optional value between 0x3C .. 0x3F
    char leader;
    /// number of arguments supplied
    int argc;
    /// an optional intermediate character between (0x20 .. 0x2F)
    char intermediate;
    /// between 0x40 .. 0x7F
    char finalSymbol;
};

constexpr int compare(function_selector const& a, function_definition const& b) noexcept
{
    if (a.category != b.category)
        return static_cast<int>(a.category) - static_cast<int>(b.category);

    if (a.finalSymbol != b.finalSymbol)
        return a.finalSymbol - b.finalSymbol;

    if (a.leader != b.leader)
        return a.leader - b.leader;

    if (a.intermediate != b.intermediate)
        return a.intermediate - b.intermediate;

    if (a.category == function_category::OSC)
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
                      std::string_view description,
                      vt_type vt = vt_type::VT100) noexcept
    {
        // clang-format off
        return function_definition { function_category::C0, 0, 0, finalCharacter, 0, 0, vt,
                                    vt_extension::None, mnemonic, description };
        // clang-format on
    }

    constexpr auto OSC(uint16_t code,
                       vt_extension ext,
                       std::string_view mnemonic,
                       std::string_view description) noexcept
    {
        // clang-format off
        return function_definition { function_category::OSC, 0, 0, 0, 0, code,
                                    vt_type::VT100,
                                    ext,
                                    mnemonic,
                                    description };
        // clang-format on
    }

    constexpr auto ESC(std::optional<char> intermediate,
                       char finalCharacter,
                       vt_type vt,
                       std::string_view mnemonic,
                       std::string_view description) noexcept
    {
        return function_definition { function_category::ESC,
                                     0,
                                     intermediate.value_or(0),
                                     finalCharacter,
                                     0,
                                     0,
                                     vt,
                                     vt_extension::None,
                                     mnemonic,
                                     description };
    }

    constexpr auto CSI(std::optional<char> leader,
                       uint8_t argc0,
                       uint8_t argc1,
                       std::optional<char> intermediate,
                       char finalCharacter,
                       vt_type vt,
                       std::string_view mnemonic,
                       std::string_view description) noexcept
    {
        // TODO: static_assert on leader/intermediate range-or-null
        return function_definition { function_category::CSI,
                                     leader.value_or(0),
                                     intermediate.value_or(0),
                                     finalCharacter,
                                     argc0,
                                     argc1,
                                     vt,
                                     vt_extension::None,
                                     mnemonic,
                                     description };
    }

    constexpr auto CSI(std::optional<char> leader,
                       uint8_t argc0,
                       uint8_t argc1,
                       std::optional<char> intermediate,
                       char finalCharacter,
                       vt_extension ext,
                       std::string_view mnemonic,
                       std::string_view description) noexcept
    {
        // TODO: static_assert on leader/intermediate range-or-null
        return function_definition { function_category::CSI,
                                     leader.value_or(0),
                                     intermediate.value_or(0),
                                     finalCharacter,
                                     argc0,
                                     argc1,
                                     vt_type::VT100,
                                     ext,
                                     mnemonic,
                                     description };
    }

    constexpr auto DCS(std::optional<char> leader,
                       uint8_t argc0,
                       uint8_t argc1,
                       std::optional<char> intermediate,
                       char finalCharacter,
                       vt_type vt,
                       std::string_view mnemonic,
                       std::string_view description) noexcept
    {
        // TODO: static_assert on leader/intermediate range-or-null
        return function_definition { function_category::DCS,
                                     leader.value_or(0),
                                     intermediate.value_or(0),
                                     finalCharacter,
                                     argc0,
                                     argc1,
                                     vt,
                                     vt_extension::None,
                                     mnemonic,
                                     description };
    }

    constexpr auto DCS(std::optional<char> leader,
                       uint8_t argc0,
                       uint8_t argc1,
                       std::optional<char> intermediate,
                       char finalCharacter,
                       vt_extension ext,
                       std::string_view mnemonic,
                       std::string_view description) noexcept
    {
        // TODO: static_assert on leader/intermediate range-or-null
        return function_definition { function_category::DCS,
                                     leader.value_or(0),
                                     intermediate.value_or(0),
                                     finalCharacter,
                                     argc0,
                                     argc1,
                                     vt_type::VT100,
                                     ext,
                                     mnemonic,
                                     description };
    }
} // namespace detail

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
constexpr inline auto LS1 = detail::C0('\x0E', "LS1", "Shift Out; Maps G1 into GL.", vt_type::VT220);
constexpr inline auto LS0 = detail::C0('\x0F', "LS0", "Shift In; Maps G0 into GL (the default).", vt_type::VT220);

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
constexpr inline auto SCS_G0_SPECIAL = detail::ESC('(', '0', vt_type::VT100, "SCS_G0_SPECIAL", "Set G0 to DEC Special Character and Line Drawing Set");
constexpr inline auto SCS_G0_USASCII = detail::ESC('(', 'B', vt_type::VT100, "SCS_G0_USASCII", "Set G0 to USASCII");
constexpr inline auto SCS_G1_SPECIAL = detail::ESC(')', '0', vt_type::VT100, "SCS_G1_SPECIAL", "Set G1 to DEC Special Character and Line Drawing Set");
constexpr inline auto SCS_G1_USASCII = detail::ESC(')', 'B', vt_type::VT100, "SCS_G1_USASCII", "Set G1 to USASCII");
constexpr inline auto DECALN  = detail::ESC('#', '8', vt_type::VT100, "DECALN", "Screen Alignment Pattern");
constexpr inline auto DECBI   = detail::ESC(std::nullopt, '6', vt_type::VT100, "DECBI", "Back Index");
constexpr inline auto DECFI   = detail::ESC(std::nullopt, '9', vt_type::VT100, "DECFI", "Forward Index");
constexpr inline auto DECKPAM = detail::ESC(std::nullopt, '=', vt_type::VT100, "DECKPAM", "Keypad Application Mode");
constexpr inline auto DECKPNM = detail::ESC(std::nullopt, '>', vt_type::VT100, "DECKPNM", "Keypad Numeric Mode");
constexpr inline auto DECRS   = detail::ESC(std::nullopt, '8', vt_type::VT100, "DECRS", "Restore Cursor");
constexpr inline auto DECSC   = detail::ESC(std::nullopt, '7', vt_type::VT100, "DECSC", "Save Cursor");
constexpr inline auto HTS     = detail::ESC(std::nullopt, 'H', vt_type::VT100, "HTS", "Horizontal Tab Set");
constexpr inline auto IND     = detail::ESC(std::nullopt, 'D', vt_type::VT100, "IND", "Index");
constexpr inline auto NEL     = detail::ESC(std::nullopt, 'E', vt_type::VT100, "NEL", "Next Line");
constexpr inline auto RI      = detail::ESC(std::nullopt, 'M', vt_type::VT100, "RI", "Reverse Index");
constexpr inline auto RIS     = detail::ESC(std::nullopt, 'c', vt_type::VT100, "RIS", "Reset to Initial State (Hard Reset)");
constexpr inline auto SS2     = detail::ESC(std::nullopt, 'N', vt_type::VT220, "SS2", "Single Shift Select (G2 Character Set)");
constexpr inline auto SS3     = detail::ESC(std::nullopt, 'O', vt_type::VT220, "SS3", "Single Shift Select (G3 Character Set)");

// CSI
constexpr inline auto ArgsMax = 127; // this is the maximum number that fits into 7 bits.

// CSI functions
constexpr inline auto ANSISYSSC   = detail::CSI(std::nullopt, 0, 0, std::nullopt, 'u', vt_type::VT100, "ANSISYSSC", "Save Cursor (ANSI.SYS)");
constexpr inline auto CBT         = detail::CSI(std::nullopt, 0, 1, std::nullopt, 'Z', vt_type::VT100, "CBT", "Cursor Backward Tabulation");
constexpr inline auto CHA         = detail::CSI(std::nullopt, 0, 1, std::nullopt, 'G', vt_type::VT100, "CHA", "Move cursor to column");
constexpr inline auto CHT         = detail::CSI(std::nullopt, 0, 1, std::nullopt, 'I', vt_type::VT100, "CHT", "Cursor Horizontal Forward Tabulation");
constexpr inline auto CNL         = detail::CSI(std::nullopt, 0, 1, std::nullopt, 'E', vt_type::VT100, "CNL", "Move cursor to next line");
constexpr inline auto CPL         = detail::CSI(std::nullopt, 0, 1, std::nullopt, 'F', vt_type::VT100, "CPL", "Move cursor to previous line");
constexpr inline auto CPR         = detail::CSI(std::nullopt, 1, 1, std::nullopt, 'n', vt_type::VT100, "CPR", "Request Cursor position");
constexpr inline auto CUB         = detail::CSI(std::nullopt, 0, 1, std::nullopt, 'D', vt_type::VT100, "CUB", "Move cursor backward");
constexpr inline auto CUD         = detail::CSI(std::nullopt, 0, 1, std::nullopt, 'B', vt_type::VT100, "CUD", "Move cursor down");
constexpr inline auto CUF         = detail::CSI(std::nullopt, 0, 1, std::nullopt, 'C', vt_type::VT100, "CUF", "Move cursor forward");
constexpr inline auto CUP         = detail::CSI(std::nullopt, 0, 2, std::nullopt, 'H', vt_type::VT100, "CUP", "Move cursor to position");
constexpr inline auto CUU         = detail::CSI(std::nullopt, 0, 1, std::nullopt, 'A', vt_type::VT100, "CUU", "Move cursor up");
constexpr inline auto DA1         = detail::CSI(std::nullopt, 0, 1, std::nullopt, 'c', vt_type::VT100, "DA1", "Send primary device attributes");
constexpr inline auto DA2         = detail::CSI('>', 0, 1, std::nullopt, 'c', vt_type::VT100, "DA2", "Send secondary device attributes");
constexpr inline auto DA3         = detail::CSI('=', 0, 1, std::nullopt, 'c', vt_type::VT100, "DA3", "Send tertiary device attributes");
constexpr inline auto DCH         = detail::CSI(std::nullopt, 0, 1, std::nullopt, 'P', vt_type::VT100, "DCH", "Delete characters");
constexpr inline auto DECCARA     = detail::CSI(std::nullopt, 5, ArgsMax, '$', 'r', vt_type::VT420, "DECCARA", "Change Attributes in Rectangular Area");
constexpr inline auto DECCRA      = detail::CSI(std::nullopt, 0, 8, '$', 'v', vt_type::VT420, "DECCRA", "Copy rectangular area");
constexpr inline auto DECERA      = detail::CSI(std::nullopt, 0, 4, '$', 'z', vt_type::VT420, "DECERA", "Erase rectangular area");
constexpr inline auto DECFRA      = detail::CSI(std::nullopt, 0, 4, '$', 'x', vt_type::VT420, "DECFRA", "Fill rectangular area");
constexpr inline auto DECDC       = detail::CSI(std::nullopt, 0, 1, '\'', '~', vt_type::VT420, "DECDC", "Delete column");
constexpr inline auto DECIC       = detail::CSI(std::nullopt, 0, 1, '\'', '}', vt_type::VT420, "DECIC", "Insert column");
constexpr inline auto DECSCA      = detail::CSI(std::nullopt, 0, 1, '"', 'q', vt_type::VT240, "DECSCA", "Select Character Protection Attribute");
constexpr inline auto DECSED      = detail::CSI('?', 0, 1, std::nullopt, 'J', vt_type::VT240, "DECSED", "Selective Erase in Display");
constexpr inline auto DECSERA     = detail::CSI(std::nullopt, 0, 4, '$', '{', vt_type::VT240, "DECSERA", "Selective Erase in Rectangular Area");
constexpr inline auto DECSEL      = detail::CSI('?', 0, 1, std::nullopt, 'K', vt_type::VT240, "DECSEL", "Selective Erase in Line");
constexpr inline auto XTRESTORE   = detail::CSI('?', 0, ArgsMax, std::nullopt, 'r', vt_extension::XTerm, "XTRESTORE", "Restore DEC private modes.");
constexpr inline auto XTSAVE      = detail::CSI('?', 0, ArgsMax, std::nullopt, 's', vt_extension::XTerm, "XTSAVE", "Save DEC private modes.");
constexpr inline auto DECRM       = detail::CSI('?', 1, ArgsMax, std::nullopt, 'l', vt_type::VT100, "DECRM", "Reset DEC-mode");
constexpr inline auto DECRQM      = detail::CSI('?', 1, 1, '$', 'p', vt_type::VT100, "DECRQM", "Request DEC-mode");
constexpr inline auto DECRQM_ANSI = detail::CSI(std::nullopt, 1, 1, '$', 'p', vt_type::VT100, "DECRQM_ANSI", "Request ANSI-mode");
constexpr inline auto DECRQPSR    = detail::CSI(std::nullopt, 1, 1, '$', 'w', vt_type::VT320, "DECRQPSR", "Request presentation state report");
constexpr inline auto DECSCL      = detail::CSI(std::nullopt, 2, 2, '"', 'p', vt_type::VT220, "DECSCL", "Set conformance level (DECSCL), VT220 and up.");
constexpr inline auto DECSCPP     = detail::CSI(std::nullopt, 0, 1, '$', '|', vt_type::VT100, "DECSCPP", "Select 80 or 132 Columns per Page");
constexpr inline auto DECSNLS     = detail::CSI(std::nullopt, 0, 1, '*', '|', vt_type::VT420, "DECSNLS", "Select number of lines per screen.");
constexpr inline auto DECSCUSR    = detail::CSI(std::nullopt, 0, 1, ' ', 'q', vt_type::VT520, "DECSCUSR", "Set Cursor Style");
constexpr inline auto DECSLRM     = detail::CSI(std::nullopt, 2, 2, std::nullopt, 's', vt_type::VT420, "DECSLRM", "Set left/right margin");
constexpr inline auto DECSM       = detail::CSI('?', 1, ArgsMax, std::nullopt, 'h', vt_type::VT100, "DECSM", "Set DEC-mode");
constexpr inline auto DECSTBM     = detail::CSI(std::nullopt, 0, 2, std::nullopt, 'r', vt_type::VT100, "DECSTBM", "Set top/bottom margin");
constexpr inline auto DECSTR      = detail::CSI(std::nullopt, 0, 0, '!', 'p', vt_type::VT100, "DECSTR", "Soft terminal reset");
constexpr inline auto DECXCPR     = detail::CSI(std::nullopt, 0, 0, std::nullopt, '6', vt_type::VT100, "DECXCPR", "Request extended cursor position");
constexpr inline auto DL          = detail::CSI(std::nullopt, 0, 1, std::nullopt, 'M', vt_type::VT100, "DL",  "Delete lines");
constexpr inline auto ECH         = detail::CSI(std::nullopt, 0, 1, std::nullopt, 'X', vt_type::VT420, "ECH", "Erase characters");
constexpr inline auto ED          = detail::CSI(std::nullopt, 0, ArgsMax, std::nullopt, 'J', vt_type::VT100, "ED",  "Erase in display");
constexpr inline auto EL          = detail::CSI(std::nullopt, 0, 1, std::nullopt, 'K', vt_type::VT100, "EL",  "Erase in line");
constexpr inline auto HPA         = detail::CSI(std::nullopt, 1, 1, std::nullopt, '`', vt_type::VT100, "HPA", "Horizontal position absolute");
constexpr inline auto HPR         = detail::CSI(std::nullopt, 1, 1, std::nullopt, 'a', vt_type::VT100, "HPR", "Horizontal position relative");
constexpr inline auto HVP         = detail::CSI(std::nullopt, 0, 2, std::nullopt, 'f', vt_type::VT100, "HVP", "Horizontal and vertical position");
constexpr inline auto ICH         = detail::CSI(std::nullopt, 0, 1, std::nullopt, '@', vt_type::VT420, "ICH", "Insert character");
constexpr inline auto IL          = detail::CSI(std::nullopt, 0, 1, std::nullopt, 'L', vt_type::VT100, "IL",  "Insert lines");
constexpr inline auto REP         = detail::CSI(std::nullopt, 1, 1, std::nullopt, 'b', vt_type::VT100, "REP", "Repeat the preceding graphic character Ps times");
constexpr inline auto RM          = detail::CSI(std::nullopt, 1, ArgsMax, std::nullopt, 'l', vt_type::VT100, "RM",  "Reset mode");
constexpr inline auto SCOSC       = detail::CSI(std::nullopt, 0, 0, std::nullopt, 's', vt_type::VT100, "SCOSC", "Save Cursor");
constexpr inline auto SD          = detail::CSI(std::nullopt, 0, 1, std::nullopt, 'T', vt_type::VT100, "SD",  "Scroll down (pan up)");
constexpr inline auto SETMARK     = detail::CSI('>', 0, 0, std::nullopt, 'M', vt_extension::Contour, "XTSETMARK", "Set Vertical Mark (experimental syntax)");
constexpr inline auto SGR         = detail::CSI(std::nullopt, 0, ArgsMax, std::nullopt, 'm', vt_type::VT100, "SGR", "Select graphics rendition");
constexpr inline auto SM          = detail::CSI(std::nullopt, 1, ArgsMax, std::nullopt, 'h', vt_type::VT100, "SM",  "Set mode");
constexpr inline auto SU          = detail::CSI(std::nullopt, 0, 1, std::nullopt, 'S', vt_type::VT100, "SU",  "Scroll up (pan down)");
constexpr inline auto TBC         = detail::CSI(std::nullopt, 0, 1, std::nullopt, 'g', vt_type::VT100, "TBC", "Horizontal Tab Clear");
constexpr inline auto VPA         = detail::CSI(std::nullopt, 0, 1, std::nullopt, 'd', vt_type::VT100, "VPA", "Vertical Position Absolute");
constexpr inline auto WINMANIP    = detail::CSI(std::nullopt, 1, 3, std::nullopt, 't', vt_extension::XTerm, "WINMANIP", "Window Manipulation");
constexpr inline auto XTSMGRAPHICS= detail::CSI('?', 2, 4, std::nullopt, 'S', vt_extension::XTerm, "XTSMGRAPHICS", "Setting/getting Sixel/ReGIS graphics settings.");
constexpr inline auto XTPOPCOLORS    = detail::CSI(std::nullopt, 0, ArgsMax, '#', 'Q', vt_extension::XTerm, "XTPOPCOLORS", "Pops the color palette from the palette's saved-stack.");
constexpr inline auto XTPUSHCOLORS   = detail::CSI(std::nullopt, 0, ArgsMax, '#', 'P', vt_extension::XTerm, "XTPUSHCOLORS", "Pushes the color palette onto the palette's saved-stack.");
constexpr inline auto XTREPORTCOLORS = detail::CSI(std::nullopt, 0, 0, '#', 'R', vt_extension::XTerm, "XTREPORTCOLORS", "Reports number of color palettes on the stack.");
constexpr inline auto XTSHIFTESCAPE=detail::CSI('>', 0, 1, std::nullopt, 's', vt_extension::XTerm, "XTSHIFTESCAPE", "Set/reset shift-escape options.");
constexpr inline auto XTVERSION   = detail::CSI('>', 0, 1, std::nullopt, 'q', vt_extension::XTerm, "XTVERSION", "Query terminal name and version");
constexpr inline auto XTCAPTURE   = detail::CSI('>', 0, 2, std::nullopt, 't', vt_extension::Contour, "XTCAPTURE", "Report screen buffer capture.");

constexpr inline auto DECSSDT     = detail::CSI(std::nullopt, 0, 1, '$', '~', vt_type::VT320, "DECSSDT", "Select Status Display (Line) Type");
constexpr inline auto DECSASD     = detail::CSI(std::nullopt, 0, 1, '$', '}', vt_type::VT420, "DECSASD", "Select Active Status Display");
constexpr inline auto DECPS       = detail::CSI(std::nullopt, 3, 18, ',', '~', vt_type::VT520, "DECPS", "Controls the sound frequency or notes");

// DCS functions
constexpr inline auto STP         = detail::DCS(std::nullopt, 0, 0, '$', 'p', vt_extension::Contour, "XTSETPROFILE", "Set Terminal Profile");
constexpr inline auto DECRQSS     = detail::DCS(std::nullopt, 0, 0, '$', 'q', vt_type::VT420, "DECRQSS", "Request Status String");
constexpr inline auto DECSIXEL    = detail::DCS(std::nullopt, 0, 3, std::nullopt, 'q', vt_type::VT330, "DECSIXEL", "Sixel Graphics Image");
constexpr inline auto XTGETTCAP   = detail::DCS(std::nullopt, 0, 0, '+', 'q', vt_extension::XTerm, "XTGETTCAP", "Request Termcap/Terminfo String");

// OSC
constexpr inline auto SETTITLE      = detail::OSC(0, vt_extension::XTerm, "SETINICON", "Change Window & Icon Title");
constexpr inline auto SETICON       = detail::OSC(1, vt_extension::XTerm, "SETWINICON", "Change Icon Title");
constexpr inline auto SETWINTITLE   = detail::OSC(2, vt_extension::XTerm, "SETWINTITLE", "Change Window Title");
constexpr inline auto SETXPROP      = detail::OSC(3, vt_extension::XTerm, "SETXPROP", "Set X11 property");
constexpr inline auto SETCOLPAL     = detail::OSC(4, vt_extension::XTerm, "SETCOLPAL", "Set/Query color palette");
// TODO: Ps = 4 ; c ; spec -> Change Color Number c to the color specified by spec.
// TODO: Ps = 5 ; c ; spec -> Change Special Color Number c to the color specified by spec.
// TODO: Ps = 6 ; c ; f -> Enable/disable Special Color Number c.
// TODO: Ps = 7 (set current working directory)
constexpr inline auto SETCWD        = detail::OSC(7, vt_extension::XTerm, "SETCWD", "Set current working directory");
constexpr inline auto HYPERLINK     = detail::OSC(8, vt_extension::Unknown, "HYPERLINK", "Hyperlinked Text");
constexpr inline auto COLORFG       = detail::OSC(10, vt_extension::XTerm, "COLORFG", "Change or request text foreground color.");
constexpr inline auto COLORBG       = detail::OSC(11, vt_extension::XTerm, "COLORBG", "Change or request text background color.");
constexpr inline auto COLORCURSOR   = detail::OSC(12, vt_extension::XTerm, "COLORCURSOR", "Change text cursor color to Pt.");
constexpr inline auto COLORMOUSEFG  = detail::OSC(13, vt_extension::XTerm, "COLORMOUSEFG", "Change mouse foreground color.");
constexpr inline auto COLORMOUSEBG  = detail::OSC(14, vt_extension::XTerm, "COLORMOUSEBG", "Change mouse background color.");
constexpr inline auto SETFONT       = detail::OSC(50, vt_extension::XTerm, "SETFONT", "Get or set font.");
constexpr inline auto SETFONTALL    = detail::OSC(60, vt_extension::Contour, "SETFONTALL", "Get or set all font faces, styles, size.");
// printf "\033]52;c;$(printf "%s" "blabla" | base64)\a"
constexpr inline auto CLIPBOARD     = detail::OSC(52, vt_extension::XTerm, "CLIPBOARD", "Clipboard management.");
constexpr inline auto RCOLPAL       = detail::OSC(104, vt_extension::XTerm, "RCOLPAL", "Reset color full palette or entry");
constexpr inline auto COLORSPECIAL  = detail::OSC(106, vt_extension::XTerm, "COLORSPECIAL", "Enable/disable Special Color Number c.");
constexpr inline auto RCOLORFG      = detail::OSC(110, vt_extension::XTerm, "RCOLORFG", "Reset VT100 text foreground color.");
constexpr inline auto RCOLORBG      = detail::OSC(111, vt_extension::XTerm, "RCOLORBG", "Reset VT100 text background color.");
constexpr inline auto RCOLORCURSOR  = detail::OSC(112, vt_extension::XTerm, "RCOLORCURSOR", "Reset text cursor color.");
constexpr inline auto RCOLORMOUSEFG = detail::OSC(113, vt_extension::XTerm,"RCOLORMOUSEFG", "Reset mouse foreground color.");
constexpr inline auto RCOLORMOUSEBG = detail::OSC(114, vt_extension::XTerm,"RCOLORMOUSEBG", "Reset mouse background color.");
constexpr inline auto RCOLORHIGHLIGHTFG = detail::OSC(119, vt_extension::XTerm,"RCOLORHIGHLIGHTFG", "Reset highlight foreground color.");
constexpr inline auto RCOLORHIGHLIGHTBG = detail::OSC(117, vt_extension::XTerm,"RCOLORHIGHLIGHTBG", "Reset highlight background color.");
constexpr inline auto NOTIFY        = detail::OSC(777, vt_extension::XTerm,"NOTIFY", "Send Notification.");
constexpr inline auto DUMPSTATE     = detail::OSC(888, vt_extension::Contour, "DUMPSTATE", "Dumps internal state to debug stream.");

constexpr inline auto CaptureBufferCode = 314;

// clang-format on

inline auto const& functions() noexcept
{
    static auto const funcs = []() constexpr { // {{{
        auto f = std::array {
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
            CPR,
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
            XTRESTORE,
            XTSAVE,
            DECPS,
            DECRM,
            DECRQM,
            DECRQM_ANSI,
            DECRQPSR,
            DECSASD,
            DECSCL,
            DECSCPP,
            DECSCUSR,
            DECSLRM,
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
        crispy::sort(f, [](function_definition const& a, function_definition const& b) constexpr {
            return compare(a, b);
        });
        return f;
    }(); // }}}

#if 0
    for (auto [a, b] : crispy::indexed(funcs))
        std::cout << fmt::format("{:>2}: {}\n", a, b);
#endif

    return funcs;
}

/// Selects a FunctionDefinition based on a FunctionSelector.
///
/// @return the matching FunctionDefinition or nullptr if none matched.
function_definition const* select(function_selector const& selector) noexcept;

/// Selects a FunctionDefinition based on given input Escape sequence fields.
///
/// @p intermediate an optional intermediate character between (0x20 .. 0x2F)
/// @p finalCharacter between 0x40 .. 0x7F
///
/// @notice multi-character intermediates are intentionally not supported.
///
/// @return the matching FunctionDefinition or nullptr if none matched.
inline function_definition const* selectEscape(char intermediate, char finalCharacter)
{
    return select({ function_category::ESC, 0, 0, intermediate, finalCharacter });
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
inline function_definition const* selectControl(char leader, int argc, char intermediate, char finalCharacter)
{
    return select({ function_category::CSI, leader, argc, intermediate, finalCharacter });
}

/// Selects a FunctionDefinition based on given input control sequence fields.
///
/// @p id leading numeric identifier (such as 8 for hyperlink)
///
/// @notice multi-character intermediates are intentionally not supported.
///
/// @return the matching FunctionDefinition or nullptr if none matched.
inline function_definition const* selectOSCommand(int id)
{
    return select({ function_category::OSC, 0, id, 0, 0 });
}

} // namespace terminal

template <>
struct std::hash<terminal::function_definition>
{
    /// This is actually perfect hashing.
    constexpr uint32_t operator()(terminal::function_definition const& fun) const noexcept
    {
        return fun.id();
    }
};

// {{{ fmtlib support
template <>
struct fmt::formatter<terminal::function_category>: fmt::formatter<std::string_view>
{
    auto format(const terminal::function_category value, format_context& ctx) -> format_context::iterator
    {
        using terminal::function_category;
        string_view name;
        switch (value)
        {
            case function_category::C0:
                name = "C0";
                break;
                ;
            case function_category::ESC:
                name = "ESC";
                break;
                ;
            case function_category::CSI:
                name = "CSI";
                break;
                ;
            case function_category::OSC:
                name = "OSC";
                break;
                ;
            case function_category::DCS:
                name = "DCS";
                break;
                ;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct fmt::formatter<terminal::function_definition>
{
    static auto parse(format_parse_context& ctx) -> format_parse_context::iterator { return ctx.begin(); }
    static auto format(const terminal::function_definition f, format_context& ctx) -> format_context::iterator
    {
        switch (f.category)
        {
            case terminal::function_category::C0:
                return fmt::format_to(ctx.out(), "{}", crispy::escape(static_cast<uint8_t>(f.finalSymbol)));
            case terminal::function_category::ESC:
                return fmt::format_to(ctx.out(),
                                      "{} {} {}",
                                      f.category,
                                      f.intermediate ? f.intermediate : ' ',
                                      f.finalSymbol ? f.finalSymbol : ' ');
            case terminal::function_category::OSC:
                return fmt::format_to(ctx.out(), "{} {}", f.category, f.maximumParameters);
            case terminal::function_category::DCS:
            case terminal::function_category::CSI:
                if (f.minimumParameters == f.maximumParameters)
                    return fmt::format_to(ctx.out(),
                                          "{} {} {}    {} {}",
                                          f.category,
                                          f.leader ? f.leader : ' ',
                                          f.minimumParameters,
                                          f.intermediate ? f.intermediate : ' ',
                                          f.finalSymbol);
                else if (f.maximumParameters == terminal::ArgsMax)
                    return fmt::format_to(ctx.out(),
                                          "{} {} {}..  {} {}",
                                          f.category,
                                          f.leader ? f.leader : ' ',
                                          f.minimumParameters,
                                          f.intermediate ? f.intermediate : ' ',
                                          f.finalSymbol);
                else
                    return fmt::format_to(ctx.out(),
                                          "{} {} {}..{} {} {}",
                                          f.category,
                                          f.leader ? f.leader : ' ',
                                          f.minimumParameters,
                                          f.maximumParameters,
                                          f.intermediate ? f.intermediate : ' ',
                                          f.finalSymbol);
        }
        return fmt::format_to(ctx.out(), "?");
    }
};

template <>
struct fmt::formatter<terminal::function_selector>
{
    static auto parse(format_parse_context& ctx) -> format_parse_context::iterator { return ctx.begin(); }
    static auto format(const terminal::function_selector f, format_context& ctx) -> format_context::iterator
    {
        switch (f.category)
        {
            case terminal::function_category::OSC:
                return fmt::format_to(ctx.out(), "{} {}", f.category, f.argc);
            default:
                return fmt::format_to(ctx.out(),
                                      "{} {} {} {} {}",
                                      f.category,
                                      f.leader ? f.leader : ' ',
                                      f.argc,
                                      f.intermediate ? f.intermediate : ' ',
                                      f.finalSymbol ? f.finalSymbol : ' ');
        }
    }
};
// }}}
