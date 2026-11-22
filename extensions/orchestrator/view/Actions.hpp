// SPDX-License-Identifier: Apache-2.0
//
// The actions: what each command is called, which apply to each row state and in what order. The action bar,
// `⏎`, the boxes at the bottom of the Focus and the command palette all read these tables, so that no action
// is described in two places.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <orchestrator/core/Engine.hpp>
#include <orchestrator/view/Fleet.hpp>
#include <orchestrator/view/Keys.hpp>
#include <orchestrator/view/Sections.hpp>

namespace orchestrator::view
{

/// What a command acts on.
enum class CommandScope : uint8_t
{
    Row = 0, ///< The selected row: it applies where the row's state has an ActionRow for it.
    Global,  ///< The dashboard: it applies wherever the engine allows it.
};

/// One command the palette lists: what it is called, and what it acts on.
struct CommandRow
{
    KeyCommand command = KeyCommand::Open;  ///< The command.
    std::string_view label = {};            ///< What the bar and the palette call it, such as `end`.
    CommandScope scope = CommandScope::Row; ///< What it acts on.
    std::string_view unoffered = {}; ///< A row command: why it does not apply to a row whose state lists it
                                     ///< but which does not offer it (Row::offers).
    /// A global command: why it does not apply now; empty while it does.
    std::string_view (*unavailable)(Engine const* engine) = nullptr;
};

/// @return Every command the palette lists, in the order it lists them.
[[nodiscard]] std::span<CommandRow const> commandRows() noexcept;

/// Where an action is offered besides its key.
enum class Hinted : uint8_t
{
    OnBar = 0,   ///< On the action bar, while it fits, and in the palette.
    PaletteOnly, ///< In the palette only: the bar keeps to what matters most for the state.
};

/// One action a row state offers.
struct ActionRow
{
    RowState when = RowState::None;        ///< The state.
    KeyCommand command = KeyCommand::Open; ///< What it does.
    std::string_view label = {};   ///< What the bar calls it for this state; empty: the command's label.
    Hinted hinted = Hinted::OnBar; ///< Where it is offered besides its key.
};

/// @return Every action, grouped by state, each state's in rank order: its first is its primary action, which
///         `⏎` does and the bar lists first.
[[nodiscard]] std::span<ActionRow const> actionRows() noexcept;

/// @param state A row state.
/// @return Its actions, in rank order; empty for RowState::None.
[[nodiscard]] std::span<ActionRow const> actionsFor(RowState state) noexcept;

/// @param command A command the palette lists.
/// @return Its label, such as `end`; empty for a command that is not listed.
[[nodiscard]] std::string_view labelOf(KeyCommand command) noexcept;

/// @param action An action.
/// @return What the bar calls it: its own label, else its command's.
[[nodiscard]] std::string_view labelOf(ActionRow const& action) noexcept;

/// @param row The selected row; nullptr: none.
/// @return What `⏎` does on it: its state's primary action; std::nullopt where it has none.
[[nodiscard]] std::optional<KeyCommand> primaryAction(Row const* row) noexcept;

/// @param row The selected row; nullptr: none.
/// @param command A command.
/// @param engine The engine; nullptr: none.
/// @return Why @p command does not apply now: no ActionRow for the row's state, the row does not offer it, or
///         the engine does not allow it; empty when it applies, or no table lists it (such as Up).
[[nodiscard]] std::string whyNot(Row const* row, KeyCommand command, Engine const* engine);

/// @param row The selected row; nullptr: none.
/// @param command A command.
/// @param engine The engine; nullptr: none.
/// @return What the bar says when @p command does not apply, such as `e end · no run selected`; empty when it
///         applies.
[[nodiscard]] std::string refusalNotice(Row const* row, KeyCommand command, Engine const* engine);

/// @param row The selected row; nullptr: none.
/// @return The actions that apply to it, as the bar hints them, in rank order: its primary action as
///         `⏎ <label>`, then each other one that has a key as `<key> <label>`.
[[nodiscard]] std::vector<std::string> actionHints(Row const* row);

/// Whether the message box has the keys.
enum class MessageKeys : uint8_t
{
    Elsewhere = 0, ///< The Fleet has them, or the Focus of a run without a message box.
    InBox,         ///< The message box has them: every printable key types.
};

/// @param row The selected row; nullptr: none.
/// @param place Which pane has the keys.
/// @param keys Whether the message box has them.
/// @param columns The cells the line may take.
/// @return The action bar's line, as many whole hints as fit: the message box's keys while it has them;
///         else the row's actions, `esc back` in the Focus and `n new agent` where the row does not offer it
///         already, then `^k all actions`, which always shows.
[[nodiscard]] std::string barText(Row const* row, Place place, MessageKeys keys, int columns);

} // namespace orchestrator::view
