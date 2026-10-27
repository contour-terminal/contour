// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/shell/CommandBlock.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace contour::session
{

/// What the gutter's block-column tooltip says about @p record: how it ended -- exit code and signal --,
/// how long it took (@see vtbackend::formatCommandDuration), when it started, where it ran and what it
/// was, each line only when the record knows it.
///
/// The command line and the directory are sanitised for display (@see vtbackend::sanitizeCommandLine):
/// both are whatever the shell, or any program writing to the PTY, reported.
/// @param record The block under the pointer.
/// @param startedAt When it started, in local civil time (@see vtbackend::Terminal::localTimeOf): its
///        command's start, else its prompt's. Not shown for a record that knows neither.
/// @return The text, one fact per line; empty for a prompt still being typed at, which has none.
[[nodiscard]] std::string commandBlockTooltipText(vtbackend::CommandBlockRecord const& record,
                                                  std::chrono::local_seconds startedAt);

/// The lines that head a block's menu: its command, how it ended and for how long (@see
/// vtbackend::blockStatusText), where it ran and when it started, each only when the record knows it.
///
/// The command line and the directory are sanitised for display, as in commandBlockTooltipText().
/// @param record The block the menu was opened for.
/// @param startedAt When it started, in local civil time; not shown for a record that knows no start.
/// @param now The wall clock now, what a running command's time counts to.
/// @return The lines; none for a prompt still being typed at.
[[nodiscard]] std::vector<std::string> commandBlockHeaderLines(vtbackend::CommandBlockRecord const& record,
                                                               std::chrono::local_seconds startedAt,
                                                               std::chrono::system_clock::time_point now);

/// What a click on the gutter's block column does for the hovered block, as its tooltip's last line says.
enum class BlockTooltipHint : uint8_t
{
    RightClickOnly =
        0,    ///< A left click does nothing there (folding is off): only the block's menu is offered.
    Menu,     ///< Its status mark, with nothing to fold: a click opens the block's menu.
    Collapse, ///< Its fold, expanded: a click collapses it.
    Expand,   ///< Its fold, collapsed: a click expands it.
};

/// The tooltip line that says what @p hint offers: what a reader cannot otherwise guess from a mark.
/// @param hint What a click does.
/// @return The line, without a line break.
[[nodiscard]] std::string_view blockTooltipHintText(BlockTooltipHint hint) noexcept;

} // namespace contour::session
