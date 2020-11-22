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

#include <terminal/VTType.h>

#include <crispy/sort.h>

#include <fmt/format.h>

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace terminal {

enum class FunctionCategory {
    C0   = 0,
    ESC  = 1,
    CSI  = 2,
    OSC  = 3,
    DCS  = 4,
};

/// Defines a function with all its syntax requirements plus some additional meta information.
struct FunctionDefinition { // TODO: rename Function
    FunctionCategory category;  // (3 bits) C0, ESC, CSI, OSC, DCS
    char leader;                // (3 bits) 0x3C..0x3F (one of: < = > ?, or 0x00 for none)
    char intermediate;          // (4 bits) 0x20..0x2F (intermediates, usually just one, or 0x00 if none)
    char finalSymbol;           // (7 bits) 0x30..0x7E (final character)
    int32_t minimumParameters;  // (4 bits) 0..7
    int32_t maximumParameters;  // (7 bits) 0..127 or 0..2^7 for integer value (OSC function parameter)

    VTType conformanceLevel;
    std::string_view mnemonic;
    std::string_view comment;

    constexpr unsigned id() const noexcept
    {
        switch (category)
        {
            case FunctionCategory::C0:
                return static_cast<unsigned>(category) | finalSymbol << 3;
            default:
                return static_cast<unsigned>(category)
                     | (!leader ? 0 :       (leader - 0x3C)       <<  3)
                     | (!intermediate ? 0 : (intermediate - 0)    << (3 + 3))
                     | (!finalSymbol ? 0 :  (finalSymbol - 0x30)  << (3 + 3 + 4))
                     | minimumParameters                          << (3 + 3 + 4 + 7)
                     | maximumParameters                          << (3 + 3 + 4 + 7 + 4);
        }
    }

    constexpr operator unsigned () const noexcept { return id(); }
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

constexpr bool operator==(FunctionDefinition const& a, FunctionDefinition const& b) noexcept { return compare(a, b) == 0; }
constexpr bool operator!=(FunctionDefinition const& a, FunctionDefinition const& b) noexcept { return compare(a, b) != 0; }
constexpr bool operator<=(FunctionDefinition const& a, FunctionDefinition const& b) noexcept { return compare(a, b) <= 0; }
constexpr bool operator>=(FunctionDefinition const& a, FunctionDefinition const& b) noexcept { return compare(a, b) >= 0; }
constexpr bool operator<(FunctionDefinition const& a, FunctionDefinition const& b) noexcept { return compare(a, b) < 0; }
constexpr bool operator>(FunctionDefinition const& a, FunctionDefinition const& b) noexcept { return compare(a, b) > 0; }

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

/// Helps constructing VT functions as they're being parsed by the VT parser.
class Sequence {
  public:
    using Parameter = int;
    using ParameterList = std::vector<std::vector<Parameter>>;
    using Intermediaries = std::string;
    using DataString = std::string;

  private:
    FunctionCategory category_;
    char leaderSymbol_ = 0;
    ParameterList parameters_;
    Intermediaries intermediateCharacters_;
    char finalChar_ = 0;
    DataString dataString_;

  public:
    size_t constexpr static MaxParameters = 16;
    size_t constexpr static MaxSubParameters = 8;

    Sequence()
    {
        parameters_.resize(MaxParameters);
        for (auto& param : parameters_)
            param.reserve(MaxSubParameters);
        parameters_.clear();
    }

    // mutators
    //
    void clear()
    {
        category_ = FunctionCategory::C0;
        leaderSymbol_ = 0;
        intermediateCharacters_.clear();
        parameters_.clear();
        finalChar_ = 0;
        dataString_.clear();
    }

    void setCategory(FunctionCategory _cat) noexcept { category_ = _cat; }
    void setLeader(char _ch) noexcept { leaderSymbol_ = _ch; }
    ParameterList& parameters() noexcept { return parameters_; }
    Intermediaries& intermediateCharacters() noexcept { return intermediateCharacters_; }
    void setFinalChar(char _ch) noexcept { finalChar_ = _ch; }

    DataString const& dataString() const noexcept { return dataString_; }
    DataString& dataString() noexcept { return dataString_; }

    /// @returns this VT-sequence into a human readable string form.
    std::string text() const;

    /// @returns the raw VT-sequence string.
    std::string raw() const;

    /// Converts a FunctionSpinto a FunctionSelector, applicable for finding the corresponding FunctionDefinition.
    FunctionSelector selector() const noexcept
    {
        switch (category_)
        {
            case FunctionCategory::OSC:
                return FunctionSelector{category_, 0, parameters_[0][0], 0, 0};
            default:
            {
                // Only support CSI sequences with 0 or 1 intermediate characters.
                char const intermediate = intermediateCharacters_.size() == 1
                    ? static_cast<char>(intermediateCharacters_[0])
                    : char{};

                return FunctionSelector{category_, leaderSymbol_, static_cast<int>(parameters_.size()), intermediate, finalChar_};
            }
        }
    }

    // accessors
    //
    FunctionCategory category() const noexcept { return category_; }
    Intermediaries const& intermediateCharacters() const noexcept { return intermediateCharacters_; }
    char finalChar() const noexcept { return finalChar_; }

    ParameterList const& parameters() const noexcept { return parameters_; }
    size_t parameterCount() const noexcept { return parameters_.size(); }
    size_t subParameterCount(size_t _index) const noexcept { return parameters_[_index].size() - 1; }

    std::optional<Parameter> param_opt(size_t _index) const noexcept
    {
        if (_index < parameters_.size() && parameters_[_index][0])
            return {parameters_[_index][0]};
        else
            return std::nullopt;
    }

    Parameter param_or(size_t _index, Parameter _defaultValue) const noexcept
    {
        return param_opt(_index).value_or(_defaultValue);
    }

    int param(size_t _index) const noexcept
    {
        assert(_index < parameters_.size());
        assert(0 < parameters_[_index].size());
        return parameters_[_index][0];
    }

    int subparam(size_t _index, size_t _subIndex) const noexcept
    {
        assert(_index < parameters_.size());
        assert(_subIndex + 1 < parameters_[_index].size());
        return parameters_[_index][_subIndex + 1];
    }

    bool containsParameter(int _value) const noexcept
    {
        for (size_t i = 0; i < parameterCount(); ++i)
            if (param(i) == _value)
                return true;
        return false;
    }
};

namespace detail // {{{
{
    constexpr auto C0(char _final, std::string_view _mnemonic, std::string_view _description) noexcept
    {
        return FunctionDefinition{FunctionCategory::C0, 0, 0, _final, 0, 0, VTType::VT100, _mnemonic, _description};
    }

    constexpr auto OSC(int _code, std::string_view _mnemonic, std::string_view _description) noexcept
    {
        return FunctionDefinition{FunctionCategory::OSC, 0, 0, 0, 0, _code, VTType::VT100, _mnemonic, _description};
    }

    constexpr auto ESC(std::optional<char> _intermediate, char _final, VTType _vt, std::string_view _mnemonic, std::string_view _description) noexcept
    {
        return FunctionDefinition{FunctionCategory::ESC, 0, _intermediate.value_or(0), _final, 0, 0, _vt, _mnemonic, _description};
    }

    constexpr auto CSI(std::optional<char> _leader, int _argc0, int _argc1, std::optional<char> _intermediate, char _final, VTType _vt, std::string_view _mnemonic, std::string_view _description) noexcept
    {
        // TODO: static_assert on _leader/_intermediate range-or-null
        return FunctionDefinition{
            FunctionCategory::CSI,
            _leader.value_or(0),
            _intermediate.value_or(0),
            _final,
            _argc0,
            _argc1,
            _vt,
            _mnemonic,
            _description
        };
    }

    constexpr auto DCS(std::optional<char> _leader, int _argc0, int _argc1, std::optional<char> _intermediate, char _final, VTType _vt, std::string_view _mnemonic, std::string_view _description) noexcept
    {
        // TODO: static_assert on _leader/_intermediate range-or-null
        return FunctionDefinition{
            FunctionCategory::DCS,
            _leader.value_or(0),
            _intermediate.value_or(0),
            _final,
            _argc0,
            _argc1,
            _vt,
            _mnemonic,
            _description
        };
    }
} // }}}

// C0
constexpr inline auto EOT = detail::C0('\x04', "EOT", "End of Transmission");
constexpr inline auto BEL = detail::C0('\x07', "BEL", "Bell");
constexpr inline auto BS  = detail::C0('\x08', "BS", "Backspace");
constexpr inline auto TAB = detail::C0('\x09', "TAB", "Tab");
constexpr inline auto LF  = detail::C0('\x0A', "LF", "Line Feed");
constexpr inline auto VT  = detail::C0('\x0B', "VT", "Vertical Tab"); // Even though VT means Vertical Tab, it seems that xterm is doing an IND instead.
constexpr inline auto FF  = detail::C0('\x0C', "FF", "Form Feed");
constexpr inline auto CR  = detail::C0('\x0D', "CR", "Carriage Return");
constexpr inline auto SO  = detail::C0('\x0E', "SO", "Shift Out; Switch to an alternative character set. ");
constexpr inline auto SI  = detail::C0('\x0F', "SI", "Shift In; Return to regular character set after Shift Out.");

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
constexpr inline auto SCS_G0_SPECIAL = detail::ESC('(', '0', VTType::VT100, "SCS_G0_SPECIAL", "Set G0 to DEC Special Character and Line Drawing Set");
constexpr inline auto SCS_G0_USASCII = detail::ESC('(', 'B', VTType::VT100, "SCS_G0_USASCII", "Set G0 to USASCII");
constexpr inline auto SCS_G1_SPECIAL = detail::ESC(')', '0', VTType::VT100, "SCS_G0_SPECIAL", "Set G1 to DEC Special Character and Line Drawing Set");
constexpr inline auto SCS_G1_USASCII = detail::ESC(')', 'B', VTType::VT100, "SCS_G0_USASCII", "Set G1 to USASCII");
constexpr inline auto DECALN  = detail::ESC('#', '8', VTType::VT100, "DECALN", "Screen Alignment Pattern");
constexpr inline auto DECBI   = detail::ESC(std::nullopt, '6', VTType::VT100, "DECBI", "Back Index");
constexpr inline auto DECFI   = detail::ESC(std::nullopt, '9', VTType::VT100, "DECFI", "Forward Index");
constexpr inline auto DECKPAM = detail::ESC(std::nullopt, '=', VTType::VT100, "DECKPAM", "Keypad Application Mode");
constexpr inline auto DECKPNM = detail::ESC(std::nullopt, '>', VTType::VT100, "DECKPNM", "Keypad Numeric Mode");
constexpr inline auto DECRS   = detail::ESC(std::nullopt, '8', VTType::VT100, "DECRS", "Restore Cursor");
constexpr inline auto DECSC   = detail::ESC(std::nullopt, '7', VTType::VT100, "DECSC", "Save Cursor");
constexpr inline auto HTS     = detail::ESC(std::nullopt, 'H', VTType::VT100, "HTS", "Horizontal Tab Set");
constexpr inline auto IND     = detail::ESC(std::nullopt, 'D', VTType::VT100, "IND", "Index");
constexpr inline auto NEL     = detail::ESC(std::nullopt, 'E', VTType::VT100, "NEL", "Next Line");
constexpr inline auto RI      = detail::ESC(std::nullopt, 'M', VTType::VT100, "RI", "Reverse Index");
constexpr inline auto RIS     = detail::ESC(std::nullopt, 'c', VTType::VT100, "RIS", "Reset to Initial State (Hard Reset)");
constexpr inline auto SS2     = detail::ESC(std::nullopt, 'N', VTType::VT220, "SS2", "Single Shift Select (G2 Character Set)");
constexpr inline auto SS3     = detail::ESC(std::nullopt, 'O', VTType::VT220, "SS3", "Single Shift Select (G3 Character Set)");

// CSI
constexpr inline auto ArgsMax = 127; // this is the maximum number that fits into 7 bits.

// CSI functions
constexpr inline auto ANSISYSSC   = detail::CSI(std::nullopt, 0, 0, std::nullopt, 'u', VTType::VT100, "ANSISYSSC", "Save Cursor (ANSI.SYS)");
constexpr inline auto CBT         = detail::CSI(std::nullopt, 0, 1, std::nullopt, 'Z', VTType::VT100, "CBT", "Cursor Backward Tabulation");
constexpr inline auto CHA         = detail::CSI(std::nullopt, 0, 1, std::nullopt, 'G', VTType::VT100, "CHA", "Move cursor to column");
constexpr inline auto CHT         = detail::CSI(std::nullopt, 0, 1, std::nullopt, 'I', VTType::VT100, "CHT", "Cursor Horizontal Forward Tabulation");
constexpr inline auto CNL         = detail::CSI(std::nullopt, 0, 1, std::nullopt, 'E', VTType::VT100, "CNL", "Move cursor to next line");
constexpr inline auto CPL         = detail::CSI(std::nullopt, 0, 1, std::nullopt, 'F', VTType::VT100, "CPL", "Move cursor to previous line");
constexpr inline auto CPR         = detail::CSI(std::nullopt, 1, 1, std::nullopt, 'n', VTType::VT100, "CPR", "Request Cursor position");
constexpr inline auto CUB         = detail::CSI(std::nullopt, 0, 1, std::nullopt, 'D', VTType::VT100, "CUB", "Move cursor backward");
constexpr inline auto CUD         = detail::CSI(std::nullopt, 0, 1, std::nullopt, 'B', VTType::VT100, "CUD", "Move cursor down");
constexpr inline auto CUF         = detail::CSI(std::nullopt, 0, 1, std::nullopt, 'C', VTType::VT100, "CUF", "Move cursor forward");
constexpr inline auto CUP         = detail::CSI(std::nullopt, 0, 2, std::nullopt, 'H', VTType::VT100, "CUP", "Move cursor to position");
constexpr inline auto CUU         = detail::CSI(std::nullopt, 0, 1, std::nullopt, 'A', VTType::VT100, "CUU", "Move cursor up");
constexpr inline auto DA1         = detail::CSI(std::nullopt, 0, 1, std::nullopt, 'c', VTType::VT100, "DA1", "Send primary device attributes");
constexpr inline auto DA2         = detail::CSI('>', 0, 1, std::nullopt, 'c', VTType::VT100, "DA2", "Send secondary device attributes");
constexpr inline auto DA3         = detail::CSI('=', 0, 1, std::nullopt, 'c', VTType::VT100, "DA3", "Send tertiary device attributes");
constexpr inline auto DCH         = detail::CSI(std::nullopt, 0, 1, std::nullopt, 'P', VTType::VT100, "DCH", "Delete characters");
constexpr inline auto DECDC       = detail::CSI(std::nullopt, 0, 1, '\'', '~', VTType::VT420, "DECDC", "Delete column");
constexpr inline auto DECIC       = detail::CSI(std::nullopt, 0, 1, '\'', '}', VTType::VT420, "DECIC", "Insert column");
constexpr inline auto DECMODERESTORE = detail::CSI('?', 0, ArgsMax, std::nullopt, 'r', VTType::VT525, "DECMODERESTORE", "Restore DEC private modes.");
constexpr inline auto DECMODESAVE    = detail::CSI('?', 0, ArgsMax, std::nullopt, 's', VTType::VT525, "DECMODESAVE", "Save DEC private modes.");
constexpr inline auto DECRM       = detail::CSI('?', 1, ArgsMax, std::nullopt, 'l', VTType::VT100, "DECRM", "Reset DEC-mode");
constexpr inline auto DECRQM      = detail::CSI('?', 1, 1, '$', 'p', VTType::VT100, "DECRQM", "Request DEC-mode");
constexpr inline auto DECRQM_ANSI = detail::CSI(std::nullopt, 1, 1, '$', 'p', VTType::VT100, "DECRQM_ANSI", "Request ANSI-mode");
constexpr inline auto DECRQPSR    = detail::CSI(std::nullopt, 1, 1, '$', 'w', VTType::VT320, "DECRQPSR", "Request presentation state report");
constexpr inline auto DECSCL      = detail::CSI(std::nullopt, 2, 2, '"', 'p', VTType::VT220, "DECSCL", "Set conformance level (DECSCL), VT220 and up.");
constexpr inline auto DECSCUSR    = detail::CSI(std::nullopt, 0, 1, ' ', 'q', VTType::VT100, "DECSCUSR", "Set Cursor Style");
constexpr inline auto DECSLRM     = detail::CSI(std::nullopt, 2, 2, std::nullopt, 's', VTType::VT420, "DECSLRM", "Set left/right margin");
constexpr inline auto DECSM       = detail::CSI('?', 1, ArgsMax, std::nullopt, 'h', VTType::VT100, "DECSM", "Set DEC-mode");
constexpr inline auto DECSTBM     = detail::CSI(std::nullopt, 0, 2, std::nullopt, 'r', VTType::VT100, "DECSTBM", "Set top/bottom margin");
constexpr inline auto DECSTR      = detail::CSI(std::nullopt, 0, 0, '!', 'p', VTType::VT100, "DECSTR", "Soft terminal reset");
constexpr inline auto DECXCPR     = detail::CSI(std::nullopt, 0, 0, std::nullopt, '6', VTType::VT100, "DECXCPR", "Request extended cursor position");
constexpr inline auto DL          = detail::CSI(std::nullopt, 0, 1, std::nullopt, 'M', VTType::VT100, "DL",  "Delete lines");
constexpr inline auto ECH         = detail::CSI(std::nullopt, 0, 1, std::nullopt, 'X', VTType::VT420, "ECH", "Erase characters");
constexpr inline auto ED          = detail::CSI(std::nullopt, 0, ArgsMax, std::nullopt, 'J', VTType::VT100, "ED",  "Erase in display");
constexpr inline auto EL          = detail::CSI(std::nullopt, 0, 1, std::nullopt, 'K', VTType::VT100, "EL",  "Erase in line");
constexpr inline auto HPA         = detail::CSI(std::nullopt, 1, 1, std::nullopt, '`', VTType::VT100, "HPA", "Horizontal position absolute");
constexpr inline auto HPR         = detail::CSI(std::nullopt, 1, 1, std::nullopt, 'a', VTType::VT100, "HPR", "Horizontal position relative");
constexpr inline auto HVP         = detail::CSI(std::nullopt, 0, 2, std::nullopt, 'f', VTType::VT100, "HVP", "Horizontal and vertical position");
constexpr inline auto ICH         = detail::CSI(std::nullopt, 0, 1, std::nullopt, '@', VTType::VT420, "ICH", "Insert character");
constexpr inline auto IL          = detail::CSI(std::nullopt, 0, 1, std::nullopt, 'L', VTType::VT100, "IL",  "Insert lines");
constexpr inline auto RM          = detail::CSI(std::nullopt, 1, ArgsMax, std::nullopt, 'l', VTType::VT100, "RM",  "Reset mode");
constexpr inline auto SCOSC       = detail::CSI(std::nullopt, 0, 0, std::nullopt, 's', VTType::VT100, "SCOSC", "Save Cursor");
constexpr inline auto SD          = detail::CSI(std::nullopt, 0, 1, std::nullopt, 'T', VTType::VT100, "SD",  "Scroll down (pan up)");
constexpr inline auto SETMARK     = detail::CSI('>', 0, 0, std::nullopt, 'M', VTType::VT100, "SETMARK", "Set Vertical Mark");
constexpr inline auto SGR         = detail::CSI(std::nullopt, 0, ArgsMax, std::nullopt, 'm', VTType::VT100, "SGR", "Select graphics rendition");
constexpr inline auto SM          = detail::CSI(std::nullopt, 1, ArgsMax, std::nullopt, 'h', VTType::VT100, "SM",  "Set mode");
constexpr inline auto SU          = detail::CSI(std::nullopt, 0, 1, std::nullopt, 'S', VTType::VT100, "SU",  "Scroll up (pan down)");
constexpr inline auto TBC         = detail::CSI(std::nullopt, 0, 1, std::nullopt, 'g', VTType::VT100, "TBC", "Horizontal Tab Clear");
constexpr inline auto VPA         = detail::CSI(std::nullopt, 0, 1, std::nullopt, 'd', VTType::VT100, "VPA", "Vertical Position Absolute");
constexpr inline auto WINMANIP    = detail::CSI(std::nullopt, 1, 3, std::nullopt, 't', VTType::VT525, "WINMANIP", "Window Manipulation");
constexpr inline auto XTSMGRAPHICS= detail::CSI('?', 2, 4, std::nullopt, 'S', VTType::VT525 /*XT*/, "XTSMGRAPHICS", "Setting/getting Sixel/ReGIS graphics settings.");

// DCS functions
constexpr inline auto DECRQSS     = detail::DCS(std::nullopt, 0, 0, '$', 'q', VTType::VT420, "DECRQSS", "Request Status String");
constexpr inline auto DECSIXEL    = detail::DCS(std::nullopt, 0, 3, std::nullopt, 'q', VTType::VT330, "DECSIXEL", "Sixel Graphics Image");

// OSC
constexpr inline auto SETTITLE      = detail::OSC(0, "SETINICON", "Change Window & Icon Title");
constexpr inline auto SETICON       = detail::OSC(1, "SETWINICON", "Change Icon Title");
constexpr inline auto SETWINTITLE   = detail::OSC(2, "SETWINTITLE", "Change Window Title");
constexpr inline auto SETXPROP      = detail::OSC(3, "SETXPROP", "Set X11 property");
// TODO: Ps = 4 ; c ; spec -> Change Color Number c to the color specified by spec.
// TODO: Ps = 5 ; c ; spec -> Change Special Color Number c to the color specified by spec.
// TODO: Ps = 6 ; c ; f -> Enable/disable Special Color Number c.
// TODO: Ps = 7 (set current working directory)
constexpr inline auto HYPERLINK     = detail::OSC(8, "HYPERLINK", "Hyperlinked Text");
constexpr inline auto COLORFG       = detail::OSC(10, "COLORFG", "Change or request text foreground color.");
constexpr inline auto COLORBG       = detail::OSC(11, "COLORBG", "Change or request text background color.");
constexpr inline auto COLORCURSOR   = detail::OSC(12, "COLORCURSOR", "Change text cursor color to Pt.");
constexpr inline auto COLORMOUSEFG  = detail::OSC(13, "COLORMOUSEFG", "Change mouse foreground color.");
constexpr inline auto COLORMOUSEBG  = detail::OSC(14, "COLORMOUSEBG", "Change mouse background color.");
// printf "\033]52;c;$(printf "%s" "blabla" | base64)\a"
constexpr inline auto CLIPBOARD     = detail::OSC(52, "CLIPBOARD", "Clipboard management.");
constexpr inline auto COLORSPECIAL  = detail::OSC(106, "COLORSPECIAL", "Enable/disable Special Color Number c.");
constexpr inline auto RCOLORFG      = detail::OSC(110, "RCOLORFG", "Reset VT100 text foreground color.");
constexpr inline auto RCOLORBG      = detail::OSC(111, "RCOLORBG", "Reset VT100 text background color.");
constexpr inline auto RCOLORCURSOR  = detail::OSC(112, "RCOLORCURSOR", "Reset text cursor color.");
constexpr inline auto RCOLORMOUSEFG = detail::OSC(113, "RCOLORMOUSEFG", "Reset mouse foreground color.");
constexpr inline auto RCOLORMOUSEBG = detail::OSC(114, "RCOLORMOUSEBG", "Reset mouse background color.");
constexpr inline auto RCOLORHIGHLIGHTFG = detail::OSC(119, "RCOLORHIGHLIGHTFG", "Reset highlight foreground color.");
constexpr inline auto RCOLORHIGHLIGHTBG = detail::OSC(117, "RCOLORHIGHLIGHTBG", "Reset highlight background color.");
constexpr inline auto NOTIFY        = detail::OSC(777, "NOTIFY", "Send Notification.");
constexpr inline auto DUMPSTATE     = detail::OSC(888, "DUMPSTATE", "Dumps internal state to debug stream.");

inline auto const& functions()
{
    static auto const funcs = []() constexpr { // {{{
        auto f = std::array{
            // C0
            EOT,
            BEL,
            BS,
            TAB,
            LF,
            VT,
            FF,
            CR,
            SO,
            SI,

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
            DECDC,
            DECIC,
            DECMODERESTORE,
            DECMODESAVE,
            DECRM,
            DECRQM,
            DECRQM_ANSI,
            DECRQPSR,
            DECSCL,
            DECSCUSR,
            DECSLRM,
            DECSM,
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
            XTSMGRAPHICS,

            // DCS
            DECRQSS,
            DECSIXEL,

            // OSC
            SETICON,
            SETTITLE,
            SETWINTITLE,
            SETXPROP,
            HYPERLINK,
            COLORFG,
            COLORBG,
            COLORCURSOR,
            COLORMOUSEFG,
            COLORMOUSEBG,
            CLIPBOARD,
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
        crispy::sort(f, [](FunctionDefinition const& a, FunctionDefinition const& b) constexpr { return compare(a, b); });
        return f;
    }();  // }}}

#if 0
    for (auto [a, b] : crispy::indexed(funcs))
        std::cout << fmt::format("{:>2}: {}\n", a, b);
#endif

    return funcs;
}

/// Selects a FunctionDefinition based on a FunctionSelector.
///
/// @return the matching FunctionDefinition or nullptr if none matched.
FunctionDefinition const* select(FunctionSelector const& _selector);

/// Selects a FunctionDefinition based on given input Escape sequence fields.
///
/// @p _intermediate an optional intermediate character between (0x20 .. 0x2F)
/// @p _final between 0x40 .. 0x7F
///
/// @notice multi-character intermediates are intentionally not supported.
///
/// @return the matching FunctionDefinition or nullptr if none matched.
inline FunctionDefinition const* selectEscape(char _intermediate, char _final)
{
    return select({FunctionCategory::ESC, 0, 0, _intermediate, _final});
}

/// Selects a FunctionDefinition based on given input control sequence fields.
///
/// @p _leader an optional value between 0x3C .. 0x3F
/// @p _argc number of arguments supplied
/// @p _intermediate an optional intermediate character between (0x20 .. 0x2F)
/// @p _final between 0x40 .. 0x7F
///
/// @notice multi-character intermediates are intentionally not supported.
///
/// @return the matching FunctionDefinition or nullptr if none matched.
inline FunctionDefinition const* selectControl(char _leader, int _argc, char _intermediate, char _final)
{
    return select({FunctionCategory::CSI, _leader, _argc, _intermediate, _final});
}

/// Selects a FunctionDefinition based on given input control sequence fields.
///
/// @p _id leading numeric identifier (such as 8 for hyperlink)
///
/// @notice multi-character intermediates are intentionally not supported.
///
/// @return the matching FunctionDefinition or nullptr if none matched.
inline FunctionDefinition const* selectOSCommand(int _id)
{
    return select({FunctionCategory::OSC, 0, _id, 0, 0});
}

} // end namespace

namespace std {
    template<>
    struct hash<terminal::FunctionDefinition> {
        /// This is actually perfect hashing.
        constexpr uint32_t operator()(terminal::FunctionDefinition const& _fun) const noexcept {
            return _fun.id();
        }
    };
}

namespace fmt // {{{
{
    template <>
    struct formatter<terminal::Sequence> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(terminal::Sequence const& seq, FormatContext& ctx)
        {
            return format_to(ctx.out(), "{}", seq.text());
        }
    };

    template <>
    struct formatter<terminal::FunctionCategory> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(const terminal::FunctionCategory value, FormatContext& ctx)
        {
            using terminal::FunctionCategory;
            switch (value)
            {
                case FunctionCategory::C0:   return format_to(ctx.out(), "C0");
                case FunctionCategory::ESC:  return format_to(ctx.out(), "ESC");
                case FunctionCategory::CSI:  return format_to(ctx.out(), "CSI");
                case FunctionCategory::OSC:  return format_to(ctx.out(), "OSC");
                case FunctionCategory::DCS:  return format_to(ctx.out(), "DCS");
            }
            return format_to(ctx.out(), "({})", static_cast<unsigned>(value));
        }
    };

    template <>
    struct formatter<terminal::FunctionDefinition> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(const terminal::FunctionDefinition f, FormatContext& ctx)
        {
            switch (f.category)
            {
                case terminal::FunctionCategory::C0:
                    return format_to(
                        ctx.out(),
                        "{}",
                        f.mnemonic
                    );
                case terminal::FunctionCategory::ESC:
                    return format_to(
                        ctx.out(),
                        "{} {} {}",
                        f.category,
                        f.intermediate ? f.intermediate : ' ',
                        f.finalSymbol ? f.finalSymbol : ' '
                    );
                case terminal::FunctionCategory::OSC:
                    return format_to(
                        ctx.out(),
                        "{} {}",
                        f.category,
                        f.maximumParameters
                    );
                case terminal::FunctionCategory::DCS:
                case terminal::FunctionCategory::CSI:
                    if (f.minimumParameters == f.maximumParameters)
                        return format_to(
                            ctx.out(),
                            "{} {} {}    {} {}",
                            f.category,
                            f.leader ? f.leader : ' ',
                            f.minimumParameters,
                            f.intermediate ? f.intermediate : ' ',
                            f.finalSymbol
                        );
                    else if (f.maximumParameters == terminal::ArgsMax)
                        return format_to(
                            ctx.out(),
                            "{} {} {}..  {} {}",
                            f.category,
                            f.leader ? f.leader : ' ',
                            f.minimumParameters,
                            f.intermediate ? f.intermediate : ' ',
                            f.finalSymbol
                        );
                    else
                        return format_to(
                            ctx.out(),
                            "{} {} {}..{} {} {}",
                            f.category,
                            f.leader ? f.leader : ' ',
                            f.minimumParameters,
                            f.maximumParameters,
                            f.intermediate ? f.intermediate : ' ',
                            f.finalSymbol
                        );
            }
            return format_to(ctx.out(), "?");
        }
    };

    template <>
    struct formatter<terminal::FunctionSelector> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(const terminal::FunctionSelector f, FormatContext& ctx)
        {
            switch (f.category)
            {
                case terminal::FunctionCategory::OSC:
                    return format_to(
                        ctx.out(),
                        "{} {}",
                        f.category,
                        f.argc
                    );
                default:
                    return format_to(
                        ctx.out(),
                        "{} {} {} {} {}",
                        f.category,
                        f.leader ? f.leader : ' ',
                        f.argc,
                        f.intermediate ? f.intermediate : ' ',
                        f.finalSymbol ? f.finalSymbol : ' '
                    );
            }
        }
    };
} // }}}
