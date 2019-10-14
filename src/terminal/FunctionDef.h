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

class HandlerContext {
  protected:
	FunctionParamList parameters_;
	Intermediaries intermediateCharacters_;
	CommandList commands_;

  public:
    size_t constexpr static MaxParameters = 16;

	HandlerContext()
	{
		parameters_.reserve(MaxParameters);
	}

	FunctionParamList const& parameters() const noexcept { return parameters_; }

	size_t parameterCount() const noexcept { return parameters_.size(); }

	std::optional<FunctionParam> param_opt(size_t _index) const noexcept
	{
		if (_index < parameters_.size() && parameters_[_index])
			return {parameters_[_index]};
		else
			return std::nullopt;
	}

	FunctionParam param_or(size_t _index, FunctionParam _defaultValue) const noexcept
	{
		return param_opt(_index).value_or(_defaultValue);
	}

    unsigned int param(size_t _index) const noexcept
    {
		return parameters_[_index];
    }

    template <typename T, typename... Args>
    HandlerResult emit(Args&&... args)
    {
        commands_.emplace_back(T{std::forward<Args>(args)...});
        // TODO: telemetry_.increment(fmt::format("{}.{}", "Command", typeid(T).name()));
		return HandlerResult::Ok;
    }

    std::vector<Command>& commands() noexcept { return commands_; }
    std::vector<Command> const& commands() const noexcept { return commands_; }
};

using FunctionHandler = std::function<HandlerResult(HandlerContext&)>;
using FunctionHandlerMap = std::unordered_map<uint32_t, std::pair<FunctionDef, FunctionHandler>>;

FunctionHandlerMap functions(VTType _vt);

std::string to_sequence(FunctionDef const& _func, HandlerContext const& _ctx);

} // namespace terminal

namespace std {
	template<>
	struct hash<terminal::FunctionDef> {
		constexpr uint32_t operator()(terminal::FunctionDef const& _fun) const noexcept {
			return _fun.id();
		}
	};
}
