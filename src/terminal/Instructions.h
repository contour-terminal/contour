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

struct InstructionDef {
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

constexpr InstructionDef CHA{std::nullopt, std::nullopt, 'G', VTType::VT100, "CHA", "Move cursor to column"};
constexpr InstructionDef CNL{std::nullopt, std::nullopt, 'E', VTType::VT100, "CNL", "Move cursor to next line"};
constexpr InstructionDef CPL{std::nullopt, std::nullopt, 'F', VTType::VT100, "CPL", "Move cursor to previous line"};
constexpr InstructionDef CPR{std::nullopt, std::nullopt, 'n', VTType::VT100, "CPR", "Request Cursor position"};
constexpr InstructionDef CUB{std::nullopt, std::nullopt, 'D', VTType::VT100, "CUB", "Move cursor backward"};
constexpr InstructionDef CUD{std::nullopt, std::nullopt, 'B', VTType::VT100, "CUD", "Move cursor down"};
constexpr InstructionDef CUF{std::nullopt, std::nullopt, 'C', VTType::VT100, "CUF", "Move cursor forward"};
constexpr InstructionDef CUP{std::nullopt, std::nullopt, 'H', VTType::VT100, "CUP", "Move cursor to position"};
constexpr InstructionDef CUU{std::nullopt, std::nullopt, 'A', VTType::VT100, "CUU", "Move cursor up"};
constexpr InstructionDef DA1{std::nullopt, std::nullopt, 'c', VTType::VT100, "DA1", "Send primary device attributes"};
constexpr InstructionDef DA2{'>', std::nullopt, 'c', VTType::VT100, "DA2", "Send secondary device attributes"};
constexpr InstructionDef DCH{std::nullopt, std::nullopt, 'P', VTType::VT100, "DCH", "Delete characters"};
constexpr InstructionDef DECDC{'\'', std::nullopt, '~', VTType::VT100, "DECDC", "Delete column"};
constexpr InstructionDef DECIC{'\'', std::nullopt, '}', VTType::VT100, "DECIC", "Insert column"};
constexpr InstructionDef DECRM{'?', std::nullopt, 'l', VTType::VT100, "DECRM", "Reset DEC-mode"};
constexpr InstructionDef DECRQM_ANSI{std::nullopt, std::nullopt, 'p', VTType::VT100, "DECRQM", "Request ANSI-mode"};
constexpr InstructionDef DECRQM{'?', std::nullopt, 'p', VTType::VT100, "DECRQM_ANSI", "Request DEC-mode"};
constexpr InstructionDef DECSLRM{std::nullopt, std::nullopt, 's', VTType::VT100, "DECSLRM", "Set left/right margin"};
constexpr InstructionDef DECSM{'?', std::nullopt, 'h', VTType::VT100, "DECSM", "Set DEC-mode"};
constexpr InstructionDef DECSTBM{std::nullopt, std::nullopt, 'r', VTType::VT100, "DECSTBM", "Set top/bottom margin"};
constexpr InstructionDef DECSTR{'!', std::nullopt, 'p', VTType::VT100, "DECSTR", "Soft terminal reset"};
constexpr InstructionDef DECXCPR{std::nullopt, std::nullopt, '6', VTType::VT100, "DECXCPR", "Request extended cursor position"};
constexpr InstructionDef DL {std::nullopt, std::nullopt, 'M', VTType::VT100, "DL",  "Delete lines"};
constexpr InstructionDef ECH{std::nullopt, std::nullopt, 'X', VTType::VT100, "ECH", "Erase characters"};
constexpr InstructionDef ED {std::nullopt, std::nullopt, 'J', VTType::VT100, "ED",  "Erase in display"};
constexpr InstructionDef EL {std::nullopt, std::nullopt, 'K', VTType::VT100, "EL",  "Erase in line"};
constexpr InstructionDef HPA{std::nullopt, std::nullopt, '`', VTType::VT100, "HPA", "Horizontal position absolute"};
constexpr InstructionDef HPR{std::nullopt, std::nullopt, 'a', VTType::VT100, "HPR", "Horizontal position relative"};
constexpr InstructionDef ICH{std::nullopt, std::nullopt, '@', VTType::VT100, "ICH", "Insert character"};
constexpr InstructionDef IL {std::nullopt, std::nullopt, 'L', VTType::VT100, "IL",  "Insert lines"};
constexpr InstructionDef RM {std::nullopt, std::nullopt, 'l', VTType::VT100, "RM",  "Reset mode"};
constexpr InstructionDef SD {std::nullopt, std::nullopt, 'T', VTType::VT100, "SD",  "Scroll down (pan up)"};
constexpr InstructionDef SGR{std::nullopt, std::nullopt, 'm', VTType::VT100, "SGR", "Select graphics rendition"};
constexpr InstructionDef SM {std::nullopt, std::nullopt, 'h', VTType::VT100, "SM",  "Set mode"};
constexpr InstructionDef SU {std::nullopt, std::nullopt, 'S', VTType::VT100, "SU",  "Scroll up (pan down)"};
constexpr InstructionDef VPA{std::nullopt, std::nullopt, 'd', VTType::VT100, "VPA", "Vertical Position Absolute"};

} // namespace terminal
