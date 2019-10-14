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

#include <terminal/Commands.h>
#include <terminal/VTType.h>

#include <functional>
#include <optional>
#include <string_view>
#include <string>
#include <unordered_map>
#include <vector>

namespace terminal {

enum class FunctionType {
	ESC = 0,
	CSI = 1,
	OSC = 2,
};

struct FunctionDef {
	FunctionType type;                  // ESC, CSI, OSC
    std::optional<char> leaderSymbol;   // < ?
    std::optional<char> followerSymbol; // $
    char finalSymbol;

    VTType conformanceLevel;

    std::string_view mnemonic;
    std::string_view comment;

    constexpr static uint32_t makeId(FunctionType _type,
									 char _leaderSymbol,
									 char _followerSymbol,
									 char _finalSymbol) noexcept {
        return _finalSymbol | _followerSymbol << 8 | _leaderSymbol << 16
			 | static_cast<unsigned>(_type) << 18;
    }

    constexpr uint32_t id() const noexcept {
        return makeId(type, leaderSymbol.value_or(0), followerSymbol.value_or(0), finalSymbol);
    }

    constexpr operator uint32_t () const noexcept { return id(); }
};

constexpr bool operator<(FunctionDef const& a, FunctionDef const& b) noexcept
{
	return a.id() < b.id();
}

using FunctionParam = unsigned int;
using FunctionParamList = std::vector<FunctionParam>;
using Intermediaries = std::string;
using CommandList = std::vector<Command>;

enum class HandlerResult {
	Ok,
	Invalid,
	Unsupported,
};

struct HandlerContext {
	FunctionParamList const& parameters;
	Intermediaries const& intermediateCharacters;
	CommandList& output;

	size_t parameterCount() const noexcept { return parameters.size(); }

	FunctionParam param_or(size_t _index, FunctionParam _defaultValue) const noexcept
	{
		if (_index < parameters.size() && parameters[_index])
			return parameters[_index];
		else
			return _defaultValue;
	}

    unsigned int param(size_t _index) const noexcept
    {
		return parameters[_index];
    }

