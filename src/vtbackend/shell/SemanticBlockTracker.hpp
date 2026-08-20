// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>

namespace vtbackend
{

/// Stores metadata for a single command block tracked via OSC 133 shell integration.
struct CommandBlockInfo
{
    std::optional<std::string> commandLine; ///< From OSC 133;C cmdline_url parameter.
    int exitCode = -1;                      ///< From OSC 133;D parameter (-1 if unknown).
    bool finished = false;                  ///< True after OSC 133;D received.
};

/// SBQUERY (CSI > Ps ; Pn b) query type parameter values.
struct SBQueryType
{
    static constexpr unsigned LastCommand = 1;          ///< Last completed command block.
    static constexpr unsigned LastNumberOfCommands = 2; ///< Last N completed command blocks (Pn = count).
    static constexpr unsigned InProgress = 3;           ///< Current in-progress command.
};

/// Tracks semantic command blocks when DEC mode 2034 (Semantic Block Reader Protocol) is enabled.
///
/// This is a standalone tracker owned by Terminal, called from Screen::processShellIntegration()
/// in addition to the existing ShellIntegration callback. It is only active when mode 2034 is enabled.
class SemanticBlockTracker
{
  public:
    /// 64-bit session token represented as 4 × uint16_t for CSI parameter encoding.
    using Token = std::array<uint16_t, 4>;

    /// Constructs a tracker with a maximum number of completed blocks to retain.
    ///
    /// @param maxBlocks Maximum number of completed command blocks to keep in history.
    explicit SemanticBlockTracker(size_t maxBlocks = 100);

    /// Enables or disables tracking.
    ///
    /// On enable: generates a fresh session token.
    /// On disable: clears all stored data and invalidates the token.
    void setEnabled(bool enabled);

    /// Returns whether tracking is currently enabled.
    [[nodiscard]] bool isEnabled() const noexcept;

    /// Returns the current session token, if mode is enabled.
    [[nodiscard]] std::optional<Token> const& token() const noexcept;

    /// Validates a candidate token against the current session token.
    ///
    /// @param candidate The token to validate.
    /// @return true if the token matches the current session token.
    [[nodiscard]] bool validateToken(Token const& candidate) const noexcept;

    /// Generates a new random 64-bit token using std::random_device.
    [[nodiscard]] static Token generateToken();

    /// Called when OSC 133;A (prompt start) is received.
    void promptStart();

    /// Called when OSC 133;C (command output start) is received.
    ///
    /// @param commandLine The command line from the cmdline_url parameter, if available.
    void commandOutputStart(std::optional<std::string> const& commandLine);

    /// Called when OSC 133;D (command finished) is received.
    ///
    /// @param exitCode The exit code of the finished command.
    void commandFinished(int exitCode);

    /// Returns the list of completed command blocks, oldest first.
    [[nodiscard]] std::deque<CommandBlockInfo> const& completedBlocks() const noexcept;

    /// Returns the currently in-progress command block, if any.
    [[nodiscard]] std::optional<CommandBlockInfo> const& currentBlock() const noexcept;

  private:
    bool _enabled = false;
    std::optional<Token> _token;
    std::deque<CommandBlockInfo> _completedBlocks;
    std::optional<CommandBlockInfo> _currentBlock;
    size_t _maxBlocks;
};

} // namespace vtbackend
