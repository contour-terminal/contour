// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <orchestrator/core/Config.hpp>
#include <orchestrator/core/Model.hpp>
#include <orchestrator/core/Run.hpp>

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
    WorkGroup group;          ///< The group.
    std::string_view foldKey; ///< The fold key that toggles it from @ref fold.
    std::string_view name;    ///< What the board calls it.
    FoldDefault fold;         ///< Whether it starts folded.
};

/// Every group, one row each, in the order they are shown.
inline constexpr auto WorkGroupRows = std::array {
    WorkGroupRow { .group = WorkGroup::NeedsYou,
                   .foldKey = "work/needs-you",
                   .name = "Needs you",
                   .fold = FoldDefault::Open },
    WorkGroupRow { .group = WorkGroup::ReadyToMerge,
                   .foldKey = "work/ready",
                   .name = "Ready to merge",
                   .fold = FoldDefault::Open },
    WorkGroupRow { .group = WorkGroup::InReview,
                   .foldKey = "work/in-review",
                   .name = "In review",
                   .fold = FoldDefault::Open },
    WorkGroupRow { .group = WorkGroup::NoMrYet,
                   .foldKey = "work/no-mr",
                   .name = "No MR yet",
                   .fold = FoldDefault::Open },
    WorkGroupRow { .group = WorkGroup::EverythingElse,
                   .foldKey = "work/else",
                   .name = "Everything else",
                   .fold = FoldDefault::Folded },
};

/// The fold key whose presence means the flat sort by last update. A group's fold key held by the engine
/// means "toggled from the group's FoldDefault"; folds and sort persist through ToggleFoldAction, with no
/// store change.
inline constexpr std::string_view SortByUpdateKey = "work/sort-by-update";

/// Whether a ticket or forge issue is being worked on.
enum class Progress : uint8_t
{
    NotStarted = 0, ///< Not in progress.
    InProgress,     ///< In progress.
};

/// An issue as a repository's sources listed it, with its progress decided.
struct ListedIssue
{
    Issue issue;                              ///< The issue.
    Progress progress = Progress::NotStarted; ///< Its progress (progressOf()).
    [[nodiscard]] bool operator==(ListedIssue const&) const = default;
};

/// One row of the Work board.
struct WorkItem
{
    WorkGroup group = WorkGroup::EverythingElse; ///< The group it is shown in.
    std::string repo;                            ///< The repository it belongs to (configured name).
    std::optional<ChangeRequest> changeRequest;  ///< An MR row's merge request; empty for a ticket row.
    std::optional<Issue> ticket;                 ///< The linked ticket (MR row) or the ticket itself.
    std::string ticketKey;      ///< The linked or own key (Jira key, or `#<n>`); empty: none.
    ItemRef item;               ///< What a dispatch works on: ChangeRequestRef, JiraRef or IssueRef.
    uint64_t updatedWallMs = 0; ///< The MR's, else the ticket's update time; 0: unknown.
    std::string agentRun;       ///< An active run on it (or on its linked ticket); empty: none.
    std::string agentWorker;    ///< That run's worker id; empty: none.
    [[nodiscard]] bool operator==(WorkItem const&) const = default;
};

/// One repository's lists, in configuration order.
struct RepoWork
{
    std::string_view repo;                         ///< The repository's configured name.
    std::span<ChangeRequest const> changeRequests; ///< Its open merge requests.
    std::span<ListedIssue const> issues;           ///< Every issue its sources listed, in source order.
};

/// What workItems() joins.
struct WorkInput
{
    std::span<RepoWork const> repos;      ///< In configuration order: a ticket listed twice is the first's.
    std::span<Issue const> linkedTickets; ///< The key lookup's tickets (Engine::linkedTickets()).
    std::span<std::string const> jiraProjects; ///< jiraProjectsOf().
    std::span<Run const> runs;                 ///< Every run; only active ones (countsAsActive) count.
};

/// @param run A run.
/// @return The worker @p run runs on: Run::worker, or ImplicitWorkerId (`claude`) when it is empty (Decision
///         15).
[[nodiscard]] std::string_view workerIdOf(Run const& run) noexcept;

/// @param item A Work board row.
/// @return The item's identity across rebuilds: `<repo>/!<n>` for an MR row, `<repo>/<JIRA-KEY>` or
///         `<repo>/#<n>` for a ticket row; what the view keeps a selection and an open menu on.
[[nodiscard]] std::string workItemKey(WorkItem const& item);

/// A key links only into a covered project: one whose key a source listed, or that a Jira source's JQL names.
/// The JQL is scanned for `project = <name>` and `project in (<name>, …)`, case-insensitively, quotes
/// stripped; a name that cannot be a key's project part (a numeric id, a name with blanks) is left out.
/// @param repos The configured repositories; only their Jira queries are read.
/// @param listed What each repository's sources listed.
/// @return The covered Jira projects, upper case, sorted, unique.
[[nodiscard]] std::vector<std::string> jiraProjectsOf(std::span<Repo const> repos,
                                                      std::span<RepoWork const> listed);

