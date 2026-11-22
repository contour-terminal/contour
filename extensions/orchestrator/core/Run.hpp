// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <orchestrator/core/Commands.hpp>
#include <orchestrator/core/Model.hpp>
#include <orchestrator/core/Provider.hpp>
#include <orchestrator/core/Stats.hpp>

namespace orchestrator
{

/// The steps that prepare a run, in order: each awaits one host call.
enum class PrepareStep : uint8_t
{
    Fetch,       ///< `git-fetch` in the repository.
    WorktreeAdd, ///< `git-worktree-add` (a change request) or `git-worktree-add-branch` (an issue).
    Checkout,    ///< `gh-pr-checkout` in the new worktree; a change request only.
    OpenPane,    ///< Opening the agent's tab.
};

/// The steps that end a run, in order. Nothing touches the worktree while the agent's tab may still run.
enum class EndStep : uint8_t
{
    ClosePane,    ///< Waiting for the agent's tab to be gone: the user ended the run, or opening it failed.
    MainCheckout, ///< Waiting for the repository's main checkout to be known, which the first checks compare
                  ///< the worktree against: no host call.
    Verify,       ///< Reading the worktree's `.git` file.
    CommonDir,    ///< `git-common-dir`: git uses the main checkout's `.git`, not a redirected one.
    IndexFlags,   ///< `git-index-flags`: no submodule, and no file marked so `git status` misses its change.
    Reachable,    ///< `git-head-reachable`: some ref contains HEAD.
    OperationState,   ///< `git-operation-state`: no operation is left half done.
    Status,           ///< `git-status`: nothing is modified or untracked.
    CleanStatusBoard, ///< `git-clean`: the status board, the one untracked file the extension writes.
    Remove,           ///< `git-worktree-remove`, never forced.
};

/// What an agent last reported about itself.
enum class AgentStatus : uint8_t
{
    Idle,    ///< Waiting for input it does not need.
    Working, ///< Working.
    Done,    ///< Finished its task; it stays open until the user ends the run.
    Blocked, ///< Waiting for the user.
    Error,   ///< Stopped by an error.
};

/// How a run ended.
enum class Ended : uint8_t
{
    Completed,         ///< Its tab went away after the agent had reported.
    EndedByYou,        ///< The user ended it.
    PreparationFailed, ///< A preparation step failed; Run::failure says which and why.
    NeverReported,     ///< Its tab went away before the agent ever reported.
    WorktreeGone,      ///< Its worktree was removed by someone else.
};

/// What happened to a finished run's worktree.
enum class Fate : uint8_t
{
    Removed, ///< It was removed, or there was none.
    Kept,    ///< It was kept; the entries say why.
};

/// Whether the user has looked at a finished run.
enum class Seen : uint8_t
{
    New,    ///< Not yet.
    Opened, ///< The user opened its details, status board or transcript.
};

/// Why the host could not read a worktree's `.git` file.
enum class FileReadError : uint8_t
{
    DirectoryMissing, ///< The worktree directory itself is gone: the one failure that lets a run be
                      ///< forgotten.
    NotRegular,       ///< The entry is a directory, a link or anything but a regular file.
    Unreadable,       ///< The file could not be read.
    TooLarge,         ///< The file is larger than a `.git` file can be.
    Escapes,          ///< The path leads outside its root.
};

/// The preparation step that failed, and why.
struct RunFailure
{
    PrepareStep step = PrepareStep::Fetch; ///< The step.
    std::string why;                       ///< Sanitised, one line.

    [[nodiscard]] bool operator==(RunFailure const&) const = default;
};

/// The run is being prepared.
struct Preparing
{
    PrepareStep step = PrepareStep::Fetch; ///< The step whose host call is awaited.

