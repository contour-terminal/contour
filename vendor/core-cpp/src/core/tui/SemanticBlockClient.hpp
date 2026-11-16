// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/Clock.hpp>
#include <core/Defines.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace core::tui
{

class TerminalOutput;
class TerminalInput;

/// @brief Information about a completed command block from the terminal.
struct CommandBlockInfo
{
    std::string command;     ///< The command line text.
    std::string output;      ///< The command's stdout/stderr output.
    int exitCode = -1;       ///< Exit code (-1 if unknown).
    bool finished = true;    ///< False for in-progress commands.
    int outputLineCount = 0; ///< Number of output lines.
};

/// @brief Status codes from semantic block query responses.
enum class SemanticBlockStatus : std::uint8_t
{
    Success,      ///< Query succeeded, block data available.
    NoData,       ///< No matching data found (status 0).
    AuthRequired, ///< Authentication required (status 2).
    AuthFailed,   ///< Authentication failed (status 3).
    Unsupported,  ///< Terminal does not support DEC mode 2034.
    Timeout,      ///< Response timed out.
    ParseError,   ///< Response received but could not be parsed.
};

/// @brief Result of a semantic block query.
struct SemanticBlockResult
{
    SemanticBlockStatus status = SemanticBlockStatus::Unsupported; ///< Status of the query.
    std::optional<CommandBlockInfo> block; ///< Block data (only when status == Success).
};

/// @brief Client for the terminal Semantic Block Query extension (DEC mode 2034).
///
/// Encapsulates the lifecycle of enabling/disabling the mode and querying the
/// terminal for information about the last command block (command text, output, exit code).
/// Uses DCS sequences for communication.
///
/// @note This extension is specific to Contour terminal emulator.
class SemanticBlockClient
{
  public:
    /// @brief Constructs a SemanticBlockClient with the given terminal I/O handles.
    /// @param output Terminal output for sending escape sequences.
    /// @param input Terminal input for receiving responses.
    /// @param clock The clock reply deadlines are measured on (not owned; must outlive the client).
    SemanticBlockClient(TerminalOutput& output,
                        TerminalInput& input,
                        IClock const& clock CORE_LIFETIMEBOUND = defaultSteadyClock());

    /// A clock is kept, not copied: a temporary would dangle, so an rvalue clock is refused.
    SemanticBlockClient(TerminalOutput& output, TerminalInput& input, IClock const&& clock) = delete;

    /// @brief Enables DEC mode 2034 and retrieves the session token.
    ///
    /// Sends CSI ?2034h to the terminal and waits for a DCS token response.
    /// @return True if the mode was enabled and a token was received, false on timeout.
    [[nodiscard]] auto enable() -> bool;

    /// @brief Disables DEC mode 2034 and clears the session token.
    void disable();

    /// @brief Returns whether the mode is active with a valid session token.
    [[nodiscard]] auto isEnabled() const noexcept -> bool;

    /// @brief Queries the terminal for the last command block.
    ///
    /// Sends a semantic block query using the stored token and waits for the response.
    /// @return The query result with status and optional block data.
    [[nodiscard]] auto queryLastCommand() -> SemanticBlockResult;

    /// @brief Parses a JSON payload from the semantic block query response.
    /// @param json The JSON string to parse.
    /// @return Parsed CommandBlockInfo, or std::nullopt on parse failure.
    [[nodiscard]] static auto parseBlockJson(std::string_view json) -> std::optional<CommandBlockInfo>;

  private:
    TerminalOutput& _output; ///< Terminal output handle.
    TerminalInput& _input;   ///< Terminal input handle.
    IClock const& _clock;    ///< The clock reply deadlines are measured on.
    std::string _token;      ///< Session token from the enable response.

    /// @brief Polls for a DCS response with deadline-based timeout.
    /// @param timeoutMs Maximum time to wait in milliseconds.
    /// @return The DCS payload, or std::nullopt on timeout.
    [[nodiscard]] auto pollForDcs(int timeoutMs) -> std::optional<std::string>;
};

} // namespace core::tui
