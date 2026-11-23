// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <orchestrator/core/Run.hpp>

namespace orchestrator
{

/// Whether the program a pane was opened with still runs in it.
enum class ProgramLiveness : uint8_t
{
    Gone,  ///< It exited, or a cold restart left a shell in its place.
    Alive, ///< It still runs: the agent is the pane's program.
};

/// A listed pane tagged with a run.
///
/// The driver builds these from the workspace listing: every listed pane that carries a run's tag, whether
/// or not the driver recorded opening it, and no other pane.
struct PaneFact
{
    std::string pane;                                ///< The pane's id.
    std::string tag;                                 ///< The id of the run it was opened for.
    std::optional<AgentStatus> status;               ///< What its agent last reported; nullopt: nothing yet.
    std::string message;                             ///< The reason line it reported with it; may be empty.
    ProgramLiveness program = ProgramLiveness::Gone; ///< Whether its agent still runs in it.

    [[nodiscard]] bool operator==(PaneFact const&) const = default;
};

/// A run waiting for its tab to open has one: feed it PaneOpened with this pane.
struct Adopt
{
    std::string run;  ///< The run's id.
    std::string pane; ///< Its tab.

    [[nodiscard]] bool operator==(Adopt const&) const = default;
};

/// A run's agent reported a status the run has not taken in: feed it StatusReported.
struct StatusChanged
{
    std::string run;                        ///< The run's id.
    AgentStatus status = AgentStatus::Idle; ///< What the agent reported.

    [[nodiscard]] bool operator==(StatusChanged const&) const = default;
};

/// A pane that must go is still listed: close it. No run is fed anything until it is no longer listed. Pane
/// ids are never reused, so asking again for a pane already being closed is harmless and retries the close.
struct ClosePane
{
    std::string pane; ///< The pane.

    [[nodiscard]] bool operator==(ClosePane const&) const = default;
};

/// No pane tagged with the run's id is listed any more: feed it PaneGone.
struct PaneWentAway
{
    std::string run; ///< The run's id.

    [[nodiscard]] bool operator==(PaneWentAway const&) const = default;
};

/// What reconciling runs with the listed panes asks for.
using Decision = std::variant<Adopt, StatusChanged, ClosePane, PaneWentAway>;

/// Where a pane listing's tags come from, which decides whether a pane missing from it is gone.
enum class PaneListing : uint8_t
{
    /// Nothing was listed: the host could not answer yet (`limit`, before it restored its sessions) or
    /// failed. Nothing is concluded from it; runs are reconciled at the next listing that is known.
    Unknown = 0,
    /// The host serves no pane listing (`not-supported-yet`): the driver has only its own records of the
    /// panes it opened, and lists none. No running run is ended on it; a run closing a tab it recorded
    /// (Run::pane) is told the tab went away, as nothing else would tell it.
    OwnRecordsOnly,
    Tagged, ///< The host's listing with the ownership tags: every tagged pane is in it.
};

/// Compares the runs with the panes listed now.
///
/// Called at start and whenever the panes have been listed. A wake carries no panes, so the driver contract
/// is: after carrying out a wake's effects, list the panes and feed what was listed, which reconciles.
///
/// Preconditions, which the driver keeps:
/// - Every listed pane that carries an ownership tag is reported as a PaneFact. The host keeps the
///   pane-to-tag registry, and its pane listing for the guest carries the tags (PaneListing::Tagged). A host
///   that serves no listing leaves the driver its own records (PaneListing::OwnRecordsOnly), which list no
///   pane: a running run is never told its tab went away, and a run in Ending(ClosePane) only when it
///   recorded its tab (Run::pane). One whose tab it never recorded waits for a tagged listing or the user.
/// - A listing the driver could not read in full is not fed at all: an incomplete listing would tell a run in
///   Ending(ClosePane) that its tab is gone.
/// - @p requestedMonoMs is read before the listing is asked for, from the clock StepContext::monoMs comes
/// from.
///
/// A pane belongs to the run whose id is its tag; paths are never compared, and no run is ever dropped.
/// While any pane tagged with a run's id is listed, that run is never told its pane went away, so nothing
/// touches its worktree: a pane whose agent is gone is closed first, and the run hears of it only once the
/// pane is no longer listed. A listing asked for before a run last changed (Run::changedMonoMs at or after
/// @p requestedMonoMs) says nothing about it: it may predate the run's tab, so the run is left alone.
/// @param runs The runs, in the order their decisions are wanted.
/// @param panes The listed panes tagged with a run.
/// @param requestedMonoMs When the listing was asked for, in monotonic milliseconds.
/// @param listing Where the tags of @p panes come from.
/// @return The decisions, run by run; a run's Adopt precedes its StatusChanged. Empty when nothing changed.
[[nodiscard]] std::vector<Decision> reconcile(std::span<Run const> runs,
                                              std::span<PaneFact const> panes,
                                              uint64_t requestedMonoMs,
                                              PaneListing listing);

/// What a worktree listing's entry is.
enum class WorktreeKind : uint8_t
{
    Checkout, ///< A checked-out working tree.
    Bare,     ///< A bare repository: it has no working tree.
};

/// One entry of `git worktree list --porcelain -z`.
struct WorktreeEntry
{
    std::string path;                  ///< Its directory, as git prints it: resolved, untrusted.
    std::optional<std::string> branch; ///< Its branch without `refs/heads/`; nullopt: detached or bare.
    WorktreeKind kind = WorktreeKind::Checkout; ///< Whether it is a bare repository.

