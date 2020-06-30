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
#include <terminal/Commands.h>

#include <fmt/format.h>

namespace terminal {

enum class FunctionCategory {
    ESC = 0,
    CSI = 1,
    OSC = 2,
};

/// Defines a function with all its syntax requirements plus some additional meta information.
struct FunctionSpec {
    FunctionCategory category;  // (2 bits) ESC, CSI, OSC
    char leader;                // (3 bits) 0x3C..0x3F (one of: < = > ?, or 0x00 for none)
    char intermediate;          // (4 bits) 0x20..0x2F (intermediates, usually just one, or 0x00 if none)
    char finalSymbol;           // (6 bits) 0x40..0x7E (final character)
    unsigned minimumParameters; // (4 bits) 0..7
    unsigned maximumParameters; // (7 bits) 0..127

    VTType conformanceLevel;
    std::string_view mnemonic;
    std::string_view comment;

    constexpr unsigned id() const noexcept {
        return static_cast<unsigned>(category)
             | leader << 2
             | intermediate << (2 + 3)
             | finalSymbol << (2 + 3 + 4)
             | minimumParameters << (2 + 3 + 4 + 6)
             | maximumParameters << (2 + 3 + 4 + 6 + 4);
    }

    constexpr operator unsigned () const noexcept { return id(); }
};

constexpr int compare(FunctionSpec const& a, FunctionSpec const& b)
{
    if (a.category != b.category)
        return static_cast<int>(a.category) - static_cast<int>(b.category);

    if (a.leader != b.leader)
        return a.leader - b.leader;

    if (a.intermediate != b.intermediate)
        return static_cast<int>(a.intermediate) - static_cast<int>(b.intermediate);

    if (a.finalSymbol != b.finalSymbol)
        return a.finalSymbol - b.finalSymbol;

    return static_cast<int>(a.minimumParameters) - static_cast<int>(b.minimumParameters);
}

constexpr bool operator==(FunctionSpec const& a, FunctionSpec const& b) noexcept { return compare(a, b) == 0; }
constexpr bool operator!=(FunctionSpec const& a, FunctionSpec const& b) noexcept { return compare(a, b) != 0; }
constexpr bool operator<=(FunctionSpec const& a, FunctionSpec const& b) noexcept { return compare(a, b) <= 0; }
constexpr bool operator>=(FunctionSpec const& a, FunctionSpec const& b) noexcept { return compare(a, b) >= 0; }
constexpr bool operator<(FunctionSpec const& a, FunctionSpec const& b) noexcept { return compare(a, b) < 0; }
constexpr bool operator>(FunctionSpec const& a, FunctionSpec const& b) noexcept { return compare(a, b) > 0; }

struct FunctionSelector
{
    /// represents the corresponding function category.
    FunctionCategory category;
    /// an optional value between 0x3C .. 0x3F
    char leader;
    /// number of arguments supplied
    size_t argc;
    /// an optional intermediate character between (0x20 .. 0x2F)
    char intermediate;
    /// between 0x40 .. 0x7F
    char finalSymbol;
};

constexpr int compare(FunctionSelector const& a, FunctionSpec const& b) noexcept
{
    if (a.category != b.category)
        return static_cast<int>(a.category) - static_cast<int>(b.category);

    if (a.leader != b.leader)
        return a.leader - b.leader;

    if (a.intermediate != b.intermediate)
        return a.intermediate - b.intermediate;

    if (a.finalSymbol != b.finalSymbol)
        return a.finalSymbol - b.finalSymbol;

    if (a.argc < b.minimumParameters)
        return -1;

    if (a.argc > b.maximumParameters)
        return +1;

    return 0;
}

namespace detail // {{{
{
    constexpr auto ESC(std::optional<char> _intermediate, char _final, VTType _vt, std::string_view _mnemonic, std::string_view _description) noexcept
    {
        return FunctionSpec{FunctionCategory::ESC, 0, _intermediate.value_or(0), _final, 0, 0, _vt, _mnemonic, _description};
    }

