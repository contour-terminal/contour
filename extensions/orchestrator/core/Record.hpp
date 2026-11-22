// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>

#include <orchestrator/core/Run.hpp>

/// One finished run as the file of finished runs keeps it (format 1, contract/agent-runs/): what a user
/// analyses afterwards, written by `contour agent record` from the line built here.
namespace orchestrator
{

/// The format of the line recordLine() builds.
inline constexpr uint32_t RecordFormat = 1;

/// @param run A finished run.
/// @return Its line: one JSON object, at most MaxTextBytes (ArgumentKinds.hpp) so that it passes as a
/// declared
///         command's Text argument; past that the history goes first, then the tools, then the statistics.
[[nodiscard]] std::string recordLine(Run const& run);

} // namespace orchestrator