    [[nodiscard]] bool operator==(WorktreeEntry const&) const = default;
};

/// Parses `git worktree list --porcelain -z`.
///
/// Each field ends in a NUL and each entry in an empty field, so no path or lock reason, whatever bytes it
/// holds, can forge a field. The fields of an entry come in git's fixed order: `worktree <path>`, then `bare`
/// or `HEAD <id>`, after HEAD `detached` or `branch <ref>`, then `locked [reason]`, then
/// `prunable [reason]`.
///
/// Fails safe: an entry counts only once its closing empty field was read, so a cut listing loses its last
/// entries instead of inventing a shortened path; and reading stops at the first entry that is not of that
/// shape, keeping the entries before it. The main worktree comes first and is not the agents' to shape, so a
/// worktree an agent made can hide itself and the entries after it from the orphan list, never the main
/// checkout. Git always lists the main worktree, so an empty result means "unknown".
/// @param porcelain The command's standard output.
/// @return The entries up to the first malformed or cut one, in git's order, the main worktree first.
[[nodiscard]] std::vector<WorktreeEntry> worktreeEntries(std::string_view porcelain);

/// Finds the worktrees left behind by runs that are gone.
///
/// An entry is an orphan when it is on an `agent/…` branch or lies under @p worktreeRoot, and no run
/// accounts for it by branch or by path, whichever repository name the run was filed under: a repository
/// configured under two names, or renamed, still accounts for its runs' worktrees. A run accounts for its
/// entry while it is unfinished, once finished if its worktree was kept, and while the listing predates its
/// end: a listing asked for before a run removed its worktree still names it. An entry a later listing
/// still names for a run filed as removed is an orphan. The first entry, the main worktree, never is;
/// neither is a worktree elsewhere on another branch. Orphans are reported, never removed.
/// @param runs The runs.
/// @param worktreeRoot The worktree root; compared with the paths as git prints them.
/// @param entries worktreeEntries() of the repository.
/// @param listedMonoMs When @p entries were asked for, in the engine's monotonic milliseconds; 0 when not
///        known, which no finished run is older than.
/// @param style How the host spells its paths (Config::pathStyle()). On Windows, git spells a listed path
///        `C:/…` and the host `C:\…`: paths are compared by sameHostPath()'s rule.
/// @return The orphans, in listing order.
[[nodiscard]] std::vector<WorktreeEntry> orphans(std::span<Run const> runs,
                                                 std::string_view worktreeRoot,
                                                 std::span<WorktreeEntry const> entries,
                                                 uint64_t listedMonoMs,
                                                 PathStyle style);

/// @param run A run.
/// @param listedMonoMs When a worktree listing was asked for (see orphans()).
/// @return Whether @p run finished after that listing was asked for, which may still name its worktree.
[[nodiscard]] bool finishedSince(Run const& run, uint64_t listedMonoMs) noexcept;

/// The repository's main checkout.
/// @param entries worktreeEntries() of the repository.
/// @return The first entry's path; nullopt when there is none or it is a bare repository.
[[nodiscard]] std::optional<std::string_view> mainCheckout(std::span<WorktreeEntry const> entries) noexcept;

} // namespace orchestrator
