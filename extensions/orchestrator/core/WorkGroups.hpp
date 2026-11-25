// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace orchestrator
{

/// The Work board's groups, in the order they are shown.
enum class WorkGroup : uint8_t
{
    NeedsYou = 0,   ///< An MR whose pipeline failed, a reviewer asked for changes, or that conflicts.
    ReadyToMerge,   ///< An MR that passed, and is approved or needs no review.
    InReview,       ///< Any other open MR.
    NoMrYet,        ///< A ticket or forge issue in progress that no MR names.
    EverythingElse, ///< Drafts, and the tickets not in progress.
};

/// Whether a group starts folded.
enum class FoldDefault : uint8_t
{
    Open = 0, ///< Shown open until the user folds it.
    Folded,   ///< Shown folded until the user opens it.
};

/// One row of the Work board's group table.
struct WorkGroupRow
{
    WorkGroup group;             ///< The group.
    std::string_view foldKey;    ///< The key its fold is kept under (SetFold).
    std::string_view name;       ///< What the board calls it.
    std::string_view configName; ///< The name `work.folded` spells it by.
    FoldDefault fold;            ///< Whether it starts folded.
};

/// Every group, one row each, in the order they are shown.
inline constexpr auto WorkGroupRows = std::array {
    WorkGroupRow { .group = WorkGroup::NeedsYou,
                   .foldKey = "work/needs-you",
                   .name = "Needs you",
                   .configName = "needs_you",
                   .fold = FoldDefault::Open },
    WorkGroupRow { .group = WorkGroup::ReadyToMerge,
                   .foldKey = "work/ready",
                   .name = "Ready to merge",
                   .configName = "ready_to_merge",
                   .fold = FoldDefault::Open },
    WorkGroupRow { .group = WorkGroup::InReview,
                   .foldKey = "work/in-review",
                   .name = "In review",
                   .configName = "in_review",
                   .fold = FoldDefault::Open },
    WorkGroupRow { .group = WorkGroup::NoMrYet,
                   .foldKey = "work/no-mr",
                   .name = "No MR yet",
                   .configName = "no_mr_yet",
                   .fold = FoldDefault::Open },
    WorkGroupRow { .group = WorkGroup::EverythingElse,
                   .foldKey = "work/else",
                   .name = "Everything else",
                   .configName = "everything_else",
                   .fold = FoldDefault::Folded },
};

/// The fold key whose presence means the flat sort by last update, toggled by ToggleFoldAction. A group's
/// fold is the user's choice, kept by SetFoldAction as `<foldKey>#open` or `<foldKey>#folded`; a bare fold
/// key, stored before choices, means "toggled from the group's FoldDefault".
inline constexpr std::string_view SortByUpdateKey = "work/sort-by-update";

/// Whether the Work view shows a group with no item.
enum class EmptyGroups : uint8_t
{
    Shown = 0, ///< Shown, with its heading and no rows.
    Hidden,    ///< Not shown at all.
};

/// The `work` settings.
struct WorkSettings
{
    std::vector<WorkGroup> folded = {};      ///< The groups folded until the user unfolds them, in row order.
    EmptyGroups empty = EmptyGroups::Hidden; ///< `fold_empty`.
    [[nodiscard]] bool operator==(WorkSettings const&) const = default;
};

/// @return The groups whose row is FoldDefault::Folded, and Hidden. Defined inline here: it filters
///         WorkGroupRows.
[[nodiscard]] inline WorkSettings defaultWorkSettings()
{
    auto folded = std::vector<WorkGroup> {};
    for (auto const& row: WorkGroupRows)
        if (row.fold == FoldDefault::Folded)
            folded.push_back(row.group);
    return WorkSettings { .folded = std::move(folded), .empty = EmptyGroups::Hidden };
}

} // namespace orchestrator
