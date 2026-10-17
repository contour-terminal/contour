// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

/// @file
/// Where the kernel says a process runs: the seam over /proc/<pid>/cgroup, and the one question asked
/// of its text.

namespace vtpty
{

/// Reads the text of /proc/<pid>/cgroup.
/// @return The text, or nothing when it cannot be read: the process is gone, or there is no /proc.
using ProcessCgroupReader = std::function<std::optional<std::string>(int pid)>;

/// Reads /proc/@p pid/cgroup as a stream: /proc reports its files as empty, so a read sized by the
/// file's size gets nothing.
/// @param pid The process to ask about.
/// @return The text, or nothing when it cannot be read.
[[nodiscard]] std::optional<std::string> readProcessCgroup(int pid);

/// @param cgroupFile The text of a /proc/<pid>/cgroup: `hierarchy:controllers:path` lines.
/// @param unit       A systemd unit name, such as `contour-session-1-2-3.scope`.
/// @return Whether a hierarchy puts the process directly in @p unit's cgroup -- the unified one does
///         wherever systemd runs, and the `name=systemd` one on a legacy hierarchy.
[[nodiscard]] bool isInUnit(std::string_view cgroupFile, std::string_view unit) noexcept;

} // namespace vtpty