/// A key starts after a byte that is not `[A-Za-z0-9]` and ends before one, so `XPRJ-1` and `PRJ-1x` are no
/// keys.
/// @param title A merge request's title, scanned case-sensitively.
/// @param branch Its source branch, scanned case-insensitively.
/// @param projects The covered projects (jiraProjectsOf()).
/// @return The first covered Jira key in @p title, else in @p branch (upper-cased); std::nullopt: none.
[[nodiscard]] std::optional<std::string> linkedJiraKey(std::string_view title,
                                                       std::string_view branch,
                                                       std::span<std::string const> projects);

/// @param mr A merge request.
/// @return The forge issues of its own repository @p mr names: closesIssues, then each `#<digits>` in its
///         title (at a word boundary), each once, in that order.
[[nodiscard]] std::vector<uint64_t> namedForgeIssues(ChangeRequest const& mr);

/// @param issue A listed issue.
/// @param inProgressKeys The keys the source's `in_progress_jql` returned; std::nullopt: none configured, or
///                       its query failed.
/// @return @p issue's progress: a forge issue (IssueRef) is InProgress; a Jira ticket is InProgress when
///         @p inProgressKeys holds its key, or — @p inProgressKeys absent — when its category is InProgress.
[[nodiscard]] Progress progressOf(Issue const& issue,
                                  std::optional<std::span<std::string const>> inProgressKeys) noexcept;

/// @param mr A merge request.
/// @return The group of an MR row: the first of Draft → EverythingElse; failing, changes requested or
///         conflicting → NeedsYou; ready → ReadyToMerge; else InReview. Ready takes a passing pipeline (or
///         none reported) and an approval (or, reported by the forge, no review required). A review inferred
///         from GitLab's merge status is ready only when Approved (`mergeable`): its None names no review;
///         and while the CI is inferred too, `mergeable` stands for the pipeline as well.
[[nodiscard]] WorkGroup groupOf(ChangeRequest const& mr) noexcept;

/// @param progress A ticket's progress.
/// @return NoMrYet for an in-progress ticket no MR names; EverythingElse otherwise.
[[nodiscard]] WorkGroup groupOf(Progress progress) noexcept;

/// @param input The repositories' lists, the looked-up tickets, the covered projects and the runs.
/// @return Every MR, and every ticket or issue no MR names, de-duplicated by key, in groups; MR rows first,
///         in repository then list order, then ticket rows likewise.
[[nodiscard]] std::vector<WorkItem> workItems(WorkInput const& input);

/// How the board orders its items.
enum class WorkSort : uint8_t
{
    Importance = 0, ///< By group, then newest first.
    LastUpdate,     ///< Newest first, groups ignored.
};

/// Items with a known update time come first, newest first; those without one (Jira tickets, forge issues)
/// follow by their position in their source's result (Issue::listRank).
/// @param items The items.
/// @param repoOrder The repositories' names, in configuration order.
/// @param sort The order asked for.
/// @return Indices into @p items: by group then by update (Importance), or by update alone (LastUpdate); ties
///         by repository order, then MR rows before ticket rows, then MR number / key ascending.
[[nodiscard]] std::vector<std::size_t> orderOf(std::span<WorkItem const> items,
                                               std::span<std::string const> repoOrder,
                                               WorkSort sort);

/// @param item A Work board row.
/// @param filter What the user typed.
/// @return Whether @p item matches @p filter, case-folded: `!<n>`/`#<n>`/`<n>` of its MR, or its ticket key,
///         title, repository or URL contains it; an empty filter matches everything.
[[nodiscard]] bool matchesFilter(WorkItem const& item, std::string_view filter);

/// The task the start dialog suggests, and why.
struct TaskSuggestion
{
    TaskDef const* task = nullptr; ///< nullptr: no task applies to the item.
    std::string reason;            ///< Shown beside it.
};

/// First match: ci Failing → FixCi, reason `pipeline fails: <failing checks, ", "-joined>` (or `pipeline
/// fails`); Conflicting → ResolveConflicts, `conflicts with <base>`; ChangesRequested → AddressReview,
/// `changes requested`; a ticket row → WorkOnIssue, `ticket in progress` / `ticket`; then the first task that
/// applies to the item, reason `first task for a merge request` / `first task for a ticket`. A role no
/// applicable task has falls through to the next rule.
/// @param config The configuration: its tasks.
/// @param item A Work board row.
/// @return The suggested task and its reason.
[[nodiscard]] TaskSuggestion suggestTask(Config const& config, WorkItem const& item);

/// @param config The configuration; unused, since each run carries its worker's id (kept for the callers'
///               signature).
/// @param runs Every run; only active ones (countsAsActive) count.
/// @param workerId A worker's id.
/// @return How many active runs run on @p workerId, each counted by workerIdOf() (Decision 15).
[[nodiscard]] std::size_t activeRunsOn(Config const& config,
                                       std::span<Run const> runs,
                                       std::string_view workerId) noexcept;

/// @param config The configuration: its tasks and workers.
/// @param runs Every run; only active ones (countsAsActive) count.
/// @param taskId A task id.
/// @return The task's own worker; when it runs at least one agent, the worker with the fewest active runs
///         (ties in configuration order) among those the engine takes for the task, so never one for which
///         takesNoProfileFor() holds.
[[nodiscard]] WorkerDef const& defaultWorker(Config const& config,
                                             std::span<Run const> runs,
                                             std::string_view taskId);

} // namespace orchestrator
