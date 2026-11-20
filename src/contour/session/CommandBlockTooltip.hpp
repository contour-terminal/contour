// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/shell/BlockStatusText.hpp>
#include <vtbackend/shell/CommandBlock.hpp>

#include <chrono>
#include <ranges>
#include <string>
#include <vector>

namespace contour::session
{

/// The lines that head a block's menu: its command, how it ended and for how long (@see
/// vtbackend::blockStatusText), where it ran and when it started, each only when the record knows it.
///
/// The command line and the directory are sanitised for display (@see vtbackend::sanitizeCommandLine): both
/// are whatever the shell, or any program writing to the PTY, reported.
/// @param record The block the menu was opened for.
/// @param startedAt When it started, in local civil time; not shown for a record that knows no start.
/// @param now The wall clock now, what a running command's time counts to.
/// @return The lines; none for a prompt still being typed at.
[[nodiscard]] std::vector<std::string> commandBlockHeaderLines(vtbackend::CommandBlockRecord const& record,
                                                               std::chrono::local_seconds startedAt,
                                                               std::chrono::system_clock::time_point now);

/// What a tab's hover card says of the commands in its active pane, beside the title and directory it
/// already shows.
struct TabHoverFacts
{
    std::string running;     ///< The command running now, sanitised for display; empty when none is known.
    std::string lastCommand; ///< The newest finished command, sanitised for display; empty with none.
    std::string lastStatus;  ///< How it ended, as the block chip says it (vtbackend::blockStatusText).

    bool operator==(TabHoverFacts const&) const = default;
};

/// Reads a pane's command blocks for its tab's hover card: the newest running command and the newest
/// finished one. A shell without OSC 133 integration has no blocks, and the card names neither.
/// @param records The pane's blocks, oldest first (CommandBlockStore::records()).
/// @param now     The wall clock now, what a running command's time counts to.
/// @return The facts; empty strings for what the blocks do not tell.
template <std::ranges::bidirectional_range Records>
[[nodiscard]] TabHoverFacts tabHoverFactsOf(Records const& records, std::chrono::system_clock::time_point now)
{
    auto facts = TabHoverFacts {};
    // Newest first, stopping at the newest finished command: a long session holds many blocks, and a block
    // still marked running behind a finished one is stale.
    for (vtbackend::CommandBlockRecord const& record: records | std::views::reverse)
    {
        if (record.state == vtbackend::CommandBlockState::Running && facts.running.empty())
            facts.running = record.commandLine.display();
        else if (record.state == vtbackend::CommandBlockState::Finished && !record.commandLine.empty())
        {
            facts.lastCommand = record.commandLine.display();
            facts.lastStatus = vtbackend::blockStatusText(record, now);
            break;
        }
    }
    return facts;
}

/// The hover card's lines for @p facts: "Running: …", then "Last: … · <status>".
/// @param facts What the pane's blocks and its foreground process tell.
/// @return The lines, each only when its fact is known.
[[nodiscard]] std::vector<std::string> tabHoverLines(TabHoverFacts const& facts);

} // namespace contour::session
