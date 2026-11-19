// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include <orchestrator/core/Run.hpp>

namespace orchestrator
{

/// How often a status board a tab shows is read again.
inline constexpr auto BoardRefreshEvery = std::chrono::milliseconds { std::chrono::seconds { 5 } };

/// The most lines of a status board that are drawn; a longer board shows its first ones and says it was cut.
inline constexpr std::size_t MaxBoardLines = 2000;

/// How far reading a run's `STATUS.md` has come.
enum class BoardState : uint8_t
{
    Unread = 0,  ///< Asked for; not read yet.
    Read,        ///< Read: Board::bytes holds it.
    Absent,      ///< There is no regular file: none was written yet, or something else is in its place.
    Gone,        ///< The worktree it lived in is gone.
    Unavailable, ///< It could not be read: Board::error says why.
};

/// A run's status board, as last read for a tab that shows it.
struct Board
{
    std::string run = {};                  ///< The run's id.
    BoardState state = BoardState::Unread; ///< How far reading it has come.
    std::string bytes = {};                ///< Raw, as read: sanitised only when drawn.
    uint64_t modifiedWallMs = 0;           ///< STATUS.md's modification time, in wall-clock ms; 0: unknown.
    uint64_t readMonoMs = 0;               ///< When it was last read, in monotonic milliseconds.
    uint64_t readWallMs = 0;               ///< When it was last read, in wall-clock milliseconds; 0: unknown.
    std::optional<FileReadError> error = {}; ///< Why it is Unavailable; std::nullopt otherwise.
    [[nodiscard]] bool operator==(Board const&) const = default;
};

} // namespace orchestrator