    constexpr auto CSI(std::optional<char> _leader, unsigned _argc0, unsigned _argc1, std::optional<char> _intermediate, char _final, VTType _vt, std::string_view _mnemonic, std::string_view _description) noexcept
    {
        return FunctionSpec{
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
    // }
} // }}}

// CSI
constexpr inline auto ArgsMax = 127; // this is the maximum number that fits into 7 bits.

// ESC functions
constexpr inline auto CS_G0_SPECIAL = detail::ESC('(', '0', VTType::VT100, "CS_G0_SPECIAL", "Set G0 to DEC Special Character and Line Drawing Set");
constexpr inline auto CS_G0_USASCII = detail::ESC('(', 'B', VTType::VT100, "CS_G0_USASCII", "Set G0 to USASCII");
constexpr inline auto CS_G1_SPECIAL = detail::ESC(')', '0', VTType::VT100, "CS_G0_SPECIAL", "Set G1 to DEC Special Character and Line Drawing Set");
constexpr inline auto CS_G1_USASCII = detail::ESC(')', 'B', VTType::VT100, "CS_G0_USASCII", "Set G1 to USASCII");
constexpr inline auto DECALN  = detail::ESC('#', '8', VTType::VT100, "DECALN", "Screen Alignment Pattern");
constexpr inline auto DECBI   = detail::ESC(std::nullopt, '6', VTType::VT100, "DECBI", "Back Index");
constexpr inline auto DECFI   = detail::ESC(std::nullopt, '9', VTType::VT100, "DECFI", "Forward Index");
constexpr inline auto DECKPAM = detail::ESC(std::nullopt, '=', VTType::VT100, "DECKPAM", "Keypad Application Mode");
constexpr inline auto DECKPNM = detail::ESC(std::nullopt, '>', VTType::VT100, "DECKPNM", "Keypad Numeric Mode");
constexpr inline auto DECRS   = detail::ESC(std::nullopt, '8', VTType::VT100, "DECRS", "Restore Cursor");
constexpr inline auto DECSC   = detail::ESC(std::nullopt, '7', VTType::VT100, "DECSC", "Save Cursor");
constexpr inline auto HTS     = detail::ESC(std::nullopt, 'H', VTType::VT100, "HTS", "Horizontal Tab Set");
constexpr inline auto IND     = detail::ESC(std::nullopt, 'D', VTType::VT100, "IND", "Index");
constexpr inline auto RI      = detail::ESC(std::nullopt, 'M', VTType::VT100, "RI", "Reverse Index");
constexpr inline auto RIS     = detail::ESC(std::nullopt, 'c', VTType::VT100, "RIS", "Reset to Initial State (Hard Reset)");
constexpr inline auto SS2     = detail::ESC(std::nullopt, 'N', VTType::VT220, "SS2", "Single Shift Select (G2 Character Set)");
constexpr inline auto SS3     = detail::ESC(std::nullopt, 'O', VTType::VT220, "SS3", "Single Shift Select (G3 Character Set)");

// CSI functions
constexpr inline auto CHA         = detail::CSI(std::nullopt, 0, 1, std::nullopt, 'G', VTType::VT100, "CHA", "Move cursor to column");
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
constexpr inline auto DECDC       = detail::CSI('\'', 0, 1, std::nullopt, '~', VTType::VT420, "DECDC", "Delete column");
constexpr inline auto DECIC       = detail::CSI(std::nullopt, 0, 1, '\'', '}', VTType::VT420, "DECIC", "Insert column");
constexpr inline auto TBC         = detail::CSI(std::nullopt, 0, 1, std::nullopt, 'g', VTType::VT100, "TBC", "Horizontal Tab Clear");
constexpr inline auto DECRM       = detail::CSI('?', 1, ArgsMax, std::nullopt, 'l', VTType::VT100, "DECRM", "Reset DEC-mode");
constexpr inline auto DECRQM      = detail::CSI('?', 1, 1, '$', 'p', VTType::VT100, "DECRQM", "Request DEC-mode");
constexpr inline auto DECRQM_ANSI = detail::CSI(std::nullopt, 1, 1, '$', 'p', VTType::VT100, "DECRQM_ANSI", "Request ANSI-mode");
constexpr inline auto DECRQPSR    = detail::CSI(std::nullopt, 1, 1, '$', 'w', VTType::VT320, "DECRQPSR", "Request presentation state report");
constexpr inline auto DECSCUSR    = detail::CSI(std::nullopt, 0, 1, ' ', 'q', VTType::VT100, "DECSCUSR", "Set Cursor Style");
constexpr inline auto ANSISYSSC   = detail::CSI(std::nullopt, 0, 0, std::nullopt, 'u', VTType::VT100, "ANSISYSSC", "Save Cursor (ANSI.SYS)");
constexpr inline auto SCOSC       = detail::CSI(std::nullopt, 0, 0, std::nullopt, 's', VTType::VT100, "SCOSC", "Save Cursor");
constexpr inline auto DECSLRM     = detail::CSI(std::nullopt, 2, 2, std::nullopt, 's', VTType::VT420, "DECSLRM", "Set left/right margin");
constexpr inline auto DECSM       = detail::CSI('?', 1, ArgsMax, std::nullopt, 'h', VTType::VT100, "DECSM", "Set DEC-mode");
constexpr inline auto DECSTBM     = detail::CSI(std::nullopt, 0, 2, std::nullopt, 'r', VTType::VT100, "DECSTBM", "Set top/bottom margin");
constexpr inline auto DECSTR      = detail::CSI('!', 0, 0, std::nullopt, 'p', VTType::VT100, "DECSTR", "Soft terminal reset");
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
constexpr inline auto SD          = detail::CSI(std::nullopt, 0, 1, std::nullopt, 'T', VTType::VT100, "SD",  "Scroll down (pan up)");
constexpr inline auto SGR         = detail::CSI(std::nullopt, 0, ArgsMax, std::nullopt, 'm', VTType::VT100, "SGR", "Select graphics rendition");
constexpr inline auto SM          = detail::CSI(std::nullopt, 1, ArgsMax, std::nullopt, 'h', VTType::VT100, "SM",  "Set mode");
constexpr inline auto SU          = detail::CSI(std::nullopt, 0, 1, std::nullopt, 'S', VTType::VT100, "SU",  "Scroll up (pan down)");
constexpr inline auto VPA         = detail::CSI(std::nullopt, 0, 1, std::nullopt, 'd', VTType::VT100, "VPA", "Vertical Position Absolute");
constexpr inline auto WINMANIP    = detail::CSI(std::nullopt, 1, 3, std::nullopt, 't', VTType::VT525, "WINMANIP", "Window Manipulation");
constexpr inline auto CBT         = detail::CSI(std::nullopt, 0, 1, std::nullopt, 'Z', VTType::VT100, "CBT", "Cursor Backward Tabulation");
constexpr inline auto SETMARK     = detail::CSI('>', 0, 0, std::nullopt, 'M', VTType::VT100, "SETMARK", "Set Vertical Mark");

/// Selects a FunctionSpec based on a FunctionSelector.
///
/// @return the matching FunctionSpec or nullptr if none matched.
FunctionSpec const* select(FunctionSelector const& _selector);

/// Selects a FunctionSpec based on given input Escape sequence fields.
///
/// @p _intermediate an optional intermediate character between (0x20 .. 0x2F)
/// @p _final between 0x40 .. 0x7F
///
/// @notice multi-character intermediates are intentionally not supported.
///
/// @return the matching FunctionSpec or nullptr if none matched.
inline FunctionSpec const* selectEscape(char _intermediate, char _final)
{
    return select({FunctionCategory::ESC, 0, 0, _intermediate, _final});
}

/// Selects a FunctionSpec based on given input control sequence fields.
///
/// @p _leader an optional value between 0x3C .. 0x3F
/// @p _argc number of arguments supplied
/// @p _intermediate an optional intermediate character between (0x20 .. 0x2F)
/// @p _final between 0x40 .. 0x7F
///
/// @notice multi-character intermediates are intentionally not supported.
///
/// @return the matching FunctionSpec or nullptr if none matched.
inline FunctionSpec const* selectControl(char _leader, unsigned _argc, char _intermediate, char _final)
{
    return select({FunctionCategory::CSI, _leader, _argc, _intermediate, _final});
}

/// Selects a FunctionSpec based on given input control sequence fields.
///
/// @p _id leading numeric identifier (such as 8 for hyperlink)
///
/// @notice multi-character intermediates are intentionally not supported.
///
/// @return the matching FunctionSpec or nullptr if none matched.
inline FunctionSpec const* selectOSCommand(unsigned _id)
{
    return select({FunctionCategory::OSC, 0, _id, 0, 0});
}

using CommandList = std::vector<Command>;

/// Helps constructing VT functions as they're being parsed by the VT parser.
class HandlerContext {
  public:
    using FunctionParam = unsigned int;
    using FunctionParamList = std::vector<std::vector<FunctionParam>>;
    using Intermediaries = std::string;

  protected:
    FunctionParamList parameters_;
    Intermediaries intermediateCharacters_;

  public:
    size_t constexpr static MaxParameters = 16;
    size_t constexpr static MaxSubParameters = 8;

    HandlerContext()
    {
        parameters_.resize(MaxParameters);
        for (auto& param : parameters_)
            param.reserve(MaxSubParameters);
        parameters_.clear();
    }

    FunctionParamList const& parameters() const noexcept { return parameters_; }
    size_t parameterCount() const noexcept { return parameters_.size(); }
    size_t subParameterCount(size_t _index) const noexcept { return parameters_[_index].size() - 1; }

    std::optional<FunctionParam> param_opt(size_t _index) const noexcept
    {
        if (_index < parameters_.size() && parameters_[_index][0])
            return {parameters_[_index][0]};
        else
            return std::nullopt;
    }

    FunctionParam param_or(size_t _index, FunctionParam _defaultValue) const noexcept
    {
        return param_opt(_index).value_or(_defaultValue);
    }

    unsigned int param(size_t _index) const noexcept
    {
        assert(_index < parameters_.size());
        assert(0 < parameters_[_index].size());
        return parameters_[_index][0];
    }

    unsigned int subparam(size_t _index, size_t _subIndex) const noexcept
    {
        assert(_index < parameters_.size());
        assert(_subIndex + 1 < parameters_[_index].size());
        return parameters_[_index][_subIndex + 1];
    }

};

enum class HandlerResult {
    Ok,
    Invalid,
    Unsupported,
};
/// Applies a FunctionSpec to a given context, emitting the respective command.
///
/// A FunctionSelector must have been transformed into a FunctionSpec already.
/// So the idea is:
///     VT sequence -> FunctionSelector -> FunctionSpec -> Command.
HandlerResult apply(FunctionSpec const& _function, HandlerContext const& _context, CommandList& _output);

/// Converts a FunctionSpec with a given context back into a human readable VT sequence.
std::string to_sequence(FunctionSpec const& _func, HandlerContext const& _ctx);

} // end namespace

namespace std {
    template<>
    struct hash<terminal::FunctionSpec> {
        /// This is actually perfect hashing.
        constexpr uint32_t operator()(terminal::FunctionSpec const& _fun) const noexcept {
            return _fun.id();
        }
    };
}

namespace fmt {
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
                case FunctionCategory::ESC: return format_to(ctx.out(), "ESC");
                case FunctionCategory::CSI: return format_to(ctx.out(), "CSI");
                case FunctionCategory::OSC: return format_to(ctx.out(), "OSC");
            }
            return format_to(ctx.out(), "({})", static_cast<int>(value));
        }
    };

    template <>
    struct formatter<terminal::FunctionSpec> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(const terminal::FunctionSpec f, FormatContext& ctx)
        {
            return format_to(
                ctx.out(),
                "{} {} ({}-{}) {} {}",
                f.category,
                f.leader ? f.leader : ' ',
                f.minimumParameters,
                f.maximumParameters,
                f.intermediate ? f.intermediate : ' ',
                f.finalSymbol
            );
        }
    };

    template <>
    struct formatter<terminal::FunctionSelector> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(const terminal::FunctionSelector f, FormatContext& ctx)
        {
            return format_to(
                ctx.out(),
                "{} {} ({}-{}) {} {}",
                f.category,
                f.leader ? f.leader : ' ',
                f.argc,
                f.intermediate ? f.intermediate : ' ',
                f.finalSymbol
            );
        }
    };
}
