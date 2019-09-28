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
    std::optional<char> const leaderSymbol;
    std::optional<char> const followerSymbol;
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

constexpr InstructionDef CUP{std::nullopt, std::nullopt, 'A', VTType::VT100, "CUP", "Move cursor up"};
constexpr InstructionDef CUD{std::nullopt, std::nullopt, 'B', VTType::VT100, "CUD", "Move cursor down"};

} // namespace terminal