    [[nodiscard]] bool operator==(Preparing const&) const = default;
};

/// The agent is working, or idle.
struct Running
{
    [[nodiscard]] bool operator==(Running const&) const = default;
};

/// The agent waits for the user, or stopped by an error.
struct Blocked
{
    [[nodiscard]] bool operator==(Blocked const&) const = default;
};

/// The agent finished its task; the run stays active until the user ends it.
struct Done
{
    [[nodiscard]] bool operator==(Done const&) const = default;
};

/// The run is over: an entry of the Finished list.
struct Finished
{
    Ended ended = Ended::Completed; ///< How it ended.
    Fate fate = Fate::Removed;      ///< What happened to its worktree.
    std::vector<std::string>
        entries;              ///< Why it was kept: `git status` lines or the check that stopped; sanitised.
    Seen seen = Seen::New;    ///< Whether the user has looked at it.
    uint64_t endedWallMs = 0; ///< When it ended, in wall-clock milliseconds.

    [[nodiscard]] bool operator==(Finished const&) const = default;
};

/// Whether an ending run has asked git to remove its worktree.
enum class Removal : uint8_t
{
    NotIssued, ///< Not yet: a worktree found gone was removed by someone else.
    Issued,    ///< `git-worktree-remove` was stored as the step: a worktree found gone may be its doing.
};

/// The run is ending: its worktree goes through the removal checks.
struct Ending
{
    EndStep step = EndStep::Verify; ///< The step whose result is awaited.
    Ended cause = Ended::Completed; ///< How the run ended, unless its worktree turns out to be gone.
    std::optional<Finished>
        resumedFrom; ///< The kept entry being checked again, whose `seen` and end time stay.
    Removal removal =
        Removal::NotIssued; ///< Whether the remove was asked for: a worktree gone after it is ours.

    [[nodiscard]] bool operator==(Ending const&) const = default;
};

/// Where a run stands. A Finished run is a state of the same record, so one store key holds both.
using RunState = std::variant<Preparing, Running, Blocked, Done, Ending, Finished>;

/// When a run's state changed, as its history keeps it.
struct StateChange
{
    uint64_t wallMs = 0; ///< When, in wall-clock milliseconds.
    std::string state;   ///< The state it entered, as the store names it: `preparing`, `running`, `blocked`,
                         ///< `done`, `ending` or `finished`.

    [[nodiscard]] bool operator==(StateChange const&) const = default;
};

/// The most state changes a run's history keeps: the first, and the latest after it.
inline constexpr std::size_t MaxHistory = 64;

/// Whether a finished run was appended to the file of finished runs (`contour agent record`).
enum class Ledger : uint8_t
{
    Pending = 0, ///< Not yet: a run stored before the file existed is appended once too.
    Written,     ///< It was.
};

/// How long a run took, read from its history.
struct RunTimes
{
    std::optional<uint64_t> wallMs; ///< From dispatch to its end; none while it runs, or without a history.
    uint64_t waitingMs = 0;         ///< Time its agent was blocked, waiting for the user; a wait still going
                                    ///< on is not counted.

