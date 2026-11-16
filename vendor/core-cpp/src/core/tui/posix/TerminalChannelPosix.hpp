// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file TerminalChannelPosix.hpp
/// @brief The POSIX channel opened by path, so a test can open a pseudo-terminal's slave by name.
///
/// Internal: not part of the module's public headers. @c openControllingTerminal() is this
/// function on `/dev/tty`.

#include <core/tui/TerminalChannel.hpp>

#include <optional>

namespace core::tui
{

/// @brief Opens the terminal at @p path as a channel.
/// @param path The terminal device to open.
/// @param access The access to grant, or nullopt to derive it: read-write when this process is in
///               the terminal's foreground process group, write-only otherwise.
/// @return The channel, or @c TerminalChannelError::NoTerminal when @p path cannot be opened.
[[nodiscard]] auto openTerminalChannelAt(char const* path, std::optional<ChannelAccess> access)
    -> TerminalChannelResult;

} // namespace core::tui
