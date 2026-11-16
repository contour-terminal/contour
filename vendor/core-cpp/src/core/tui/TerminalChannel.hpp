// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file TerminalChannel.hpp
/// @brief A short-lived connection to the controlling terminal, apart from standard I/O.
///
/// @c TerminalInput and @c TerminalOutput are standard input and output. A program whose output is
/// redirected (`clip foo > file`, `x=$(clip foo)`) still has a terminal to talk to: the one it is
/// attached to. A channel opens that terminal for the length of one exchange -- write a query or a
/// command, read the replies -- and gives it back as it found it.

#include <core/tui/InputEvent.hpp>

#include <cstdint>
#include <expected>
#include <memory>
#include <string_view>
#include <vector>

namespace core::tui
{

/// @brief What a channel may do with its terminal.
enum class ChannelAccess : std::uint8_t
{
    ReadWrite, ///< Writes, and reads the terminal's replies.
    WriteOnly, ///< Writes only: the process is not in the terminal's foreground process group, and
               ///< a read would stop it with SIGTTIN.
};

/// @brief Why a channel could not be opened, written or read.
enum class TerminalChannelError : std::uint8_t
{
    NoTerminal, ///< There is no terminal to open.
    IoError,    ///< Reading or writing the terminal failed.
};

/// @brief A connection to a terminal: bytes to it, decoded replies from it.
class TerminalChannel
{
  public:
    TerminalChannel() = default;
    virtual ~TerminalChannel() = default;
    TerminalChannel(TerminalChannel const&) = delete;
    auto operator=(TerminalChannel const&) -> TerminalChannel& = delete;
    TerminalChannel(TerminalChannel&&) = delete;
    auto operator=(TerminalChannel&&) -> TerminalChannel& = delete;

    /// @brief Whether replies can be read from this channel.
    [[nodiscard]] virtual auto access() const noexcept -> ChannelAccess = 0;

    /// @brief Writes @p bytes to the terminal, all of them.
    /// @param bytes The bytes to write.
    /// @return Nothing, or @c TerminalChannelError::IoError.
    [[nodiscard]] virtual auto write(std::string_view bytes) -> std::expected<void, TerminalChannelError> = 0;

    /// @brief Waits up to @p timeoutMs for input and decodes it, OSC replies included.
    ///
    /// May return early with nothing (a signal, half of a sequence); the caller's deadline decides
    /// when waiting is over.
    /// @param timeoutMs The longest to wait, in milliseconds.
    /// @return The decoded events, possibly none, or @c TerminalChannelError::IoError.
    [[nodiscard]] virtual auto poll(int timeoutMs)
        -> std::expected<std::vector<InputEvent>, TerminalChannelError> = 0;

    /// @brief Drops the terminal's input that is queued and not yet read.
    ///
    /// For a caller about to wait for a reply: a reply to an earlier exchange that gave up waiting
    /// may still be queued, and read now it would answer this one. Keys typed ahead go with it.
    /// A write-only channel has nothing to drop.
    virtual void discardPendingInput() = 0;
};

/// @brief An open channel, or why there is none.
using TerminalChannelResult = std::expected<std::unique_ptr<TerminalChannel>, TerminalChannelError>;

/// @brief Opens the process's controlling terminal: `/dev/tty` on POSIX, the console on Windows.
///
/// While the channel is open on POSIX, the terminal neither echoes nor buffers lines (ICANON and
/// ECHO are cleared; ISIG is kept, so Ctrl+C still interrupts); the destructor restores what it
/// found. A process outside the terminal's foreground process group gets a @c WriteOnly channel.
/// @return The channel, or @c TerminalChannelError::NoTerminal when there is no terminal to open.
[[nodiscard]] auto openControllingTerminal() -> TerminalChannelResult;

} // namespace core::tui
