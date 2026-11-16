// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace core::tui
{

/// @brief Specifies which mode(s) a command is available in.
enum class CommandContext : std::uint8_t
{
    Shell, ///< Command available only in shell mode.
    Agent, ///< Command available only in agent mode.
    Both,  ///< Command available in both modes.
};

/// @brief A single command entry in the command palette.
struct CommandEntry
{
    std::string id;               ///< Unique identifier, e.g. "shell.go_to_directory".
    std::string label;            ///< Display label, e.g. "Go to Directory".
    std::string description;      ///< Short description shown next to label.
    std::string category;         ///< Category grouping, e.g. "Navigation", "Session".
    std::string keybinding;       ///< Display hint for keybinding, e.g. "Ctrl+T".
    CommandContext context;       ///< Which mode(s) this command is available in.
    std::function<void()> action; ///< Callback invoked when the command is executed.
};

/// @brief Registry of commands available in the command palette.
///
/// Provides a data-driven, centralized store of palette commands. Commands
/// can be added and removed dynamically, and filtered by context (Shell/Agent/Both).
class CommandRegistry
{
  public:
    CommandRegistry() = default;

    /// @brief Adds a command entry to the registry.
    /// @param entry The command to register.
    void add(CommandEntry entry);

    /// @brief Removes a command by its unique identifier.
    /// @param id The command ID to remove.
    void remove(std::string_view id);

    /// @brief Returns all registered commands.
    [[nodiscard]] auto commands() const -> std::span<CommandEntry const>;

    /// @brief Returns commands filtered by the given context.
    ///
    /// Returns pointers to commands that match the given context or have
    /// CommandContext::Both.
    /// @param ctx The context to filter by.
    /// @return Vector of pointers to matching commands.
    [[nodiscard]] auto commandsForContext(CommandContext ctx) const -> std::vector<CommandEntry const*>;

    /// @brief Finds a command by its unique identifier.
    /// @param id The command ID to look up.
    /// @return Pointer to the command, or nullptr if not found.
    [[nodiscard]] auto find(std::string_view id) const -> CommandEntry const*;

    /// @brief Returns the number of registered commands.
    [[nodiscard]] auto size() const noexcept -> std::size_t;

    /// @brief Returns whether the registry is empty.
    [[nodiscard]] auto empty() const noexcept -> bool;

  private:
    std::vector<CommandEntry> _commands;
};

} // namespace core::tui
