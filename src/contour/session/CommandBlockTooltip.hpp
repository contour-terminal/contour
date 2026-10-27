// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/shell/CommandBlock.hpp>

#include <chrono>
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

} // namespace contour::session
