// SPDX-License-Identifier: Apache-2.0
//
// The JSON shapes `contour mcp` reads and writes itself, as opposed to the control API's own records.
// They live in a header because glaze cannot reflect a type in an anonymous namespace.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <vtcontrol/Api.hpp>
#include <vtcontrol/Json.hpp>

namespace contour::cli::wire
{
/// The JSON-RPC message, as far as routing needs it. Unknown members are ignored.
struct Envelope
{
    std::string jsonrpc {};
    vtcontrol::RawJson id {};
    std::string method {};
    vtcontrol::RawJson params {};
};

struct InitializeParams
{
    std::string protocolVersion {};
};

/// Arguments of the composite tools; a member without a default is required.
struct SplitArguments
{
    std::string pane {};
    std::optional<vtcontrol::SplitDirection> direction {};
    std::optional<uint8_t> ratioPercent {};
    std::string cwd {};
    std::vector<std::string> command {};
    std::string label {};
};

struct TypeArguments
{
    std::string pane {};
    std::optional<std::string> text {};
};

struct RunArguments
{
    std::string pane {};
    std::optional<std::string> command {};
    std::optional<uint32_t> timeoutSeconds {};
    std::optional<uint32_t> maxOutputBytes {};
};

struct TabCreateArguments
{
    std::string cwd {};
    std::vector<std::string> command {};
    std::string label {};
};

/// What `pane_run` returns.
struct RunResult
{
    int32_t exitCode = 0;
    std::string output {};
    std::optional<uint32_t> durationMs {};
    std::string blockId {};
    vtcontrol::Truncation truncated = vtcontrol::Truncation::None;
};

struct CallParams
{
    std::string name {};
    vtcontrol::RawJson arguments {};
};
} // namespace contour::cli::wire
