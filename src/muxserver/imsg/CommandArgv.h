// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// MSG_COMMAND's payload: `struct msg_command { int argc; }` followed by the
/// argv strings packed NUL-separated (tmux's cmd_pack_argv/cmd_unpack_argv).

#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include <muxserver/imsg/ImsgCodec.h>

namespace muxserver::imsg
{

/// tmux rejects argc outside [0, 1000] (cmd_unpack_argv).
inline constexpr int MaxArgc = 1000;

/// Packs @p arguments into the MSG_COMMAND payload layout.
[[nodiscard]] std::vector<std::byte> packArgv(std::span<std::string const> arguments);

/// Unpacks a MSG_COMMAND payload per cmd_unpack_argv's rules: argc 0 is
/// legal (an empty command runs the server default), argc outside [0, 1000]
/// or a truncated string list is rejected.
[[nodiscard]] std::expected<std::vector<std::string>, ImsgError> unpackArgv(
    std::span<std::byte const> payload);

} // namespace muxserver::imsg
