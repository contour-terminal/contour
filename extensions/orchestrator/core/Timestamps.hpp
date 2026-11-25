// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace orchestrator
{

/// Reads an ISO 8601 time with a zone, as the forges print one.
/// @param text An ISO 8601 time with a zone: `YYYY-MM-DDTHH:MM:SS`, an optional `.` and 1-9 fraction digits,
/// then
///        `Z`, `+HH:MM` or `+HHMM` (GitHub, GitLab and Jira spell it these ways).
/// @return Milliseconds since the Unix epoch (fraction truncated to ms); std::nullopt for anything else, or a
///         time before 1970.
[[nodiscard]] std::optional<uint64_t> unixMsOf(std::string_view text) noexcept;

} // namespace orchestrator