    template <typename T, typename... Args>
    HandlerResult emit(Args&&... args)
    {
        output.emplace_back(T{std::forward<Args>(args)...});
		return HandlerResult::Ok;
    }
};

using FunctionHandler = std::function<HandlerResult(HandlerContext&)>;
using FunctionHandlerMap = std::unordered_map<uint32_t, std::pair<FunctionDef, FunctionHandler>>;

FunctionHandlerMap functions(VTType _vt);

std::string to_sequence(FunctionDef const& _func, HandlerContext const& _ctx);

// ==========================================================================================
// below is the deprecated code

constexpr FunctionDef CSI(
	std::optional<char> _leader, std::optional<char> _follower, char _final,
	VTType _vt, std::string_view _mnemonic, std::string_view _comment) noexcept
{
	return FunctionDef{
		FunctionType::CSI,
		_leader, _follower, _final, _vt,
		_mnemonic, _comment
	};
}

// CSI codes
constexpr auto CHA = CSI(std::nullopt, std::nullopt, 'G', VTType::VT100, "CHA", "Move cursor to column");
constexpr auto CNL = CSI(std::nullopt, std::nullopt, 'E', VTType::VT100, "CNL", "Move cursor to next line");
constexpr auto CPL = CSI(std::nullopt, std::nullopt, 'F', VTType::VT100, "CPL", "Move cursor to previous line");
constexpr auto CPR = CSI(std::nullopt, std::nullopt, 'n', VTType::VT100, "CPR", "Request Cursor position");
constexpr auto CUB = CSI(std::nullopt, std::nullopt, 'D', VTType::VT100, "CUB", "Move cursor backward");
constexpr auto CUD = CSI(std::nullopt, std::nullopt, 'B', VTType::VT100, "CUD", "Move cursor down");
constexpr auto CUF = CSI(std::nullopt, std::nullopt, 'C', VTType::VT100, "CUF", "Move cursor forward");
constexpr auto CUP = CSI(std::nullopt, std::nullopt, 'H', VTType::VT100, "CUP", "Move cursor to position");
constexpr auto CUU = CSI(std::nullopt, std::nullopt, 'A', VTType::VT100, "CUU", "Move cursor up");
constexpr auto DA1 = CSI(std::nullopt, std::nullopt, 'c', VTType::VT100, "DA1", "Send primary device attributes");
constexpr auto DA2 = CSI('>', std::nullopt, 'c', VTType::VT100, "DA2", "Send secondary device attributes");
constexpr auto DCH = CSI(std::nullopt, std::nullopt, 'P', VTType::VT100, "DCH", "Delete characters");
constexpr auto DECDC = CSI('\'', std::nullopt, '~', VTType::VT420, "DECDC", "Delete column");
constexpr auto DECIC = CSI('\'', std::nullopt, '}', VTType::VT420, "DECIC", "Insert column");
constexpr auto DECRM = CSI('?', std::nullopt, 'l', VTType::VT100, "DECRM", "Reset DEC-mode");
constexpr auto DECRQM = CSI('?', '$', 'p', VTType::VT100, "DECRQM", "Request DEC-mode");
constexpr auto DECRQM_ANSI = CSI(std::nullopt, '$', 'p', VTType::VT100, "DECRQM_ANSI", "Request ANSI-mode");
constexpr auto DECSCUSR = CSI(std::nullopt, ' ', 'q', VTType::VT100, "DECSCUSR", "Set Cursor Style");
constexpr auto DECSLRM = CSI(std::nullopt, std::nullopt, 's', VTType::VT420, "DECSLRM", "Set left/right margin");
constexpr auto DECSM = CSI('?', std::nullopt, 'h', VTType::VT100, "DECSM", "Set DEC-mode");
constexpr auto DECSTBM = CSI(std::nullopt, std::nullopt, 'r', VTType::VT100, "DECSTBM", "Set top/bottom margin");
constexpr auto DECSTR = CSI('!', std::nullopt, 'p', VTType::VT100, "DECSTR", "Soft terminal reset");
constexpr auto DECXCPR = CSI(std::nullopt, std::nullopt, '6', VTType::VT100, "DECXCPR", "Request extended cursor position");
constexpr auto DL  = CSI(std::nullopt, std::nullopt, 'M', VTType::VT100, "DL",  "Delete lines");
constexpr auto ECH = CSI(std::nullopt, std::nullopt, 'X', VTType::VT420, "ECH", "Erase characters");
constexpr auto ED  = CSI(std::nullopt, std::nullopt, 'J', VTType::VT100, "ED",  "Erase in display");
constexpr auto EL  = CSI(std::nullopt, std::nullopt, 'K', VTType::VT100, "EL",  "Erase in line");
constexpr auto HPA = CSI(std::nullopt, std::nullopt, '`', VTType::VT100, "HPA", "Horizontal position absolute");
constexpr auto HPR = CSI(std::nullopt, std::nullopt, 'a', VTType::VT100, "HPR", "Horizontal position relative");
constexpr auto HVP = CSI(std::nullopt, std::nullopt, 'f', VTType::VT100, "HVP", "Horizontal and vertical position");
constexpr auto ICH = CSI(std::nullopt, std::nullopt, '@', VTType::VT420, "ICH", "Insert character");
constexpr auto IL  = CSI(std::nullopt, std::nullopt, 'L', VTType::VT100, "IL",  "Insert lines");
constexpr auto RM  = CSI(std::nullopt, std::nullopt, 'l', VTType::VT100, "RM",  "Reset mode");
constexpr auto SD  = CSI(std::nullopt, std::nullopt, 'T', VTType::VT100, "SD",  "Scroll down (pan up)");
constexpr auto SGR = CSI(std::nullopt, std::nullopt, 'm', VTType::VT100, "SGR", "Select graphics rendition");
constexpr auto SM  = CSI(std::nullopt, std::nullopt, 'h', VTType::VT100, "SM",  "Set mode");
constexpr auto SU  = CSI(std::nullopt, std::nullopt, 'S', VTType::VT100, "SU",  "Scroll up (pan down)");
constexpr auto VPA = CSI(std::nullopt, std::nullopt, 'd', VTType::VT100, "VPA", "Vertical Position Absolute");

} // namespace terminal

namespace std {
	template<>
	struct hash<terminal::FunctionDef> {
		constexpr uint32_t operator()(terminal::FunctionDef const& _fun) const noexcept {
			return _fun.id();
		}
	};
}
