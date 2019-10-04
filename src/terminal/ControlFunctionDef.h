/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019 Christian Parpart <christian@parpart.family>
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
#include <optional>
#include <string_view>
#include <string>

namespace terminal {

struct ControlFunctionDef {
    std::optional<char> const leaderSymbol;   // < ?
    std::optional<char> const followerSymbol; // $
    char const finalSymbol;
    VTType const conformanceLevel;
    std::string_view const mnemonic;
    std::string_view const comment;

    constexpr static uint32_t makeId(char _leaderSymbol, char _followerSymbol, char _finalSymbol) noexcept {
        return _finalSymbol | _followerSymbol << 8 | _leaderSymbol << 16;
    }

    constexpr uint32_t id() const noexcept {
        return makeId(leaderSymbol.value_or(0), followerSymbol.value_or(0), finalSymbol);
    }

    constexpr operator uint32_t () const noexcept { return id(); }
};

constexpr ControlFunctionDef CHA{std::nullopt, std::nullopt, 'G', VTType::VT100, "CHA", "Move cursor to column"};
constexpr ControlFunctionDef CNL{std::nullopt, std::nullopt, 'E', VTType::VT100, "CNL", "Move cursor to next line"};
constexpr ControlFunctionDef CPL{std::nullopt, std::nullopt, 'F', VTType::VT100, "CPL", "Move cursor to previous line"};
constexpr ControlFunctionDef CPR{std::nullopt, std::nullopt, 'n', VTType::VT100, "CPR", "Request Cursor position"};
constexpr ControlFunctionDef CUB{std::nullopt, std::nullopt, 'D', VTType::VT100, "CUB", "Move cursor backward"};
constexpr ControlFunctionDef CUD{std::nullopt, std::nullopt, 'B', VTType::VT100, "CUD", "Move cursor down"};
constexpr ControlFunctionDef CUF{std::nullopt, std::nullopt, 'C', VTType::VT100, "CUF", "Move cursor forward"};
constexpr ControlFunctionDef CUP{std::nullopt, std::nullopt, 'H', VTType::VT100, "CUP", "Move cursor to position"};
constexpr ControlFunctionDef CUU{std::nullopt, std::nullopt, 'A', VTType::VT100, "CUU", "Move cursor up"};
constexpr ControlFunctionDef DA1{std::nullopt, std::nullopt, 'c', VTType::VT100, "DA1", "Send primary device attributes"};
constexpr ControlFunctionDef DA2{'>', std::nullopt, 'c', VTType::VT100, "DA2", "Send secondary device attributes"};
constexpr ControlFunctionDef DCH{std::nullopt, std::nullopt, 'P', VTType::VT100, "DCH", "Delete characters"};
constexpr ControlFunctionDef DECDC{'\'', std::nullopt, '~', VTType::VT420, "DECDC", "Delete column"};
constexpr ControlFunctionDef DECIC{'\'', std::nullopt, '}', VTType::VT420, "DECIC", "Insert column"};
constexpr ControlFunctionDef DECRM{'?', std::nullopt, 'l', VTType::VT100, "DECRM", "Reset DEC-mode"};
constexpr ControlFunctionDef DECRQM_ANSI{std::nullopt, '$', 'p', VTType::VT100, "DECRQM_ANSI", "Request ANSI-mode"};
constexpr ControlFunctionDef DECRQM{'?', '$', 'p', VTType::VT100, "DECRQM", "Request DEC-mode"};
constexpr ControlFunctionDef DECSLRM{std::nullopt, std::nullopt, 's', VTType::VT420, "DECSLRM", "Set left/right margin"};
constexpr ControlFunctionDef DECSM{'?', std::nullopt, 'h', VTType::VT100, "DECSM", "Set DEC-mode"};
constexpr ControlFunctionDef DECSTBM{std::nullopt, std::nullopt, 'r', VTType::VT100, "DECSTBM", "Set top/bottom margin"};
constexpr ControlFunctionDef DECSTR{'!', std::nullopt, 'p', VTType::VT100, "DECSTR", "Soft terminal reset"};
constexpr ControlFunctionDef DECXCPR{std::nullopt, std::nullopt, '6', VTType::VT100, "DECXCPR", "Request extended cursor position"};
constexpr ControlFunctionDef DL {std::nullopt, std::nullopt, 'M', VTType::VT100, "DL",  "Delete lines"};
constexpr ControlFunctionDef ECH{std::nullopt, std::nullopt, 'X', VTType::VT420, "ECH", "Erase characters"};
constexpr ControlFunctionDef ED {std::nullopt, std::nullopt, 'J', VTType::VT100, "ED",  "Erase in display"};
constexpr ControlFunctionDef EL {std::nullopt, std::nullopt, 'K', VTType::VT100, "EL",  "Erase in line"};
constexpr ControlFunctionDef HPA{std::nullopt, std::nullopt, '`', VTType::VT100, "HPA", "Horizontal position absolute"};
constexpr ControlFunctionDef HPR{std::nullopt, std::nullopt, 'a', VTType::VT100, "HPR", "Horizontal position relative"};
constexpr ControlFunctionDef HVP{std::nullopt, std::nullopt, 'f', VTType::VT100, "HVP", "Horizontal and vertical position"};
constexpr ControlFunctionDef ICH{std::nullopt, std::nullopt, '@', VTType::VT420, "ICH", "Insert character"};
constexpr ControlFunctionDef IL {std::nullopt, std::nullopt, 'L', VTType::VT100, "IL",  "Insert lines"};
constexpr ControlFunctionDef RM {std::nullopt, std::nullopt, 'l', VTType::VT100, "RM",  "Reset mode"};
constexpr ControlFunctionDef SD {std::nullopt, std::nullopt, 'T', VTType::VT100, "SD",  "Scroll down (pan up)"};
constexpr ControlFunctionDef SGR{std::nullopt, std::nullopt, 'm', VTType::VT100, "SGR", "Select graphics rendition"};
constexpr ControlFunctionDef SM {std::nullopt, std::nullopt, 'h', VTType::VT100, "SM",  "Set mode"};
constexpr ControlFunctionDef SU {std::nullopt, std::nullopt, 'S', VTType::VT100, "SU",  "Scroll up (pan down)"};
constexpr ControlFunctionDef VPA{std::nullopt, std::nullopt, 'd', VTType::VT100, "VPA", "Vertical Position Absolute"};

// xterm extensions
constexpr ControlFunctionDef WINMANIP{std::nullopt, std::nullopt, 't', VTType::VT525, "WINMANIP", "Window Manipulation"};

ControlFunctionDef const* controlFunctionById(uint32_t _id) noexcept;

} // namespace terminal
