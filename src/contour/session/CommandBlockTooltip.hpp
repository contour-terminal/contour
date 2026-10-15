// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/shell/CommandBlock.hpp>

#include <chrono>
#include <string>

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

} // namespace contour::session