    [[nodiscard]] bool operator==(RunTimes const&) const = default;
};

/// One agent run, from dispatch to the Finished list.
struct Run
{
    std::string id;                  ///< runId(): a Name.
    std::string repo;                ///< The repository's configured name.
    std::string repoPath;            ///< The repository's directory.
    Forge forge = Forge::GitHub;     ///< The repository's forge: its provider row prepares the worktree.
    std::string remote;              ///< The remote to fetch from: a Name.
    std::string base;                ///< The base branch; the worktree starts at `<remote>/<base>`.
    ItemRef item;                    ///< What the run works on.
    std::string url;                 ///< The item's web page; empty for a topic.
    std::string topic {};            ///< A TopicRef's text, sanitised to one line; empty for any other item.
    std::string task;                ///< The task's id.
    std::string model;               ///< The model the agent runs: a Name.
    std::string prompt;              ///< The agent's first prompt: a Text.
    std::string session;             ///< The agent's session: a Uuid.
    std::string branch;              ///< `agent/<id>`: a Ref.
    std::string worktree;            ///< The worktree's directory: a Path.
    std::optional<std::string> pane; ///< The agent's tab, a Name, once open and until it is gone.
    std::optional<std::string>
        nearTab {}; ///< The dashboard tab it was dispatched from, a Name; none: unknown.
    RunState state; ///< Where it stands.
    std::optional<AgentStatus> lastStatus; ///< What the agent last reported; std::nullopt: never reported.
    std::optional<RunFailure> failure;     ///< The preparation step that failed, if one did.
    uint64_t startedWallMs = 0;            ///< When it was dispatched, in wall-clock milliseconds.
    uint32_t attempt = 0; ///< How many host calls were issued for it; each call's tag carries the count, so
                          ///< that a late reply to an earlier call is told apart from the awaited one.
    uint64_t runningSinceMs = 0; ///< When its tab opened; monotonic, never persisted.
    uint64_t changedMonoMs = 0; ///< When beginRun() or advance() last changed it; monotonic, never persisted.
    std::optional<RunStats> stats {}; ///< What its agent cost and did, as last read; kept once it finished.
    std::vector<StateChange> history {}; ///< When its state changed, oldest first; at most MaxHistory.
    Ledger ledger = Ledger::Pending;     ///< Whether it was appended to the file of finished runs.
};

/// A host call's result: the command exited 0.
struct CommandOk
{
    std::string_view stdoutText; ///< Its standard output.
};

/// A host call's result: the command was refused, timed out, exited non-zero, or its output was cut.
struct CommandFailed
{
    std::string_view why; ///< Untrusted; sanitised before it is kept.
};

/// A host call's result: the command could not start, because its working directory is gone.
struct CwdMissing
{
};

/// The awaited result will never come: the guest restarted and the host knows nothing of the call.
struct Lost
{
};

/// A host call's result: the worktree's `.git` file, read.
struct GitFileRead
{
    std::expected<std::string_view, FileReadError> content; ///< Its content, or why it could not be read.
};

/// A host call's result: the agent's tab is open.
struct PaneOpened
{
    std::string_view pane; ///< The tab.
    uint64_t monoMs = 0;   ///< When, in monotonic milliseconds.
};

/// A host call's result: the agent's tab could not be opened.
struct PaneOpenFailed
{
    std::string_view why; ///< Untrusted; sanitised before it is kept.
};

/// The agent reported its status.
struct StatusReported
{
    AgentStatus status = AgentStatus::Idle; ///< What it reported.
};

/// The agent's tab is no longer listed.
struct PaneGone
{
};

/// The user ended the run.
struct EndByUser
{
};

/// The agent stayed done for `end_done_after`: a done run ends as completed; any other state ignores it.
struct EndWhenDone
{
};

/// A kept worktree is checked again: by the user, or on a poll.
struct RetryRemoval
{
};

/// The repository's worktree listing answered: a run waiting for its main checkout looks again.
struct CheckoutListed
{
};

/// The user dismissed a finished run.
struct Dismiss
{
};

/// The user opened a finished run.
struct MarkSeen
{
};

/// What advance() is fed.
using RunInput = std::variant<CommandOk,
                              CommandFailed,
                              CwdMissing,
                              Lost,
                              GitFileRead,
                              PaneOpened,
                              PaneOpenFailed,
                              StatusReported,
                              PaneGone,
                              EndByUser,
                              EndWhenDone,
                              RetryRemoval,
                              CheckoutListed,
                              Dismiss,
                              MarkSeen>;

/// Run a declared command. Its working directory follows from the command's root: a repository command
/// runs in Run::repoPath, a worktree command in Run::worktree.
struct RunDeclared
{
    std::string_view name;           ///< The command, as declared.
    std::vector<Argument> arguments; ///< Its arguments, each accepted by its kind.
};

/// Read the worktree's `.git` file.
struct ReadGitFile
{
};

/// Open the agent's tab, with the run's model, session and prompt, in its worktree, tagged with its id.
struct OpenAgentPane
{
};

/// Close every tab tagged with the run's id, whether or not the run knows its tab. The run then waits for
/// PaneGone, which is fed once no tab with that tag is listed.
struct CloseAgentPane
{
};

/// Store the run.
struct Persist
{
};

/// Delete the stored run.
struct Forget
{
};

/// What advance() asks its driver to do, in order. A Persist comes first, so that the stored run names the
/// step before any call it awaits is made.
using RunEffect = std::variant<RunDeclared, ReadGitFile, OpenAgentPane, CloseAgentPane, Persist, Forget>;

/// Why a repository's main checkout is not known.
enum class CheckoutUnknown : uint8_t
{
    Unavailable, ///< For good: its listing named none, its directory is gone, or the listing failed too
                 ///< often. A check that needs it keeps the worktree without running anything there.
    Pending,     ///< Not yet: its listing has not answered. A check that needs it waits for CheckoutListed.
};

/// What a step may know about the world beyond the run.
struct StepContext
{
    std::optional<std::string_view>
        mainCheckout; ///< The repository's main checkout, resolved; nullopt if unknown.
    CheckoutUnknown unknownCheckout =
        CheckoutUnknown::Unavailable; ///< Why mainCheckout is nullopt; not read while it is known.
    uint64_t wallMs = 0;              ///< Now, in wall-clock milliseconds.
    uint64_t monoMs = 0;              ///< Now, in monotonic milliseconds: what a change is stamped with.
    /// The provider rows the configuration was read with; a step never runs an Unmeasured row's command.
    std::span<Provider const> providers = orchestrator::providers();
};

/// The most characters an entry or a reason keeps, once sanitised.
inline constexpr std::size_t MaxEntryChars = 200;

/// The most bytes an entry or a reason keeps: MaxEntryChars characters of up to four bytes each.
inline constexpr std::size_t MaxEntryBytes = MaxEntryChars * 4;

/// The most `git status` lines a kept run keeps; a last entry counts the rest.
inline constexpr std::size_t MaxStatusEntries = 20;

/// The most bytes a stored item URL may have.
inline constexpr std::size_t MaxUrlBytes = 2048;

/// The most bytes toJson() writes for a run isStorable() accepts: the store's limit for one value.
inline constexpr std::size_t MaxStoredRunBytes = std::size_t { 64 } * 1024;

/// Why beginRun() did not start a run.
enum class BeginError : uint8_t
{
    NotPreparing, ///< The run is not waiting for its fetch.
    NotStorable,  ///< isStorable() refuses it: it must not be stored or dispatched.
};

/// Starts a run stored as `Preparing(Fetch)`.
/// @param run The run, which the caller checks with isStorable() before it stores it.
/// @param context The world beyond the run.
/// @return Persist and the fetch, or a failed preparation for a remote no Name accepts; or why the run was
///         not started.
[[nodiscard]] std::expected<std::vector<RunEffect>, BeginError> beginRun(Run& run,
                                                                         StepContext const& context);

/// Moves a run one step on.
/// @param run The run.
/// @param input What happened.
/// @param context The world beyond the run.
/// @return What to do, in order; empty for an input its state does not await, which leaves it unchanged.
///         Whenever it is not empty, Run::changedMonoMs is set to StepContext::monoMs.
[[nodiscard]] std::vector<RunEffect> advance(Run& run, RunInput const& input, StepContext const& context);

/// The tag of the host call a run awaits.
/// @param run The run.
/// @return Such as "<id>/fetch" or "<id>/verify"; std::nullopt when it awaits no host call.
[[nodiscard]] std::optional<std::string> stepTag(Run const& run);

/// Whether a run counts toward the dispatch limits and as its item's run.
/// @param run The run.
/// @return Whether it is Preparing, Running, Blocked or Done.
[[nodiscard]] bool countsAsActive(Run const& run) noexcept;

/// Whether a run still has steps to take.
/// @param run The run.
/// @return Whether it is anything but Finished.
[[nodiscard]] bool isUnfinished(Run const& run) noexcept;

/// Whether a run's worktree is still there, as far as the run knows: where its status board is read.
/// @param run The run.
/// @return Whether it is anything but a Finished run whose worktree was removed.
[[nodiscard]] bool keepsWorktree(Run const& run) noexcept;

/// The name of @p status. The store keeps it, and Contour's program status reports an agent's state in the
/// same words: one table serves both.
/// @param status What an agent reported.
/// @return Its name, such as `working`.
[[nodiscard]] std::string_view agentStatusName(AgentStatus status) noexcept;

/// @param ended How a run ended.
/// @return Its name as the store and the file of finished runs spell it, such as `ended-by-you`.
[[nodiscard]] std::string_view endedName(Ended ended) noexcept;

/// The state an agent's report puts its run in.
/// @param status What it reported.
/// @return Running for Idle and Working, Blocked for Blocked and Error, Done for Done.
[[nodiscard]] RunState stateFor(AgentStatus status) noexcept;

/// Whether a worktree's `.git` file still points to where git put it.
/// @param content The file's content.
/// @param mainCheckout The repository's main checkout.
/// @param worktree The worktree's directory.
/// @return Whether @p content is `gitdir: <mainCheckout>/.git/worktrees/<last component of worktree>`,
///         optionally followed by one line end, and nothing else.
[[nodiscard]] bool gitFileIsIntact(std::string_view content,
                                   std::string_view mainCheckout,
                                   std::string_view worktree) noexcept;

/// Whether git, run inside a worktree, uses its repository's own directory: a redirected `commondir` link
/// would make it read another directory's configuration and attributes.
/// @param output What `git rev-parse --path-format=absolute --git-common-dir` printed there.
/// @param mainCheckout The repository's main checkout.
/// @return Whether @p output is `<mainCheckout>/.git`, optionally followed by one line end, and nothing else.
[[nodiscard]] bool commonDirIsMain(std::string_view output, std::string_view mainCheckout) noexcept;

/// Whether a run fits the store: every field holds what its kind accepts, and the free text is bounded so
/// that toJson() stays within MaxStoredRunBytes however it escapes. The prompt is Text with no control
/// character but a line end or a tab; the URL at most MaxUrlBytes of URL bytes; the paths hold no control
/// character; the tab a Name; each entry and reason at most MaxEntryChars characters and MaxEntryBytes bytes
/// without a control character, and at most MaxStatusEntries + 1 entries. Nothing is cut: a dispatch whose
/// prompt, URL or paths do not fit is refused; entries and reasons are cut when they are made, and a tab id
/// that is no Name fails the tab's opening.
/// @param run The run.
/// @return Whether runFromJson(toJson(run)) reads it back.
[[nodiscard]] bool isStorable(Run const& run) noexcept;

/// The version toJson() writes and runFromJson() reads.
inline constexpr uint32_t RunSchema = 3;

/// A run as stored, without its monotonic fields.
/// @param run The run.
/// @return One JSON object with `"v": RunSchema`.
/// @param history A run's history (Run::history).
/// @return How long it took, and how long its agent waited for the user.
[[nodiscard]] RunTimes timesOf(std::span<StateChange const> history) noexcept;

[[nodiscard]] std::string toJson(Run const& run);

/// A stored run.
/// @param text What toJson() wrote.
/// @return The run; std::nullopt for anything but one JSON object of version RunSchema, of a run isStorable()
///         accepts, whose enum names are
///         known, whose fields pass their kinds and whose state is consistent.
[[nodiscard]] std::optional<Run> runFromJson(std::string_view text);

} // namespace orchestrator
