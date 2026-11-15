// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>

namespace orchestrator
{

/// A git operation that can be left half done in a worktree.
enum class Operation : uint8_t
{
    Rebase,     ///< A rebase, stopped at a conflict, an `edit` or a `break`.
    Merge,      ///< A merge with unmerged paths, or one not yet committed.
    CherryPick, ///< A cherry-pick, or a sequence of them.
    Revert,     ///< A revert, or a sequence of them.
    Bisect,     ///< A bisect session.
    Am,         ///< An `am` session.
};

/// The operation's name, as a kept run's entry spells it.
/// @param operation The operation.
/// @return Such as "rebase" or "cherry-pick".
[[nodiscard]] std::string_view operationName(Operation operation) noexcept;

/// Why operationInProgress could not tell whether an operation is in progress.
enum class OperationStateError : uint8_t
{
    Unrecognised, ///< The output does not start with a line git prints there: it may be another git's
                  ///< wording.
};

/// The operation a long `git status` says is in progress.
///
/// Reads the output of `git -c status.displayCommentPrefix=false -c color.status=false status --long`, run
/// with `LC_ALL=C`: git names an operation in progress on a line of its own, starting in column 0. File names
/// are indented and quoted in that format, so no file name can be read as such a line. Several of these
/// states leave `git status --porcelain` empty and HEAD inside a ref, so only this check keeps their
/// worktree. A git that words its lines differently could hide an operation, so the output's first line
/// must be one of the shapes git is known to start with; anything else is unrecognised, which keeps the
/// worktree.
/// @param longStatus The command's standard output.
/// @return The operation the first line beginning with one of git's state lines names; std::nullopt when no
///         line does; OperationStateError::Unrecognised when the first line has no known shape.
[[nodiscard]] std::expected<std::optional<Operation>, OperationStateError> operationInProgress(
    std::string_view longStatus) noexcept;

} // namespace orchestrator
