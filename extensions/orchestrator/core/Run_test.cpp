// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <orchestrator/core/ArgumentKinds.hpp>
#include <orchestrator/core/Commands.hpp>
#include <orchestrator/core/Model.hpp>
#include <orchestrator/core/Naming.hpp>
#include <orchestrator/core/Run.hpp>

using namespace std::string_view_literals;

namespace
{

using orchestrator::AgentStatus;
using orchestrator::Blocked;
using orchestrator::ChangeRequestRef;
using orchestrator::CheckoutListed;
using orchestrator::CheckoutUnknown;
using orchestrator::CloseAgentPane;
using orchestrator::CommandFailed;
using orchestrator::CommandOk;
using orchestrator::CwdMissing;
using orchestrator::Dismiss;
using orchestrator::Done;
using orchestrator::EndByUser;
using orchestrator::Ended;
using orchestrator::Ending;
using orchestrator::EndStep;
using orchestrator::Fate;
using orchestrator::FileReadError;
using orchestrator::Finished;
using orchestrator::Forget;
using orchestrator::GitFileRead;
using orchestrator::IssueRef;
using orchestrator::ItemRef;
using orchestrator::JiraRef;
using orchestrator::Lost;
using orchestrator::MarkSeen;
using orchestrator::OpenAgentPane;
using orchestrator::PaneGone;
using orchestrator::PaneOpened;
using orchestrator::PaneOpenFailed;
using orchestrator::Persist;
using orchestrator::PrepareStep;
using orchestrator::Preparing;
using orchestrator::ReadGitFile;
using orchestrator::Removal;
using orchestrator::RetryRemoval;
using orchestrator::Run;
using orchestrator::RunDeclared;
using orchestrator::RunEffect;
using orchestrator::RunFailure;
using orchestrator::RunInput;
using orchestrator::Running;
using orchestrator::RunState;
using orchestrator::Seen;
using orchestrator::StatusReported;
using orchestrator::StepContext;

// {{{ the run every row starts from

constexpr auto MainCheckout = "/home/u/projects/contour"sv;
/// What `git rev-parse --path-format=absolute --git-common-dir` prints in an intact worktree of MainCheckout.
constexpr auto MainCommonDir = "/home/u/projects/contour/.git\n"sv;
constexpr auto IntactGitFile = "gitdir: /home/u/projects/contour/.git/worktrees/pr-2334-20261113-150847\n"sv;
constexpr auto HiddenKept =
    "Finished(completed, kept, new: a file is marked assume-unchanged or skip-worktree, so git "
    "status cannot see its changes)"sv;
constexpr auto SubmoduleKept =
    "Finished(completed, kept, new: a submodule is in the index: git status would run git in "
    "its own repository, and git worktree remove refuses it)"sv;
constexpr auto RedirectedKept =
    "Finished(completed, kept, new: git uses another directory than the main checkout's "
    ".git; nothing more was run there)"sv;
constexpr auto AlteredGitFile = "gitdir: /tmp/elsewhere/.git/worktrees/pr-2334-20261113-150847\n"sv;
constexpr auto StartedMs = uint64_t { 1'794'578'927'000 };
constexpr auto NowMs = uint64_t { 1'794'580'000'000 };

/// The item a row's run works on.
enum class Item : uint8_t
{
    Pr,
    Issue,
    Jira,
};

[[nodiscard]] ItemRef itemRef(Item item)
{
    switch (item)
    {
        case Item::Pr: return ChangeRequestRef { .number = 2334 };
        case Item::Issue: return IssueRef { .number = 2301 };
        case Item::Jira: return JiraRef { .key = "CT-418" };
    }
    return ChangeRequestRef {};
}

[[nodiscard]] Run makeRun(Item item, RunState state)
{
    return Run {
        .id = "pr-2334-20261113-150847",
        .repo = "contour",
        .repoPath = std::string(MainCheckout),
        .forge = orchestrator::Forge::GitHub,
        .remote = "origin",
        .base = "master",
        .item = itemRef(item),
        .url = "https://github.com/contour-terminal/contour/pull/2334",
        .task = "review",
        .model = "opus",
        .prompt = "Review https://github.com/contour-terminal/contour/pull/2334",
        .session = "123e4567-e89b-12d3-a456-426614174000",
        .branch = "agent/pr-2334-20261113-150847",
        .worktree = "/home/u/worktrees/contour/pr-2334-20261113-150847",
        .pane = std::nullopt,
        .state = std::move(state),
        .lastStatus = std::nullopt,
        .failure = std::nullopt,
        .startedWallMs = StartedMs,
        .attempt = 3,
        .runningSinceMs = 0,
        .changedMonoMs = 0,
    };
}

[[nodiscard]] StepContext knownContext()
{
    return StepContext { .mainCheckout = MainCheckout, .wallMs = NowMs };
}

/// The effects beginRun() returns; none when it refuses.
[[nodiscard]] std::vector<RunEffect> beginEffects(Run& run)
{
    return orchestrator::beginRun(run, knownContext()).value_or(std::vector<RunEffect> {});
}

// }}}
// {{{ descriptions

template <typename E>
struct Spelling
{
    E value;
    std::string_view text;
};

template <typename E, std::size_t N>
[[nodiscard]] std::string_view spell(std::array<Spelling<E>, N> const& table, E value)
{
    auto const row = std::ranges::find(table, value, &Spelling<E>::value);
    return row != table.end() ? row->text : "?"sv;
}

constexpr auto PrepareSteps = std::array {
    Spelling { PrepareStep::Fetch, "fetch"sv },
    Spelling { PrepareStep::WorktreeAdd, "worktree-add"sv },
    Spelling { PrepareStep::Checkout, "checkout"sv },
    Spelling { PrepareStep::OpenPane, "open-pane"sv },
};

constexpr auto EndSteps = std::array {
    Spelling { EndStep::ClosePane, "close-pane"sv },
    Spelling { EndStep::MainCheckout, "main-checkout"sv },
    Spelling { EndStep::Verify, "verify"sv },
    Spelling { EndStep::CommonDir, "common-dir"sv },
    Spelling { EndStep::IndexFlags, "index-flags"sv },
    Spelling { EndStep::Reachable, "reachable"sv },
    Spelling { EndStep::OperationState, "operation-state"sv },
    Spelling { EndStep::Status, "status"sv },
    Spelling { EndStep::CleanStatusBoard, "clean-status-board"sv },
    Spelling { EndStep::Remove, "remove"sv },
};

constexpr auto EndedNames = std::array {
    Spelling { Ended::Completed, "completed"sv },
    Spelling { Ended::EndedByYou, "ended-by-you"sv },
    Spelling { Ended::PreparationFailed, "preparation-failed"sv },
    Spelling { Ended::NeverReported, "never-reported"sv },
    Spelling { Ended::WorktreeGone, "worktree-gone"sv },
};

constexpr auto FateNames = std::array {
    Spelling { Fate::Removed, "removed"sv },
    Spelling { Fate::Kept, "kept"sv },
};

constexpr auto SeenNames = std::array {
    Spelling { Seen::New, "new"sv },
    Spelling { Seen::Opened, "opened"sv },
};

[[nodiscard]] std::string describeFinished(Finished const& finished)
{
    auto text = std::format("Finished({}, {}, {}",
                            spell(EndedNames, finished.ended),
                            spell(FateNames, finished.fate),
                            spell(SeenNames, finished.seen));
    for (auto const& [index, entry]: std::views::zip(std::views::iota(0), finished.entries))
        text += std::format("{}{}", index == 0 ? ": " : " | ", entry);
    return text + ")";
}

[[nodiscard]] std::string describe(RunState const& state)
{
    struct Describer
    {
        std::string operator()(Preparing const& p) const
        {
            return std::format("Preparing({})", spell(PrepareSteps, p.step));
        }
        std::string operator()(Running const&) const { return "Running"; }
        std::string operator()(Blocked const&) const { return "Blocked"; }
        std::string operator()(Done const&) const { return "Done"; }
        std::string operator()(Ending const& e) const
        {
            return std::format("Ending({}, {}{}{})",
                               spell(EndSteps, e.step),
                               spell(EndedNames, e.cause),
                               e.resumedFrom ? ", resumed" : "",
                               e.removal == Removal::Issued ? ", removal issued" : "");
        }
        std::string operator()(Finished const& f) const { return describeFinished(f); }
    };
    return std::visit(Describer {}, state);
}

[[nodiscard]] std::string describe(RunEffect const& effect)
{
    struct Describer
    {
        std::string operator()(RunDeclared const& declared) const
        {
            auto text = std::format("run {}", declared.name);
            for (auto const& argument: declared.arguments)
                text += std::format(" {}", argument.name);
            return text;
        }
        std::string operator()(ReadGitFile const&) const { return "read-git-file"; }
        std::string operator()(OpenAgentPane const&) const { return "open-pane"; }
        std::string operator()(CloseAgentPane const&) const { return "close-pane"; }
        std::string operator()(Persist const&) const { return "persist"; }
        std::string operator()(Forget const&) const { return "forget"; }
    };
    return std::visit(Describer {}, effect);
}

[[nodiscard]] std::string describe(std::vector<RunEffect> const& effects)
{
    auto text = std::string {};
    for (auto const& effect: effects)
        text += (text.empty() ? "" : ", ") + describe(effect);
    return text;
}

[[nodiscard]] std::string describe(std::optional<RunFailure> const& failure)
{
    return failure ? std::format("{}: {}", spell(PrepareSteps, failure->step), failure->why) : std::string {};
}

// }}}
// {{{ the transition table

/// Whether the run carries a preparation failure before the row's input.
enum class Carried : uint8_t
{
    Nothing,
    Failure,
};

/// What a row expects a predicate to answer.
enum class Expect : uint8_t
{
    No,
    Yes,
};

/// What the step knows about the main checkout.
enum class Checkout : uint8_t
{
    Known,
    Unknown, ///< For good: CheckoutUnknown::Unavailable.
    Pending, ///< Not yet: CheckoutUnknown::Pending.
};

struct TransitionRow
{
    std::string_view from;
    RunState state;
    RunInput input;
    std::string_view to = {}; ///< Empty: unchanged.
    std::string_view effects = {};
    Item item = Item::Pr;
    Carried carried = Carried::Nothing;
    std::optional<AgentStatus> lastStatus = std::nullopt;
    std::string_view failureAfter = {};
    Checkout checkout = Checkout::Known;
};

[[nodiscard]] Ending endingAt(EndStep step, Ended cause = Ended::Completed)
{
    // A run reaches Remove only through the step that asks for the removal.
    return Ending { .step = step,
                    .cause = cause,
                    .resumedFrom = std::nullopt,
                    .removal = step == EndStep::Remove ? Removal::Issued : Removal::NotIssued };
}

[[nodiscard]] Finished kept(Ended ended, Seen seen = Seen::New)
{
    return Finished {
        .ended = ended, .fate = Fate::Kept, .entries = { "M a.txt" }, .seen = seen, .endedWallMs = StartedMs
    };
}

/// A kept entry being checked again, at @p step.
[[nodiscard]] Ending resumedAt(EndStep step)
{
    auto ending = endingAt(step);
    ending.resumedFrom = kept(Ended::Completed, Seen::Opened);
    return ending;
}

[[nodiscard]] Finished removed(Ended ended)
{
    return Finished {
        .ended = ended, .fate = Fate::Removed, .entries = {}, .seen = Seen::New, .endedWallMs = StartedMs
    };
}

[[nodiscard]] GitFileRead gitFileError(FileReadError error)
{
    return GitFileRead { .content = std::unexpected(error) };
}

constexpr auto RebaseAtBreak =
    "interactive rebase in progress; onto 1234567\n"
    "Last command done (1 command done):\n"
    "   break\n"
    "You are currently editing a commit while rebasing branch 'agent/x' on '1234567'.\n"
    "\n"
    "nothing to commit, working tree clean\n"sv;
constexpr auto CleanLongStatus = "On branch agent/x\nnothing to commit, working tree clean\n"sv;

/// Every row of the run's transition table. Effects are listed with the names of the arguments they pass.
[[nodiscard]] std::vector<TransitionRow> transitionRows()
{
    constexpr auto VerifyEffects = "persist, read-git-file"sv;
    auto rows = std::vector<TransitionRow> {
        // Preparing(Fetch): no worktree can exist yet.
        { .from = "fetch ok, PR",
          .state = Preparing { PrepareStep::Fetch },
          .input = CommandOk {},
          .to = "Preparing(worktree-add)",
          .effects = "persist, run git-worktree-add path base" },
        { .from = "fetch ok, issue",
          .state = Preparing { PrepareStep::Fetch },
          .input = CommandOk {},
          .to = "Preparing(worktree-add)",
          .effects = "persist, run git-worktree-add-branch branch path base",
          .item = Item::Issue },
        { .from = "fetch ok, Jira",
          .state = Preparing { PrepareStep::Fetch },
          .input = CommandOk {},
          .to = "Preparing(worktree-add)",
          .effects = "persist, run git-worktree-add-branch branch path base",
          .item = Item::Jira },
        { .from = "fetch failed",
          .state = Preparing { PrepareStep::Fetch },
          .input = CommandFailed { "could not resolve\x1b[31m host" },
          .to = "Finished(preparation-failed, removed, new)",
          .effects = "persist",
          .failureAfter = "fetch: could not resolve[31m host" },
        { .from = "fetch lost",
          .state = Preparing { PrepareStep::Fetch },
          .input = Lost {},
          .to = "Finished(preparation-failed, removed, new)",
          .effects = "persist",
          .failureAfter = "fetch: the step was lost" },
        { .from = "fetch in a missing directory",
          .state = Preparing { PrepareStep::Fetch },
          .input = CwdMissing {},
          .to = "Finished(preparation-failed, removed, new)",
          .effects = "persist",
          .failureAfter = "fetch: its directory is gone" },
        // Preparing(WorktreeAdd)
        { .from = "worktree-add ok, PR",
          .state = Preparing { PrepareStep::WorktreeAdd },
          .input = CommandOk {},
          .to = "Preparing(checkout)",
          .effects = "persist, run gh-pr-checkout number branch" },
        { .from = "worktree-add ok, issue",
          .state = Preparing { PrepareStep::WorktreeAdd },
          .input = CommandOk {},
          .to = "Preparing(open-pane)",
          .effects = "persist, open-pane",
          .item = Item::Issue },
        { .from = "worktree-add in a missing directory",
          .state = Preparing { PrepareStep::WorktreeAdd },
          .input = CwdMissing {},
          .to = "Finished(preparation-failed, removed, new)",
          .effects = "persist",
          .failureAfter = "worktree-add: its directory is gone" },
        // Preparing(Checkout)
        { .from = "checkout ok",
          .state = Preparing { PrepareStep::Checkout },
          .input = CommandOk {},
          .to = "Preparing(open-pane)",
          .effects = "persist, open-pane" },
        { .from = "checkout in a missing directory",
          .state = Preparing { PrepareStep::Checkout },
          .input = CwdMissing {},
          .to = "Finished(preparation-failed, removed, new)",
          .effects = "persist",
          .failureAfter = "checkout: its directory is gone" },
        // Preparing(OpenPane)
        { .from = "pane opened",
          .state = Preparing { PrepareStep::OpenPane },
          .input = PaneOpened { .pane = "pane-7", .monoMs = 42 },
          .to = "Running",
          .effects = "persist" },
        // Running, Blocked, Done
        { .from = "running, idle reported",
          .state = Running {},
          .input = StatusReported { AgentStatus::Idle },
          .to = "Running",
          .effects = "persist" },
        { .from = "running, working reported",
          .state = Running {},
          .input = StatusReported { AgentStatus::Working },
          .to = "Running",
          .effects = "persist" },
        { .from = "running, blocked reported",
          .state = Running {},
          .input = StatusReported { AgentStatus::Blocked },
          .to = "Blocked",
          .effects = "persist" },
        { .from = "running, error reported",
          .state = Running {},
          .input = StatusReported { AgentStatus::Error },
          .to = "Blocked",
          .effects = "persist" },
        { .from = "running, done reported",
          .state = Running {},
          .input = StatusReported { AgentStatus::Done },
          .to = "Done",
          .effects = "persist" },
        { .from = "running, the same status again",
          .state = Running {},
          .input = StatusReported { AgentStatus::Working },
          .to = "Running",
          .effects = "",
          .lastStatus = AgentStatus::Working },
        { .from = "running, idle after working",
          .state = Running {},
          .input = StatusReported { AgentStatus::Idle },
          .to = "Running",
          .effects = "persist",
          .lastStatus = AgentStatus::Working },
        { .from = "blocked, working reported",
          .state = Blocked {},
          .input = StatusReported { AgentStatus::Working },
          .to = "Running",
          .effects = "persist",
          .lastStatus = AgentStatus::Blocked },
        { .from = "done, working reported",
          .state = Done {},
          .input = StatusReported { AgentStatus::Working },
          .to = "Running",
          .effects = "persist",
          .lastStatus = AgentStatus::Done },
        { .from = "done, done again",
          .state = Done {},
          .input = StatusReported { AgentStatus::Done },
          .to = "Done",
          .effects = "",
          .lastStatus = AgentStatus::Done },
        { .from = "running, pane gone after a report",
          .state = Running {},
          .input = PaneGone {},
          .to = "Ending(verify, completed)",
          .effects = VerifyEffects,
          .lastStatus = AgentStatus::Working },
        // The checks compare the worktree against the main checkout: before it is known, nothing is read.
        { .from = "running, pane gone, main checkout not known yet",
          .state = Running {},
          .input = PaneGone {},
          .to = "Ending(main-checkout, completed)",
          .effects = "persist",
          .lastStatus = AgentStatus::Working,
          .checkout = Checkout::Pending },
        // Ending(MainCheckout): waiting for a listing to name the main checkout.
        { .from = "main-checkout, listed",
          .state = endingAt(EndStep::MainCheckout),
          .input = CheckoutListed {},
          .to = "Ending(verify, completed)",
          .effects = VerifyEffects },
        { .from = "main-checkout, listed, none to be known",
          .state = endingAt(EndStep::MainCheckout),
          .input = CheckoutListed {},
          .to = "Ending(verify, completed)",
          .effects = VerifyEffects,
          .checkout = Checkout::Unknown },
        { .from = "main-checkout, still not known",
          .state = endingAt(EndStep::MainCheckout),
          .input = CheckoutListed {},
          .effects = "",
          .checkout = Checkout::Pending },
        { .from = "main-checkout, a stray file read",
          .state = endingAt(EndStep::MainCheckout),
          .input = GitFileRead { .content = IntactGitFile },
          .effects = "" },
        { .from = "main-checkout, a stray command result",
          .state = endingAt(EndStep::MainCheckout),
          .input = CommandOk { MainCommonDir },
          .effects = "" },
        { .from = "verify, listed while not ending",
          .state = endingAt(EndStep::Verify),
          .input = CheckoutListed {},
          .effects = "" },
        { .from = "blocked, pane gone after a report",
          .state = Blocked {},
          .input = PaneGone {},
          .to = "Ending(verify, completed)",
          .effects = VerifyEffects,
          .lastStatus = AgentStatus::Error },
        { .from = "done, pane gone",
          .state = Done {},
          .input = PaneGone {},
          .to = "Ending(verify, completed)",
          .effects = VerifyEffects,
          .lastStatus = AgentStatus::Done },
        { .from = "running, pane gone before any report",
          .state = Running {},
          .input = PaneGone {},
          .to = "Ending(verify, never-reported)",
          .effects = VerifyEffects },
        { .from = "running, ended by the user",
          .state = Running {},
          .input = EndByUser {},
          .to = "Ending(close-pane, ended-by-you)",
          .effects = "persist, close-pane",
          .lastStatus = AgentStatus::Working },
        { .from = "blocked, ended by the user",
          .state = Blocked {},
          .input = EndByUser {},
          .to = "Ending(close-pane, ended-by-you)",
          .effects = "persist, close-pane",
          .lastStatus = AgentStatus::Blocked },
        { .from = "done, ended by the user",
          .state = Done {},
          .input = EndByUser {},
          .to = "Ending(close-pane, ended-by-you)",
          .effects = "persist, close-pane",
          .lastStatus = AgentStatus::Done },
        // Ending(ClosePane): nothing touches the worktree while the tab may still run.
        { .from = "closing, pane gone",
          .state = endingAt(EndStep::ClosePane, Ended::EndedByYou),
          .input = PaneGone {},
          .to = "Ending(verify, ended-by-you)",
          .effects = VerifyEffects },
        { .from = "closing after a failed opening, pane gone",
          .state = endingAt(EndStep::ClosePane, Ended::PreparationFailed),
          .input = PaneGone {},
          .to = "Ending(verify, preparation-failed)",
          .effects = VerifyEffects,
          .carried = Carried::Failure,
          .failureAfter = "worktree-add: exit 128" },
        { .from = "closing, lost",
          .state = endingAt(EndStep::ClosePane, Ended::EndedByYou),
          .input = Lost {},
          .to = "Ending(close-pane, ended-by-you)",
          .effects = "persist, close-pane" },
        { .from = "closing after a failed opening, lost",
          .state = endingAt(EndStep::ClosePane, Ended::PreparationFailed),
          .input = Lost {},
          .to = "Ending(close-pane, preparation-failed)",
          .effects = "persist, close-pane",
          .carried = Carried::Failure,
          .failureAfter = "worktree-add: exit 128" },
        { .from = "closing, a git file read",
          .state = endingAt(EndStep::ClosePane, Ended::EndedByYou),
          .input = GitFileRead { .content = IntactGitFile },
          .to = "Ending(close-pane, ended-by-you)",
          .effects = "" },
        // Ending(Verify)
        { .from = "verify, intact",
          .state = endingAt(EndStep::Verify),
          .input = GitFileRead { .content = IntactGitFile },
          .to = "Ending(common-dir, completed)",
          .effects = "persist, run git-common-dir" },
        { .from = "verify, intact without a line end",
          .state = endingAt(EndStep::Verify),
          .input = GitFileRead { .content = IntactGitFile.substr(0, IntactGitFile.size() - 1) },
          .to = "Ending(common-dir, completed)",
          .effects = "persist, run git-common-dir" },
        { .from = "verify, altered",
          .state = endingAt(EndStep::Verify),
          .input = GitFileRead { .content = AlteredGitFile },
          .to = "Finished(completed, kept, new: .git file was changed; nothing was run there)",
          .effects = "persist" },
        { .from = "verify, main checkout unknown",
          .state = endingAt(EndStep::Verify),
          .input = GitFileRead { .content = IntactGitFile },
          .to = "Finished(completed, kept, new: the main checkout is not known; nothing was run there)",
          .effects = "persist",
          .checkout = Checkout::Unknown },
        { .from = "verify, main checkout not known yet",
          .state = endingAt(EndStep::Verify),
          .input = GitFileRead { .content = IntactGitFile },
          .to = "Ending(main-checkout, completed)",
          .effects = "persist",
          .checkout = Checkout::Pending },
        { .from = "verify of a kept entry, main checkout unknown",
          .state = resumedAt(EndStep::Verify),
          .input = GitFileRead { .content = IntactGitFile },
          .to = "Finished(completed, kept, opened: M a.txt)",
          .effects = "persist",
          .checkout = Checkout::Unknown },
        { .from = "verify, directory missing",
          .state = endingAt(EndStep::Verify),
          .input = gitFileError(FileReadError::DirectoryMissing),
          .to = "Finished(worktree-gone, removed, new)",
          .effects = "persist" },
        { .from = "verify, directory missing after a failed preparation",
          .state = endingAt(EndStep::Verify, Ended::PreparationFailed),
          .input = gitFileError(FileReadError::DirectoryMissing),
          .to = "Finished(preparation-failed, removed, new)",
          .effects = "persist",
          .carried = Carried::Failure,
          .failureAfter = "worktree-add: exit 128" },
        { .from = "verify, not a regular file",
          .state = endingAt(EndStep::Verify),
          .input = gitFileError(FileReadError::NotRegular),
          .to = "Finished(completed, kept, new: the .git entry is not a readable file: it is not a regular "
                "file)",
          .effects = "persist" },
        { .from = "verify, unreadable",
          .state = endingAt(EndStep::Verify),
          .input = gitFileError(FileReadError::Unreadable),
          .to = "Finished(completed, kept, new: the .git entry is not a readable file: it cannot be read)",
          .effects = "persist" },
        { .from = "verify, too large",
          .state = endingAt(EndStep::Verify),
          .input = gitFileError(FileReadError::TooLarge),
          .to = "Finished(completed, kept, new: the .git entry is not a readable file: it is too large)",
          .effects = "persist" },
        { .from = "verify, escapes",
          .state = endingAt(EndStep::Verify),
          .input = gitFileError(FileReadError::Escapes),
          .to = "Finished(completed, kept, new: the .git entry is not a readable file: it leads outside the "
                "worktree)",
          .effects = "persist" },
        { .from = "verify, lost",
          .state = endingAt(EndStep::Verify),
          .input = Lost {},
          .to = "Ending(verify, completed)",
          .effects = VerifyEffects },
        // Ending(CommonDir): git's own answer, after the .git file and before any other git command.
        { .from = "common-dir, the main checkout's .git",
          .state = endingAt(EndStep::CommonDir),
          .input = CommandOk { MainCommonDir },
          .to = "Ending(index-flags, completed)",
          .effects = "persist, run git-index-flags" },
        { .from = "common-dir, the main checkout's .git without a line end",
          .state = endingAt(EndStep::CommonDir),
          .input = CommandOk { "/home/u/projects/contour/.git" },
          .to = "Ending(index-flags, completed)",
          .effects = "persist, run git-index-flags" },
        { .from = "common-dir, redirected elsewhere",
          .state = endingAt(EndStep::CommonDir),
          .input = CommandOk { "/tmp/evil.git\n" },
          .to = RedirectedKept,
          .effects = "persist" },
        { .from = "common-dir, redirected to another repository's .git",
          .state = endingAt(EndStep::CommonDir),
          .input = CommandOk { "/home/u/projects/other/.git\n" },
          .to = RedirectedKept,
          .effects = "persist" },
        { .from = "common-dir, redirected below the main checkout",
          .state = endingAt(EndStep::CommonDir),
          .input = CommandOk { "/home/u/projects/contour/.git/x\n" },
          .to = RedirectedKept,
          .effects = "persist" },
        { .from = "common-dir, a sibling sharing the main checkout's prefix",
          .state = endingAt(EndStep::CommonDir),
          .input = CommandOk { "/home/u/projects/contour-evil/.git\n" },
          .to = RedirectedKept,
          .effects = "persist" },
        { .from = "common-dir, no output",
          .state = endingAt(EndStep::CommonDir),
          .input = CommandOk { "" },
          .to = RedirectedKept,
          .effects = "persist" },
        { .from = "common-dir, a second line",
          .state = endingAt(EndStep::CommonDir),
          .input = CommandOk { "/home/u/projects/contour/.git\n\n" },
          .to = RedirectedKept,
          .effects = "persist" },
        { .from = "common-dir, main checkout unknown",
          .state = endingAt(EndStep::CommonDir),
          .input = CommandOk { MainCommonDir },
          .to = "Finished(completed, kept, new: the main checkout is not known; nothing was run there)",
          .effects = "persist",
          .checkout = Checkout::Unknown },
        { .from = "common-dir, main checkout not known yet",
          .state = endingAt(EndStep::CommonDir),
          .input = CommandOk { MainCommonDir },
          .to = "Ending(main-checkout, completed)",
          .effects = "persist",
          .checkout = Checkout::Pending },
        { .from = "common-dir of a kept entry, main checkout unknown",
          .state = resumedAt(EndStep::CommonDir),
          .input = CommandOk { MainCommonDir },
          .to = "Finished(completed, kept, opened: M a.txt)",
          .effects = "persist",
          .checkout = Checkout::Unknown },
        // Ending(IndexFlags): `git ls-files -v --stage`, before anything that reads attributes.
        { .from = "index-flags, none set",
          .state = endingAt(EndStep::IndexFlags),
          .input = CommandOk { "H 100644 78981922613b2afb6025042ff6bd878ac1994e85 0\ta.txt\nH 100755 "
                               "78981922613b2afb6025042ff6bd878ac1994e85 0\tdir/b.sh\n" },
          .to = "Ending(reachable, completed)",
          .effects = "persist, run git-head-reachable" },
        { .from = "index-flags, an empty index",
          .state = endingAt(EndStep::IndexFlags),
          .input = CommandOk { "" },
          .to = "Ending(reachable, completed)",
          .effects = "persist, run git-head-reachable" },
        { .from = "index-flags, a symbolic link",
          .state = endingAt(EndStep::IndexFlags),
          .input = CommandOk { "H 120000 78981922613b2afb6025042ff6bd878ac1994e85 0\tlink\n" },
          .to = "Ending(reachable, completed)",
          .effects = "persist, run git-head-reachable" },
        { .from = "index-flags, an unmerged entry",
          .state = endingAt(EndStep::IndexFlags),
          .input = CommandOk { "M 100644 78981922613b2afb6025042ff6bd878ac1994e85 2\ta.txt\n" },
          .to = "Ending(reachable, completed)",
          .effects = "persist, run git-head-reachable" },
        { .from = "index-flags, a file marked assume-unchanged",
          .state = endingAt(EndStep::IndexFlags),
          .input = CommandOk { "H 100644 78981922613b2afb6025042ff6bd878ac1994e85 0\ta.txt\nh 100644 "
                               "78981922613b2afb6025042ff6bd878ac1994e85 0\tb.txt\n" },
          .to = HiddenKept,
          .effects = "persist" },
        { .from = "index-flags, a file marked skip-worktree",
          .state = endingAt(EndStep::IndexFlags),
          .input = CommandOk { "S 100644 78981922613b2afb6025042ff6bd878ac1994e85 0\tb.txt\nH 100644 "
                               "78981922613b2afb6025042ff6bd878ac1994e85 0\tc.txt\n" },
          .to = HiddenKept,
          .effects = "persist" },
        { .from = "index-flags, a file marked both",
          .state = endingAt(EndStep::IndexFlags),
          .input = CommandOk { "s 100644 78981922613b2afb6025042ff6bd878ac1994e85 0\tb.txt\n" },
          .to = HiddenKept,
          .effects = "persist" },
        { .from = "index-flags, a submodule",
          .state = endingAt(EndStep::IndexFlags),
          .input = CommandOk { "H 100644 78981922613b2afb6025042ff6bd878ac1994e85 0\ta.txt\nH 160000 "
                               "78981922613b2afb6025042ff6bd878ac1994e85 0\tsub\n" },
          .to = SubmoduleKept,
          .effects = "persist" },
        { .from = "index-flags, a submodule and a hidden file",
          .state = endingAt(EndStep::IndexFlags),
          .input = CommandOk { "h 100644 78981922613b2afb6025042ff6bd878ac1994e85 0\ta.txt\nH 160000 "
                               "78981922613b2afb6025042ff6bd878ac1994e85 0\tsub\n" },
          .to = SubmoduleKept,
          .effects = "persist" },
        { .from = "index-flags, the old format without --stage",
          .state = endingAt(EndStep::IndexFlags),
          .input = CommandOk { "H a.txt\n" },
          .to = "Finished(completed, kept, new: unrecognised git ls-files output)",
          .effects = "persist" },
        { .from = "index-flags, a line without a mode",
          .state = endingAt(EndStep::IndexFlags),
          .input = CommandOk { "H  x 0\ta.txt\n" },
          .to = "Finished(completed, kept, new: unrecognised git ls-files output)",
          .effects = "persist" },
        // Ending(Reachable)
        { .from = "reachable, HEAD in no ref",
          .state = endingAt(EndStep::Reachable),
          .input = CommandOk { "" },
          .to = "Finished(completed, kept, new: HEAD is not contained in any ref)",
          .effects = "persist" },
        { .from = "reachable, only a line end",
          .state = endingAt(EndStep::Reachable),
          .input = CommandOk { "\n" },
          .to = "Finished(completed, kept, new: HEAD is not contained in any ref)",
          .effects = "persist" },
        { .from = "reachable, HEAD in a ref",
          .state = endingAt(EndStep::Reachable),
          .input = CommandOk { "1234567 commit\trefs/heads/agent/pr-2334-20261113-150847\n" },
          .to = "Ending(operation-state, completed)",
          .effects = "persist, run git-operation-state" },
        // Ending(OperationState)
        { .from = "operation-state, a rebase stopped at break",
          .state = endingAt(EndStep::OperationState),
          .input = CommandOk { RebaseAtBreak },
          .to = "Finished(completed, kept, new: rebase in progress)",
          .effects = "persist" },
        { .from = "operation-state, a bisect",
          .state = endingAt(EndStep::OperationState, Ended::NeverReported),
          .input =
              CommandOk {
                  "On branch agent/x\nYou are currently bisecting, started from branch 'agent/x'.\n" },
          .to = "Finished(never-reported, kept, new: bisect in progress)",
          .effects = "persist" },
        { .from = "operation-state, a first line of unknown shape",
          .state = endingAt(EndStep::OperationState),
          .input = CommandOk { "Currently on branch agent/x\nnothing to commit\n" },
          .to = "Finished(completed, kept, new: unrecognised git status)",
          .effects = "persist" },
        { .from = "operation-state, no output",
          .state = endingAt(EndStep::OperationState),
          .input = CommandOk { "" },
          .to = "Finished(completed, kept, new: unrecognised git status)",
          .effects = "persist" },
        { .from = "operation-state, none",
          .state = endingAt(EndStep::OperationState),
          .input = CommandOk { CleanLongStatus },
          .to = "Ending(status, completed)",
          .effects = "persist, run git-status" },
        // Ending(Status)
        { .from = "status, clean",
          .state = endingAt(EndStep::Status),
          .input = CommandOk { "" },
          .to = "Ending(remove, completed, removal issued)",
          .effects = "persist, run git-worktree-remove path" },
        { .from = "status, only the status board",
          .state = endingAt(EndStep::Status),
          .input = CommandOk { "?? STATUS.md\n" },
          .to = "Ending(clean-status-board, completed)",
          .effects = "persist, run git-clean" },
        { .from = "status, modified and untracked",
          .state = endingAt(EndStep::Status),
          .input = CommandOk { " M a.txt\n?? b\x1b[2Jc.txt\n" },
          .to = "Finished(completed, kept, new: M a.txt | ?? b[2Jc.txt)",
          .effects = "persist" },
        { .from = "status, the status board and more",
          .state = endingAt(EndStep::Status),
          .input = CommandOk { "?? STATUS.md\n?? notes.txt\n" },
          .to = "Finished(completed, kept, new: ?? STATUS.md | ?? notes.txt)",
          .effects = "persist" },
        { .from = "status, a status board in a directory",
          .state = endingAt(EndStep::Status),
          .input = CommandOk { "?? STATUS.md/\n" },
          .to = "Finished(completed, kept, new: ?? STATUS.md/)",
          .effects = "persist" },
        // Ending(CleanStatusBoard), Ending(Remove)
        { .from = "clean-status-board ok",
          .state = endingAt(EndStep::CleanStatusBoard),
          .input = CommandOk { "Removing STATUS.md\n" },
          .to = "Ending(remove, completed, removal issued)",
          .effects = "persist, run git-worktree-remove path" },
        { .from = "remove ok",
          .state = endingAt(EndStep::Remove),
          .input = CommandOk {},
          .to = "Finished(completed, removed, new)",
          .effects = "persist" },
        { .from = "remove ok, ended by the user",
          .state = endingAt(EndStep::Remove, Ended::EndedByYou),
          .input = CommandOk {},
          .to = "Finished(ended-by-you, removed, new)",
          .effects = "persist" },
        { .from = "remove ok after a failed preparation",
          .state = endingAt(EndStep::Remove, Ended::PreparationFailed),
          .input = CommandOk {},
          .to = "Finished(preparation-failed, removed, new)",
          .effects = "persist",
          .carried = Carried::Failure,
          .failureAfter = "worktree-add: exit 128" },
        // Finished
        { .from = "kept, retried",
          .state = kept(Ended::Completed),
          .input = RetryRemoval {},
          .to = "Ending(verify, completed, resumed)",
          .effects = VerifyEffects },
        { .from = "kept, retried before the main checkout is known",
          .state = kept(Ended::Completed),
          .input = RetryRemoval {},
          .to = "Ending(main-checkout, completed, resumed)",
          .effects = "persist",
          .checkout = Checkout::Pending },
        { .from = "kept after a failed preparation, retried",
          .state = kept(Ended::PreparationFailed),
          .input = RetryRemoval {},
          .to = "Ending(verify, preparation-failed, resumed)",
          .effects = VerifyEffects,
          .carried = Carried::Failure,
          .failureAfter = "worktree-add: exit 128" },
        { .from = "kept, opened",
          .state = kept(Ended::Completed),
          .input = MarkSeen {},
          .to = "Finished(completed, kept, opened: M a.txt)",
          .effects = "persist" },
        { .from = "kept, opened again",
          .state = kept(Ended::Completed, Seen::Opened),
          .input = MarkSeen {},
          .to = "Finished(completed, kept, opened: M a.txt)",
          .effects = "" },
        { .from = "removed, opened",
          .state = removed(Ended::Completed),
          .input = MarkSeen {},
          .to = "Finished(completed, removed, opened)",
          .effects = "persist" },
        { .from = "removed, dismissed",
          .state = removed(Ended::WorktreeGone),
          .input = Dismiss {},
          .to = "Finished(worktree-gone, removed, new)",
          .effects = "forget" },
        // Inputs a state does not await leave it unchanged.
        { .from = "kept, dismissed",
          .state = kept(Ended::Completed),
          .input = Dismiss {},
          .to = "Finished(completed, kept, new: M a.txt)",
          .effects = "" },
        { .from = "removed, retried",
          .state = removed(Ended::Completed),
          .input = RetryRemoval {},
          .to = "Finished(completed, removed, new)",
          .effects = "" },
        { .from = "running, retried",
          .state = Running {},
          .input = RetryRemoval {},
          .to = "Running",
          .effects = "",
          .lastStatus = AgentStatus::Working },
        { .from = "running, dismissed",
          .state = Running {},
          .input = Dismiss {},
          .to = "Running",
          .effects = "" },
        { .from = "running, a command result",
          .state = Running {},
          .input = CommandOk {},
          .to = "Running",
          .effects = "" },
        { .from = "finished, ended by the user",
          .state = kept(Ended::Completed),
          .input = EndByUser {},
          .to = "Finished(completed, kept, new: M a.txt)",
          .effects = "" },
        { .from = "finished, pane gone",
          .state = removed(Ended::Completed),
          .input = PaneGone {},
          .to = "Finished(completed, removed, new)",
          .effects = "" },
        { .from = "ending, ended by the user",
          .state = endingAt(EndStep::Status),
          .input = EndByUser {},
          .to = "Ending(status, completed)",
          .effects = "" },
        { .from = "ending, a status report",
          .state = endingAt(EndStep::Reachable),
          .input = StatusReported { AgentStatus::Working },
          .to = "Ending(reachable, completed)",
          .effects = "" },
        { .from = "ending, pane gone",
          .state = endingAt(EndStep::Verify),
          .input = PaneGone {},
          .to = "Ending(verify, completed)",
          .effects = "" },
        { .from = "verify, a command result",
          .state = endingAt(EndStep::Verify),
          .input = CommandOk {},
          .to = "Ending(verify, completed)",
          .effects = "" },
        { .from = "status, a git file read",
          .state = endingAt(EndStep::Status),
          .input = GitFileRead { .content = IntactGitFile },
          .to = "Ending(status, completed)",
          .effects = "" },
        { .from = "fetch, a pane opened",
          .state = Preparing { PrepareStep::Fetch },
          .input = PaneOpened { .pane = "pane-7", .monoMs = 1 },
          .to = "Preparing(fetch)",
          .effects = "" },
        { .from = "fetch, a pane that failed to open",
          .state = Preparing { PrepareStep::Fetch },
          .input = PaneOpenFailed { "no" },
          .to = "Preparing(fetch)",
          .effects = "" },
        { .from = "open-pane, a command result",
          .state = Preparing { PrepareStep::OpenPane },
          .input = CommandOk {},
          .to = "Preparing(open-pane)",
          .effects = "" },
        { .from = "open-pane in a missing directory",
          .state = Preparing { PrepareStep::OpenPane },
          .input = CwdMissing {},
          .to = "Preparing(open-pane)",
          .effects = "" },
    };

    // A preparation step that may have made a worktree fails through the removal checks.
    struct FailedInput
    {
        std::string_view name;
        RunInput input;
        std::string_view why;
    };
    auto const failedInputs = std::array {
        FailedInput { .name = "command failed", .input = CommandFailed { "exit 128" }, .why = "exit 128" },
        FailedInput { .name = "lost", .input = Lost {}, .why = "the step was lost" },
        FailedInput { .name = "pane failed", .input = PaneOpenFailed { "no tab" }, .why = "no tab" },
    };
    constexpr auto FailedFailures = std::array {
        std::array {
            "worktree-add: exit 128"sv, "worktree-add: the step was lost"sv, "worktree-add: no tab"sv },
        std::array { "checkout: exit 128"sv, "checkout: the step was lost"sv, "checkout: no tab"sv },
        std::array { "open-pane: exit 128"sv, "open-pane: the step was lost"sv, "open-pane: no tab"sv },
    };
    constexpr auto FailingSteps =
        std::array { PrepareStep::WorktreeAdd, PrepareStep::Checkout, PrepareStep::OpenPane };
    // A tab may exist once its opening was asked for: the removal checks wait until it is gone.
    for (auto const& [step, failures]: std::views::zip(FailingSteps, FailedFailures))
        for (auto const& [failed, failure]: std::views::zip(failedInputs, failures))
            rows.push_back({ .from = failed.name,
                             .state = Preparing { step },
                             .input = failed.input,
                             .to = step == PrepareStep::OpenPane ? "Ending(close-pane, preparation-failed)"
                                                                 : "Ending(verify, preparation-failed)",
                             .effects = step == PrepareStep::OpenPane ? "persist, close-pane" : VerifyEffects,
                             .failureAfter = failure });

    // The command steps of the removal checks: a failure keeps the worktree, a missing directory forgets it.
    struct CommandStep
    {
        EndStep step;
        std::string_view kept;
    };
    constexpr auto CommandSteps = std::array {
        CommandStep { .step = EndStep::CommonDir,
                      .kept =
                          "Finished(completed, kept, new: the common-directory check failed: timed out)" },
        CommandStep { .step = EndStep::Reachable,
                      .kept = "Finished(completed, kept, new: the reachability check failed: timed out)" },
        CommandStep { .step = EndStep::OperationState,
                      .kept = "Finished(completed, kept, new: the operation-state check failed: timed out)" },
        CommandStep { .step = EndStep::IndexFlags,
                      .kept = "Finished(completed, kept, new: the index check failed: timed out)" },
        CommandStep { .step = EndStep::Status,
                      .kept = "Finished(completed, kept, new: git status failed: timed out)" },
        CommandStep { .step = EndStep::CleanStatusBoard,
                      .kept = "Finished(completed, kept, new: removing STATUS.md failed: timed out)" },
        CommandStep { .step = EndStep::Remove,
                      .kept = "Finished(completed, kept, new: git worktree remove failed: timed out)" },
    };
    for (auto const& command: CommandSteps)
    {
        rows.push_back({ .from = "command failed",
                         .state = endingAt(command.step),
                         .input = CommandFailed { "timed out" },
                         .to = command.kept,
                         .effects = "persist" });
        // Once the removal was asked for, a worktree found gone may be its doing: the run ended as it did.
        auto const removing = command.step == EndStep::Remove;
        rows.push_back(
            { .from = "missing directory",
              .state = endingAt(command.step),
              .input = CwdMissing {},
              .to = removing ? "Finished(completed, removed, new)" : "Finished(worktree-gone, removed, new)",
              .effects = "persist" });
        rows.push_back({ .from = "lost",
                         .state = endingAt(command.step, Ended::NeverReported),
                         .input = Lost {},
                         .to = removing ? "Ending(verify, never-reported, removal issued)"
                                        : "Ending(verify, never-reported)",
                         .effects = VerifyEffects });
        rows.push_back(
            { .from = "a pane gone", .state = endingAt(command.step), .input = PaneGone {}, .effects = "" });
    }
    // Inputs Preparing does not await, at every step.
    for (auto const step: FailingSteps)
    {
        rows.push_back({ .from = "a status report",
                         .state = Preparing { step },
                         .input = StatusReported {},
                         .effects = "" });
        rows.push_back(
            { .from = "a pane gone", .state = Preparing { step }, .input = PaneGone {}, .effects = "" });
        rows.push_back({ .from = "ended by the user",
                         .state = Preparing { step },
                         .input = EndByUser {},
                         .effects = "" });
    }
    return rows;
}

[[nodiscard]] Run runFor(TransitionRow const& row)
{
    auto run = makeRun(row.item, row.state);
    run.lastStatus = row.lastStatus;
    if (row.carried == Carried::Failure)
        run.failure = RunFailure { .step = PrepareStep::WorktreeAdd, .why = "exit 128" };
    return run;
}

[[nodiscard]] StepContext contextFor(TransitionRow const& row)
{
    auto context = knownContext();
    if (row.checkout != Checkout::Known)
        context.mainCheckout = std::nullopt;
    context.unknownCheckout =
        row.checkout == Checkout::Pending ? CheckoutUnknown::Pending : CheckoutUnknown::Unavailable;
    return context;
}

// }}}

} // namespace

TEST_CASE("the run's transition table", "[orchestrator][run]")
{
    for (auto const& row: transitionRows())
    {
        auto run = runFor(row);
        auto const before = describe(run.state);
        auto const effects = orchestrator::advance(run, row.input, contextFor(row));
        // An empty `to` reads "unchanged".
        auto const to = row.to.empty() ? before : std::string(row.to);
        CAPTURE(row.from, before, row.input.index());
        CHECK(describe(run.state) == to);
        CHECK(describe(effects) == row.effects);
        CHECK(describe(run.failure) == row.failureAfter);
    }
}

TEST_CASE("reachability keeps a detached-HEAD commit", "[orchestrator][run]")
{
    // Measured: after a commit on a detached HEAD, `git for-each-ref --contains HEAD` prints nothing and
    // `git status --porcelain` is empty, so only this step stands between the commit and its loss.
    auto run = makeRun(Item::Pr, endingAt(EndStep::Reachable));
    auto const effects = orchestrator::advance(run, CommandOk { "" }, knownContext());
    auto const* const finished = std::get_if<Finished>(&run.state);
    REQUIRE(finished != nullptr);
    CHECK(finished->fate == Fate::Kept);
    CHECK(finished->entries == std::vector<std::string> { "HEAD is not contained in any ref" });
    CHECK(describe(effects) == "persist");
}

TEST_CASE("a .git that became a directory keeps the run", "[orchestrator][run]")
{
    struct Row
    {
        FileReadError error;
        Fate fate;
    };
    // Only a missing worktree directory lets the run be forgotten.
    constexpr auto Rows = std::array {
        Row { .error = FileReadError::DirectoryMissing, .fate = Fate::Removed },
        Row { .error = FileReadError::NotRegular, .fate = Fate::Kept },
        Row { .error = FileReadError::Unreadable, .fate = Fate::Kept },
        Row { .error = FileReadError::TooLarge, .fate = Fate::Kept },
        Row { .error = FileReadError::Escapes, .fate = Fate::Kept },
    };
    for (auto const& row: Rows)
    {
        CAPTURE(row.error);
        auto run = makeRun(Item::Pr, endingAt(EndStep::Verify));
        auto const effects = orchestrator::advance(run, gitFileError(row.error), knownContext());
        auto const* const finished = std::get_if<Finished>(&run.state);
        REQUIRE(finished != nullptr);
        CHECK(finished->fate == row.fate);
        CHECK(describe(effects) == "persist");
        // A kept run cannot be dismissed: it stays in the store until its worktree is gone.
        CHECK(describe(orchestrator::advance(run, Dismiss {}, knownContext()))
              == (row.fate == Fate::Kept ? "" : "forget"));
    }
}

TEST_CASE("a run that never reported is its own ended value", "[orchestrator][run]")
{
    auto run = makeRun(Item::Issue, Running {});
    run.pane = "pane-7";
    auto const context = knownContext();
    CHECK(describe(orchestrator::advance(run, PaneGone {}, context)) == "persist, read-git-file");
    CHECK_FALSE(run.pane.has_value());
    CHECK_FALSE(run.lastStatus.has_value());
    CHECK(describe(orchestrator::advance(run, GitFileRead { .content = IntactGitFile }, context))
          == "persist, run git-common-dir");
    CHECK(describe(orchestrator::advance(run, CommandOk { MainCommonDir }, context))
          == "persist, run git-index-flags");
    CHECK(describe(orchestrator::advance(
              run, CommandOk { "H 100644 78981922613b2afb6025042ff6bd878ac1994e85 0\ta.txt\n" }, context))
          == "persist, run git-head-reachable");
    CHECK(describe(orchestrator::advance(run, CommandOk { "c\trefs/heads/agent/x\n" }, context))
          == "persist, run git-operation-state");
    CHECK(describe(orchestrator::advance(run, CommandOk { CleanLongStatus }, context))
          == "persist, run git-status");
    CHECK(describe(orchestrator::advance(run, CommandOk { "" }, context))
          == "persist, run git-worktree-remove path");
    CHECK(describe(orchestrator::advance(run, CommandOk { "" }, context)) == "persist");
    CHECK(describe(run.state) == "Finished(never-reported, removed, new)");
    CHECK_FALSE(run.lastStatus.has_value());
    auto const* const finished = std::get_if<Finished>(&run.state);
    REQUIRE(finished != nullptr);
    CHECK(finished->endedWallMs == NowMs);
}

TEST_CASE("a worktree gone after its own removal was asked for is removed, not gone", "[orchestrator][run]")
{
    // The remove succeeded, then the guest restarted before its result arrived.
    auto const context = knownContext();
    auto run = makeRun(Item::Pr, endingAt(EndStep::Status, Ended::EndedByYou));
    CHECK(describe(orchestrator::advance(run, CommandOk { "" }, context))
          == "persist, run git-worktree-remove path");
    auto const stored = orchestrator::runFromJson(orchestrator::toJson(run));
    REQUIRE(stored.has_value());
    run = *stored;
    CHECK(describe(orchestrator::advance(run, Lost {}, context)) == "persist, read-git-file");
    CHECK(describe(orchestrator::advance(run, gitFileError(FileReadError::DirectoryMissing), context))
          == "persist");
    CHECK(describe(run.state) == "Finished(ended-by-you, removed, new)");

    // Without the removal asked for, the same read means someone else removed it.
    auto other = makeRun(Item::Pr, endingAt(EndStep::Status, Ended::EndedByYou));
    (void) orchestrator::advance(other, Lost {}, context);
    (void) orchestrator::advance(other, gitFileError(FileReadError::DirectoryMissing), context);
    CHECK(describe(other.state) == "Finished(worktree-gone, removed, new)");
}

TEST_CASE("a kept run re-checked on a poll stays seen", "[orchestrator][run]")
{
    auto const context = knownContext();
    auto run = makeRun(Item::Pr, kept(Ended::Completed, Seen::Opened));

    // Still dirty: kept again, with the new entries, still seen, still ended when it first ended.
    CHECK(describe(orchestrator::advance(run, RetryRemoval {}, context)) == "persist, read-git-file");
    CHECK_FALSE(orchestrator::countsAsActive(run));
    CHECK(orchestrator::isUnfinished(run));
    (void) orchestrator::advance(run, GitFileRead { .content = IntactGitFile }, context);
    (void) orchestrator::advance(run, CommandOk { MainCommonDir }, context);
    (void) orchestrator::advance(
        run, CommandOk { "H 100644 78981922613b2afb6025042ff6bd878ac1994e85 0\ta.txt\n" }, context);
    (void) orchestrator::advance(run, CommandOk { "c\trefs/heads/x\n" }, context);
    (void) orchestrator::advance(run, CommandOk { CleanLongStatus }, context);
    (void) orchestrator::advance(run, CommandOk { "?? new.txt\n" }, context);
    CHECK(describe(run.state) == "Finished(completed, kept, opened: ?? new.txt)");
    auto const* finished = std::get_if<Finished>(&run.state);
    REQUIRE(finished != nullptr);
    CHECK(finished->endedWallMs == StartedMs);

    // Cleaned up by hand: removed, still seen.
    (void) orchestrator::advance(run, RetryRemoval {}, context);
    (void) orchestrator::advance(run, gitFileError(FileReadError::DirectoryMissing), context);
    CHECK(describe(run.state) == "Finished(completed, removed, opened)");
    finished = std::get_if<Finished>(&run.state);
    REQUIRE(finished != nullptr);
    CHECK(finished->endedWallMs == StartedMs);
}

TEST_CASE("a kept run opened while it is re-checked stays seen", "[orchestrator][run]")
{
    auto const context = knownContext();
    auto run = makeRun(Item::Pr, kept(Ended::Completed));
    (void) orchestrator::advance(run, RetryRemoval {}, context);
    CHECK(describe(orchestrator::advance(run, MarkSeen {}, context)) == "persist");
    CHECK(describe(orchestrator::advance(run, MarkSeen {}, context)).empty());
    (void) orchestrator::advance(run, GitFileRead { .content = AlteredGitFile }, context);
    CHECK(describe(run.state)
          == "Finished(completed, kept, opened: .git file was changed; nothing was run there)");
}

TEST_CASE("a user ending the run waits for its tab", "[orchestrator][run]")
{
    auto const context = knownContext();
    auto run = makeRun(Item::Pr, Done {});
    run.pane = "pane-7";
    run.lastStatus = AgentStatus::Done;
    CHECK(describe(orchestrator::advance(run, EndByUser {}, context)) == "persist, close-pane");
    CHECK(run.pane == "pane-7");
    CHECK_FALSE(orchestrator::stepTag(run).has_value());
    CHECK(describe(orchestrator::advance(run, PaneGone {}, context)) == "persist, read-git-file");
    CHECK_FALSE(run.pane.has_value());
    CHECK(describe(run.state) == "Ending(verify, ended-by-you)");
}

TEST_CASE("a tab whose opening failed is gone before the worktree is touched", "[orchestrator][run]")
{
    struct Row
    {
        std::string_view name;
        RunInput input;
    };
    auto const rows = std::array {
        Row { .name = "timed out after the tab was created", .input = PaneOpenFailed { "timed out" } },
        Row { .name = "guest restarted mid-open", .input = Lost {} },
        Row { .name = "refused", .input = CommandFailed { "refused" } },
    };
    auto const context = knownContext();
    for (auto const& row: rows)
    {
        CAPTURE(row.name);
        auto run = makeRun(Item::Issue, Preparing { PrepareStep::OpenPane });
        CHECK(describe(orchestrator::advance(run, row.input, context)) == "persist, close-pane");
        // No git file read, no command: only the tab being gone moves it on.
        CHECK(
            describe(orchestrator::advance(run, GitFileRead { .content = IntactGitFile }, context)).empty());
        CHECK(describe(orchestrator::advance(run, CommandOk {}, context)).empty());
        CHECK(describe(orchestrator::advance(run, PaneGone {}, context)) == "persist, read-git-file");
        CHECK(describe(run.state) == "Ending(verify, preparation-failed)");
    }
}

TEST_CASE("an opened tab is the run's", "[orchestrator][run]")
{
    auto run = makeRun(Item::Pr, Preparing { PrepareStep::OpenPane });
    (void) orchestrator::advance(run, PaneOpened { .pane = "pane-7", .monoMs = 42 }, knownContext());
    CHECK(run.pane == "pane-7");
    CHECK(run.runningSinceMs == 42);
}

TEST_CASE("a change is stamped with the monotonic time it was made at", "[orchestrator][run]")
{
    auto context = knownContext();
    context.monoMs = 500;
    auto run = makeRun(Item::Pr, Preparing { PrepareStep::Fetch });
    REQUIRE(orchestrator::beginRun(run, context).has_value());
    CHECK(run.changedMonoMs == 500);

    // An input the state does not await changes nothing, so it stamps nothing.
    context.monoMs = 600;
    CHECK(describe(orchestrator::advance(run, PaneGone {}, context)).empty());
    CHECK(run.changedMonoMs == 500);

    context.monoMs = 700;
    CHECK_FALSE(describe(orchestrator::advance(run, CommandOk {}, context)).empty());
    CHECK(run.changedMonoMs == 700);
}

TEST_CASE("a kept run keeps at most so many status lines", "[orchestrator][run]")
{
    auto status = std::string {};
    for (auto const index: std::views::iota(0, 25))
        status += std::format("?? file{}.txt\n", index);
    auto run = makeRun(Item::Pr, endingAt(EndStep::Status));
    (void) orchestrator::advance(run, CommandOk { status }, knownContext());
    auto const* const finished = std::get_if<Finished>(&run.state);
    REQUIRE(finished != nullptr);
    REQUIRE(finished->entries.size() == orchestrator::MaxStatusEntries + 1);
    CHECK(finished->entries.front() == "?? file0.txt");
    CHECK(finished->entries[orchestrator::MaxStatusEntries - 1] == "?? file19.txt");
    CHECK(finished->entries.back() == "and 5 more");
}

TEST_CASE("a long reason is cut", "[orchestrator][run]")
{
    auto const why = std::string(500, 'x');
    auto run = makeRun(Item::Pr, Preparing { PrepareStep::WorktreeAdd });
    (void) orchestrator::advance(run, CommandFailed { why }, knownContext());
    REQUIRE(run.failure.has_value());
    CHECK(run.failure->why.size() <= orchestrator::MaxEntryChars * 3);
    CHECK(run.failure->why.ends_with("…"));
}

// {{{ no awaited step swallows its result

namespace
{

/// The states that await a host call, each with the inputs that call can come back as.
struct AwaitingState
{
    std::string_view name;
    RunState state;
    Item item;
    std::vector<RunInput> inputs;
};

[[nodiscard]] std::vector<AwaitingState> awaitingStates()
{
    auto const commandResults =
        std::vector<RunInput> { CommandOk { "" }, CommandFailed { "exit 1" }, CwdMissing {}, Lost {} };
    auto const paneResults = std::vector<RunInput> {
        PaneOpened { .pane = "pane-7", .monoMs = 1 }, PaneOpenFailed { "no" }, CommandFailed { "no" }, Lost {}
    };
    auto const fileResults = std::vector<RunInput> {
        GitFileRead { .content = IntactGitFile },      GitFileRead { .content = AlteredGitFile },
        gitFileError(FileReadError::DirectoryMissing), gitFileError(FileReadError::NotRegular),
        gitFileError(FileReadError::Unreadable),       gitFileError(FileReadError::TooLarge),
        gitFileError(FileReadError::Escapes),          Lost {},
    };
    auto states = std::vector<AwaitingState> {};
    for (auto const item: { Item::Pr, Item::Issue })
    {
        states.push_back({ "fetch", Preparing { PrepareStep::Fetch }, item, commandResults });
        states.push_back({ "worktree-add", Preparing { PrepareStep::WorktreeAdd }, item, commandResults });
        states.push_back({ "open-pane", Preparing { PrepareStep::OpenPane }, item, paneResults });
    }
    states.push_back({ "checkout", Preparing { PrepareStep::Checkout }, Item::Pr, commandResults });
    states.push_back({ "verify", endingAt(EndStep::Verify), Item::Pr, fileResults });
    for (auto const step: { EndStep::CommonDir,
                            EndStep::Reachable,
                            EndStep::OperationState,
                            EndStep::IndexFlags,
                            EndStep::Status,
                            EndStep::CleanStatusBoard,
                            EndStep::Remove })
        states.push_back({ "command", endingAt(step), Item::Pr, commandResults });
    return states;
}

} // namespace

TEST_CASE("no awaited step swallows its result", "[orchestrator][run]")
{
    for (auto const& awaiting: awaitingStates())
    {
        for (auto const& input: awaiting.inputs)
        {
            auto run = makeRun(awaiting.item, awaiting.state);
            REQUIRE(orchestrator::stepTag(run).has_value());
            auto const before = describe(run.state);
            auto const effects = orchestrator::advance(run, input, knownContext());
            CAPTURE(awaiting.name, before, input.index());
            CHECK((describe(run.state) != before || !effects.empty()));
        }
    }
}

TEST_CASE("every effect list writes its step ahead of the call it makes", "[orchestrator][run]")
{
    auto const checkWriteAhead = [](std::vector<RunEffect> const& effects) {
        CAPTURE(describe(effects));
        auto const persist = std::ranges::find_if(
            effects, [](RunEffect const& effect) { return std::holds_alternative<Persist>(effect); });
        auto const call = std::ranges::find_if(effects, [](RunEffect const& effect) {
            return std::holds_alternative<RunDeclared>(effect)
                   || std::holds_alternative<OpenAgentPane>(effect)
                   || std::holds_alternative<ReadGitFile>(effect)
                   || std::holds_alternative<CloseAgentPane>(effect);
        });
        if (call != effects.end())
            CHECK(persist < call);
        if (persist != effects.end())
            CHECK(persist == effects.begin());
    };
    for (auto const& row: transitionRows())
    {
        CAPTURE(row.from);
        auto run = runFor(row);
        checkWriteAhead(orchestrator::advance(run, row.input, contextFor(row)));
    }
    for (auto const& awaiting: awaitingStates())
        for (auto const& input: awaiting.inputs)
        {
            CAPTURE(awaiting.name, input.index());
            auto run = makeRun(awaiting.item, awaiting.state);
            checkWriteAhead(orchestrator::advance(run, input, knownContext()));
        }
    auto starting = makeRun(Item::Pr, Preparing { PrepareStep::Fetch });
    checkWriteAhead(beginEffects(starting));
}

// }}}
// {{{ declared commands

namespace
{

/// Every RunDeclared the table and the awaiting states produce, with the run that produced it.
[[nodiscard]] std::vector<RunDeclared> everyDeclaration()
{
    auto declarations = std::vector<RunDeclared> {};
    auto const collect = [&](std::vector<RunEffect> const& effects) {
        for (auto const& effect: effects)
            if (auto const* declared = std::get_if<RunDeclared>(&effect))
                declarations.push_back(*declared);
    };
    for (auto const& row: transitionRows())
    {
        auto run = runFor(row);
        collect(orchestrator::advance(run, row.input, contextFor(row)));
    }
    for (auto const& awaiting: awaitingStates())
        for (auto const& input: awaiting.inputs)
        {
            auto run = makeRun(awaiting.item, awaiting.state);
            collect(orchestrator::advance(run, input, knownContext()));
        }
    auto starting = makeRun(Item::Pr, Preparing { PrepareStep::Fetch });
    collect(beginEffects(starting));
    return declarations;
}

} // namespace

TEST_CASE("every declared command passes its declaration", "[orchestrator][run]")
{
    auto const declarations = everyDeclaration();
    auto names = std::vector<std::string_view> {};
    for (auto const& declared: declarations)
    {
        CAPTURE(declared.name);
        names.push_back(declared.name);
        auto const* const command = orchestrator::findCommand(declared.name);
        REQUIRE(command != nullptr);
        auto placeholders = std::vector<std::string_view> {};
        for (auto const word: command->argv)
            if (auto const name = orchestrator::placeholderName(word))
                placeholders.push_back(*name);
        auto argumentNames = std::vector<std::string_view> {};
        for (auto const& argument: declared.arguments)
        {
            CAPTURE(argument.name, argument.value);
            argumentNames.push_back(argument.name);
            auto const* const decl = orchestrator::findArgumentDecl(argument.name);
            REQUIRE(decl != nullptr);
            CHECK(orchestrator::argumentAccepts(decl->kind, argument.value));
        }
        std::ranges::sort(placeholders);
        std::ranges::sort(argumentNames);
        CHECK(argumentNames == placeholders);
        CHECK(orchestrator::substitute(command->argv, declared.arguments).has_value());
    }
    std::ranges::sort(names);
    auto const [first, last] = std::ranges::unique(names);
    names.erase(first, last);
    CHECK(names
          == std::vector<std::string_view> { "gh-pr-checkout",
                                             "git-clean",
                                             "git-common-dir",
                                             "git-fetch",
                                             "git-head-reachable",
                                             "git-index-flags",
                                             "git-operation-state",
                                             "git-status",
                                             "git-worktree-add",
                                             "git-worktree-add-branch",
                                             "git-worktree-remove" });
}

TEST_CASE("the values each command is passed", "[orchestrator][run]")
{
    struct Row
    {
        std::string_view name;
        Item item;
        RunState state;
        RunInput input;
        std::string_view command;
        std::vector<std::string_view> values;
    };
    auto const rows = std::vector<Row> {
        { "fetch", Item::Pr, Preparing { PrepareStep::Fetch }, Lost {}, "git-fetch", { "origin" } },
        { "worktree-add",
          Item::Pr,
          Preparing { PrepareStep::Fetch },
          CommandOk {},
          "git-worktree-add",
          { "/home/u/worktrees/contour/pr-2334-20261113-150847", "origin/master" } },
        { "worktree-add-branch",
          Item::Issue,
          Preparing { PrepareStep::Fetch },
          CommandOk {},
          "git-worktree-add-branch",
          { "agent/pr-2334-20261113-150847",
            "/home/u/worktrees/contour/pr-2334-20261113-150847",
            "origin/master" } },
        { "checkout",
          Item::Pr,
          Preparing { PrepareStep::WorktreeAdd },
          CommandOk {},
          "gh-pr-checkout",
          { "2334", "agent/pr-2334-20261113-150847" } },
        { "remove",
          Item::Pr,
          endingAt(EndStep::Status),
          CommandOk {},
          "git-worktree-remove",
          { "/home/u/worktrees/contour/pr-2334-20261113-150847" } },
    };
    for (auto const& row: rows)
    {
        CAPTURE(row.name);
        auto run = makeRun(row.item, row.state);
        // The fetch is started by beginRun(); every other command by advance().
        auto const effects = row.command == "git-fetch"
                                 ? beginEffects(run)
                                 : orchestrator::advance(run, row.input, knownContext());
        REQUIRE(effects.size() == 2);
        auto const* const declared = std::get_if<RunDeclared>(&effects[1]);
        REQUIRE(declared != nullptr);
        CHECK(declared->name == row.command);
        auto values = std::vector<std::string_view> {};
        for (auto const& argument: declared->arguments)
            values.push_back(argument.value);
        CHECK(values == row.values);
    }
}

TEST_CASE("a value its kind refuses is never passed", "[orchestrator][run]")
{
    auto const context = knownContext();

    SECTION("a remote that is no name is never dispatched")
    {
        auto run = makeRun(Item::Pr, Preparing { PrepareStep::Fetch });
        run.remote = "--upload-pack=evil";
        CHECK(orchestrator::beginRun(run, context)
              == std::unexpected { orchestrator::BeginError::NotStorable });
        CHECK(describe(run.state) == "Preparing(fetch)");
    }
    SECTION("a worktree that is no path is kept instead of removed")
    {
        auto run = makeRun(Item::Pr, endingAt(EndStep::Status));
        run.worktree = "relative/../path";
        CHECK(describe(orchestrator::advance(run, CommandOk {}, context)) == "persist");
        CHECK(describe(run.state)
              == "Finished(completed, kept, new: git worktree remove failed: git-worktree-remove refused the "
                 "value "
                 "of {path})");
    }
    SECTION("a base that is no ref fails through the removal checks")
    {
        auto run = makeRun(Item::Pr, Preparing { PrepareStep::Fetch });
        run.base = "a..b";
        CHECK(describe(orchestrator::advance(run, CommandOk {}, context)) == "persist, read-git-file");
        CHECK(describe(run.state) == "Ending(verify, preparation-failed)");
    }
    SECTION("a session that is no UUID never opens a tab")
    {
        auto run = makeRun(Item::Pr, Preparing { PrepareStep::Checkout });
        run.session = "not-a-uuid";
        // No tab was asked for, so there is none to wait for.
        CHECK(describe(orchestrator::advance(run, CommandOk {}, context)) == "persist, read-git-file");
        CHECK(describe(run.failure) == "open-pane: agent refused the value of {session}");
    }
    SECTION("beginRun leaves a started run alone")
    {
        auto run = makeRun(Item::Pr, Running {});
        CHECK(orchestrator::beginRun(run, context)
              == std::unexpected { orchestrator::BeginError::NotPreparing });
        CHECK(describe(run.state) == "Running");
    }
}

// }}}
// {{{ predicates

TEST_CASE("stepTag, countsAsActive and isUnfinished per state", "[orchestrator][run]")
{
    struct Row
    {
        RunState state;
        std::string_view tag; ///< Empty: none.
        Expect active;
        Expect unfinished;
    };
    auto const rows = std::vector<Row> {
        { Preparing { PrepareStep::Fetch }, "pr-2334-20261113-150847/fetch", Expect::Yes, Expect::Yes },
        { Preparing { PrepareStep::WorktreeAdd },
          "pr-2334-20261113-150847/worktree-add",
          Expect::Yes,
          Expect::Yes },
        { Preparing { PrepareStep::Checkout }, "pr-2334-20261113-150847/checkout", Expect::Yes, Expect::Yes },
        { Preparing { PrepareStep::OpenPane },
          "pr-2334-20261113-150847/open-pane",
          Expect::Yes,
          Expect::Yes },
        { Running {}, "", Expect::Yes, Expect::Yes },
        { Blocked {}, "", Expect::Yes, Expect::Yes },
        { Done {}, "", Expect::Yes, Expect::Yes },
        // An Ending run neither blocks its item nor holds a slot, but still has steps to take.
        { endingAt(EndStep::ClosePane), "", Expect::No, Expect::Yes },
        { endingAt(EndStep::MainCheckout), "", Expect::No, Expect::Yes },
        { endingAt(EndStep::Verify), "pr-2334-20261113-150847/verify", Expect::No, Expect::Yes },
        { endingAt(EndStep::CommonDir), "pr-2334-20261113-150847/common-dir", Expect::No, Expect::Yes },
        { endingAt(EndStep::Reachable), "pr-2334-20261113-150847/reachable", Expect::No, Expect::Yes },
        { endingAt(EndStep::OperationState),
          "pr-2334-20261113-150847/operation-state",
          Expect::No,
          Expect::Yes },
        { endingAt(EndStep::IndexFlags), "pr-2334-20261113-150847/index-flags", Expect::No, Expect::Yes },
        { endingAt(EndStep::Status), "pr-2334-20261113-150847/status", Expect::No, Expect::Yes },
        { endingAt(EndStep::CleanStatusBoard),
          "pr-2334-20261113-150847/clean-status-board",
          Expect::No,
          Expect::Yes },
        { endingAt(EndStep::Remove), "pr-2334-20261113-150847/remove", Expect::No, Expect::Yes },
        { kept(Ended::Completed), "", Expect::No, Expect::No },
        { removed(Ended::Completed), "", Expect::No, Expect::No },
    };
    for (auto const& row: rows)
    {
        auto const run = makeRun(Item::Pr, row.state);
        CAPTURE(describe(run.state));
        CHECK(orchestrator::stepTag(run).value_or("") == row.tag);
        CHECK(orchestrator::countsAsActive(run) == (row.active == Expect::Yes));
        CHECK(orchestrator::isUnfinished(run) == (row.unfinished == Expect::Yes));
    }
}

TEST_CASE("stateFor", "[orchestrator][run]")
{
    struct Row
    {
        AgentStatus status;
        std::string_view state;
    };
    constexpr auto Rows = std::array {
        Row { .status = AgentStatus::Idle, .state = "Running" },
        Row { .status = AgentStatus::Working, .state = "Running" },
        Row { .status = AgentStatus::Done, .state = "Done" },
        Row { .status = AgentStatus::Blocked, .state = "Blocked" },
        Row { .status = AgentStatus::Error, .state = "Blocked" },
    };
    for (auto const& row: Rows)
    {
        CAPTURE(row.status);
        CHECK(describe(orchestrator::stateFor(row.status)) == row.state);
    }
}

TEST_CASE("agentStatusName", "[orchestrator][run]")
{
    struct Row
    {
        AgentStatus status;
        std::string_view name;
    };
    static constexpr auto Rows = std::array {
        Row { .status = AgentStatus::Idle, .name = "idle" },
        Row { .status = AgentStatus::Working, .name = "working" },
        Row { .status = AgentStatus::Done, .name = "done" },
        Row { .status = AgentStatus::Blocked, .name = "blocked" },
        Row { .status = AgentStatus::Error, .name = "error" },
    };
    for (auto const& row: Rows)
    {
        CAPTURE(row.name);
        CHECK(orchestrator::agentStatusName(row.status) == row.name);
    }
}

TEST_CASE("gitFileIsIntact", "[orchestrator][run]")
{
    struct Row
    {
        std::string_view name;
        std::string_view content;
        Expect intact;
    };
    constexpr auto Worktree = "/w/contour/pr-1-20261113-150847"sv;
    constexpr auto Rows = std::array {
        Row { .name = "exact",
              .content = "gitdir: /r/contour/.git/worktrees/pr-1-20261113-150847",
              .intact = Expect::Yes },
        Row { .name = "line end",
              .content = "gitdir: /r/contour/.git/worktrees/pr-1-20261113-150847\n",
              .intact = Expect::Yes },
        Row { .name = "two line ends",
              .content = "gitdir: /r/contour/.git/worktrees/pr-1-20261113-150847\n\n",
              .intact = Expect::No },
        Row { .name = "carriage return",
              .content = "gitdir: /r/contour/.git/worktrees/pr-1-20261113-150847\r\n",
              .intact = Expect::No },
        Row { .name = "other main checkout",
              .content = "gitdir: /evil/contour/.git/worktrees/pr-1-20261113-150847\n",
              .intact = Expect::No },
        Row { .name = "other prefix",
              .content = "gitdir:/r/contour/.git/worktrees/pr-1-20261113-150847\n",
              .intact = Expect::No },
        Row { .name = "other name",
              .content = "gitdir: /r/contour/.git/worktrees/pr-2-20261113-150847\n",
              .intact = Expect::No },
        Row { .name = "a name git disambiguated",
              .content = "gitdir: /r/contour/.git/worktrees/pr-1-20261113-1508471\n",
              .intact = Expect::No },
        Row { .name = "extra line",
              .content = "gitdir: /r/contour/.git/worktrees/pr-1-20261113-150847\ncommondir: /evil\n",
              .intact = Expect::No },
        Row { .name = "nested",
              .content = "gitdir: /r/contour/.git/worktrees/x/pr-1-20261113-150847\n",
              .intact = Expect::No },
        Row { .name = "empty", .content = "", .intact = Expect::No },
    };
    for (auto const& row: Rows)
    {
        CAPTURE(row.name);
        CHECK(orchestrator::gitFileIsIntact(row.content, "/r/contour", Worktree)
              == (row.intact == Expect::Yes));
    }
    CHECK_FALSE(
        orchestrator::gitFileIsIntact("gitdir: /r/contour/.git/worktrees/\n", "/r/contour", "/w/contour/"));
}

TEST_CASE("commonDirIsMain", "[orchestrator][run]")
{
    struct Row
    {
        std::string_view name;
        std::string_view output;
        Expect main;
    };
    // Compared as gitFileIsIntact compares: exact, one optional line end. git prints the resolved path, as
    // `git worktree list` does for the main checkout (measured: harness/r11.sh).
    constexpr auto Rows = std::array {
        Row { .name = "exact", .output = "/r/contour/.git", .main = Expect::Yes },
        Row { .name = "line end", .output = "/r/contour/.git\n", .main = Expect::Yes },
        Row { .name = "two line ends", .output = "/r/contour/.git\n\n", .main = Expect::No },
        Row { .name = "carriage return", .output = "/r/contour/.git\r\n", .main = Expect::No },
        Row { .name = "redirected", .output = "/tmp/evil.git\n", .main = Expect::No },
        Row {
            .name = "a sibling sharing the prefix", .output = "/r/contour-evil/.git\n", .main = Expect::No },
        Row { .name = "below the .git", .output = "/r/contour/.git/worktrees/x\n", .main = Expect::No },
        Row { .name = "a trailing slash", .output = "/r/contour/.git/\n", .main = Expect::No },
        Row { .name = "relative", .output = "../../.git\n", .main = Expect::No },
        Row { .name = "the checkout itself", .output = "/r/contour\n", .main = Expect::No },
        Row { .name = "empty", .output = "", .main = Expect::No },
    };
    for (auto const& row: Rows)
    {
        CAPTURE(row.name);
        CHECK(orchestrator::commonDirIsMain(row.output, "/r/contour") == (row.main == Expect::Yes));
    }
    CHECK_FALSE(orchestrator::commonDirIsMain("/.git\n", ""));
}

// }}}
// {{{ storage

namespace
{

[[nodiscard]] std::string describeItem(ItemRef const& item)
{
    if (auto const* const pr = std::get_if<ChangeRequestRef>(&item))
        return std::format("pr {}", pr->number);
    if (auto const* const issue = std::get_if<IssueRef>(&item))
        return std::format("issue {}", issue->number);
    return std::format("jira {}", std::get<JiraRef>(item).key);
}

void checkSame(Run const& actual, Run const& expected)
{
    CHECK(actual.id == expected.id);
    CHECK(actual.repo == expected.repo);
    CHECK(actual.repoPath == expected.repoPath);
    CHECK(actual.remote == expected.remote);
    CHECK(actual.base == expected.base);
    CHECK(describeItem(actual.item) == describeItem(expected.item));
    CHECK(actual.url == expected.url);
    CHECK(actual.task == expected.task);
    CHECK(actual.model == expected.model);
    CHECK(actual.prompt == expected.prompt);
    CHECK(actual.session == expected.session);
    CHECK(actual.branch == expected.branch);
    CHECK(actual.worktree == expected.worktree);
    CHECK(actual.pane == expected.pane);
    CHECK(actual.state == expected.state);
    CHECK(actual.lastStatus == expected.lastStatus);
    CHECK(actual.failure == expected.failure);
    CHECK(actual.startedWallMs == expected.startedWallMs);
    CHECK(actual.attempt == expected.attempt);
}

} // namespace

TEST_CASE("near round-trips and an absent near reads as none", "[orchestrator][run]")
{
    auto run = makeRun(Item::Pr, Running {});
    run.pane = "p1";
    run.near = "0e1f2a3b-4c5d-4e6f-8a9b-0c1d2e3f4a5b";
    auto const json = orchestrator::toJson(run);
    CHECK(json.contains(R"("near":"0e1f2a3b-4c5d-4e6f-8a9b-0c1d2e3f4a5b")"));
    auto const read = orchestrator::runFromJson(json);
    REQUIRE(read.has_value());
    CHECK(read->near == run.near);

    // A run stored before it existed has no near, and reads as none; one with none stores none.
    run.near.reset();
    auto const without = orchestrator::toJson(run);
    CHECK_FALSE(without.contains("near"));
    auto const readWithout = orchestrator::runFromJson(without);
    REQUIRE(readWithout.has_value());
    CHECK_FALSE(readWithout->near.has_value());

    // A near that is no Name is no stored run.
    run.near = "a b";
    CHECK_FALSE(orchestrator::isStorable(run));
}

TEST_CASE("runFromJson reads what toJson writes", "[orchestrator][run]")
{
    auto resumed = endingAt(EndStep::Status, Ended::NeverReported);
    resumed.resumedFrom = kept(Ended::NeverReported, Seen::Opened);
    auto withEntries = kept(Ended::EndedByYou, Seen::Opened);
    withEntries.entries = { "?? a \"quoted\" name", "M b.txt" };
    auto const states = std::vector<RunState> {
        Preparing { PrepareStep::Fetch },
        Preparing { PrepareStep::WorktreeAdd },
        Preparing { PrepareStep::Checkout },
        Preparing { PrepareStep::OpenPane },
        Running {},
        Blocked {},
        Done {},
        endingAt(EndStep::ClosePane, Ended::EndedByYou),
        endingAt(EndStep::MainCheckout),
        endingAt(EndStep::Verify),
        endingAt(EndStep::CommonDir),
        endingAt(EndStep::Reachable, Ended::PreparationFailed),
        endingAt(EndStep::OperationState, Ended::NeverReported),
        endingAt(EndStep::IndexFlags),
        endingAt(EndStep::Status, Ended::WorktreeGone),
        endingAt(EndStep::CleanStatusBoard),
        endingAt(EndStep::Remove),
        endingAt(EndStep::Verify, Ended::EndedByYou),
        resumed,
        removed(Ended::Completed),
        removed(Ended::WorktreeGone),
        withEntries,
    };
    for (auto const& state: states)
        for (auto const item: { Item::Pr, Item::Issue, Item::Jira })
            for (auto const variant: std::views::iota(0, 4))
            {
                auto run = makeRun(item, state);
                if (variant & 1)
                    run.failure = RunFailure { .step = PrepareStep::Checkout, .why = "exit 1: \"no\"" };
                if (variant & 2)
                {
                    run.lastStatus = AgentStatus::Blocked;
                    run.pane = "pane-7";
                }
                run.runningSinceMs = 77;
                run.changedMonoMs = 79;
                auto const json = orchestrator::toJson(run);
                CAPTURE(json);
                auto const read = orchestrator::runFromJson(json);
                REQUIRE(read.has_value());
                checkSame(*read, run);
                // The monotonic fields mean nothing to another process.
                CHECK(read->runningSinceMs == 0);
                CHECK(read->changedMonoMs == 0);
                CHECK_FALSE(json.contains("runningSinceMs"));
                CHECK(orchestrator::toJson(*read) == json);
            }
}

TEST_CASE("runFromJson refuses what toJson never writes", "[orchestrator][run]")
{
    auto const good = orchestrator::toJson(makeRun(Item::Pr, Preparing { PrepareStep::Fetch }));
    auto const replaced = [&](std::string_view from, std::string_view to) {
        auto text = good;
        auto const at = text.find(from);
        REQUIRE(at != std::string::npos);
        text.replace(at, from.size(), to);
        return text;
    };
    REQUIRE(orchestrator::runFromJson(good).has_value());
    struct Row
    {
        std::string_view name;
        std::string text;
    };
    auto const rows = std::vector<Row> {
        { "empty", "" },
        { "garbage", "not json" },
        { "an array", "[]" },
        { "cut off", good.substr(0, good.size() / 2) },
        { "trailing garbage", good + "x" },
        { "version 2", replaced(R"("v":3)", R"("v":2)") },
        { "no version", replaced(R"("v":3,)", "") },
        { "an unknown key", replaced(R"("v":3,)", R"("v":3,"colour":"red",)") },
        { "a board count, which version 3 dropped", replaced(R"("v":3,)", R"("v":3,"boardPosts":0,)") },
        { "an unknown state", replaced(R"("state":"preparing")", R"("state":"flying")") },
        { "an unknown step", replaced(R"("step":"fetch")", R"("step":"nap")") },
        { "an end step while preparing", replaced(R"("step":"fetch")", R"("step":"verify")") },
        { "no step while preparing", replaced(R"(,"step":"fetch")", "") },
        { "an unknown forge", replaced(R"("forge":"github")", R"("forge":"svn")") },
        { "no forge", replaced(R"("forge":"github",)", "") },
        { "an unknown item", replaced(R"("itemKind":"pr")", R"("itemKind":"mr")") },
        { "a pr without a number", replaced(R"("itemNumber":2334,)", "") },
        { "a session that is no uuid", replaced("123e4567-e89b-12d3-a456-426614174000", "123") },
        { "a worktree that is no path", replaced("/home/u/worktrees/contour/", "home/u/worktrees/contour/") },
        { "a branch that is no ref", replaced("agent/pr-2334", "agent/../pr-2334") },
        { "a model that is no name", replaced(R"("model":"opus")", R"("model":"-opus")") },
    };
    for (auto const& row: rows)
    {
        CAPTURE(row.name, row.text);
        CHECK_FALSE(orchestrator::runFromJson(row.text).has_value());
    }
}

TEST_CASE("runFromJson refuses an inconsistent ending or finish", "[orchestrator][run]")
{
    auto withStatus = makeRun(Item::Pr, Running {});
    withStatus.lastStatus = AgentStatus::Working;
    auto const running = orchestrator::toJson(withStatus);
    auto const ending = orchestrator::toJson(makeRun(Item::Pr, endingAt(EndStep::Verify)));
    auto const finished = orchestrator::toJson(makeRun(Item::Pr, removed(Ended::Completed)));
    auto const replaced = [](std::string text, std::string_view from, std::string_view to) {
        auto const at = text.find(from);
        REQUIRE(at != std::string::npos);
        text.replace(at, from.size(), to);
        return text;
    };
    REQUIRE(orchestrator::runFromJson(running).has_value());
    REQUIRE(orchestrator::runFromJson(ending).has_value());
    REQUIRE(orchestrator::runFromJson(finished).has_value());
    struct Row
    {
        std::string_view name;
        std::string text;
    };
    auto const rows = std::vector<Row> {
        { "an unknown status", replaced(running, R"("lastStatus":"working")", R"("lastStatus":"sleepy")") },
        { "a step while running",
          replaced(running, R"("state":"running")", R"("state":"running","step":"fetch")") },
        { "an ending without a cause", replaced(ending, R"(,"cause":"completed")", "") },
        { "an unknown cause", replaced(ending, R"("cause":"completed")", R"("cause":"bored")") },
        { "an unknown removal", replaced(ending, R"("removal":"not-issued")", R"("removal":"maybe")") },
        { "an ending without its removal", replaced(ending, R"(,"removal":"not-issued")", "") },
        { "a removal while running",
          replaced(running, R"("state":"running")", R"("state":"running","removal":"issued")") },
        { "a finish without its fields", replaced(finished, R"("state":"finished")", R"("state":"done")") },
        { "an unknown fate", replaced(finished, R"("fate":"removed")", R"("fate":"lost")") },
        { "an unknown seen", replaced(finished, R"("seen":"new")", R"("seen":"maybe")") },
    };
    for (auto const& row: rows)
    {
        CAPTURE(row.name, row.text);
        CHECK_FALSE(orchestrator::runFromJson(row.text).has_value());
    }
}

// }}}

TEST_CASE("every state a step leaves behind is stored and read back as it was", "[orchestrator][run]")
{
    // A state that saves but cannot load would be lost on the next restart.
    auto const checkStored = [](Run const& run) {
        CAPTURE(describe(run.state));
        CHECK(orchestrator::isStorable(run));
        auto const json = orchestrator::toJson(run);
        auto const read = orchestrator::runFromJson(json);
        REQUIRE(read.has_value());
        auto expected = run;
        expected.runningSinceMs = 0;
        checkSame(*read, expected);
        CHECK(orchestrator::toJson(*read) == json);
    };
    for (auto const& row: transitionRows())
    {
        CAPTURE(row.from);
        auto run = runFor(row);
        (void) orchestrator::advance(run, row.input, contextFor(row));
        checkStored(run);
    }
    for (auto const& awaiting: awaitingStates())
        for (auto const& input: awaiting.inputs)
        {
            CAPTURE(awaiting.name, input.index());
            auto run = makeRun(awaiting.item, awaiting.state);
            (void) orchestrator::advance(run, input, knownContext());
            checkStored(run);
        }
    for (auto const item: { Item::Pr, Item::Issue, Item::Jira })
    {
        auto run = makeRun(item, Preparing { PrepareStep::Fetch });
        REQUIRE(orchestrator::beginRun(run, knownContext()).has_value());
        checkStored(run);
    }
}

// {{{ the store's bound

namespace
{

/// @return @p count copies of @p piece.
[[nodiscard]] std::string repeated(std::string_view piece, std::size_t count)
{
    auto text = std::string {};
    for ([[maybe_unused]] auto const index: std::views::iota(std::size_t { 0 }, count))
        text += piece;
    return text;
}

/// A path of @p bytes bytes whose components are quotes, the character JSON escapes longest among those a
/// stored path may hold.
[[nodiscard]] std::string worstPath(std::size_t bytes)
{
    auto path = std::string {};
    while (path.size() + 2 <= bytes)
        path += path.size() + 64 < bytes ? std::string("/") + repeated("\"", 62) : "/\"";
    return path;
}

/// The largest run isStorable() accepts, each field filled with what escapes longest.
[[nodiscard]] Run worstCaseRun(RunState state)
{
    auto run = makeRun(Item::Jira, std::move(state));
    auto const name = repeated("n", orchestrator::MaxNameBytes);
    run.id = name;
    run.repo = name;
    run.remote = name;
    run.task = name;
    run.model = name;
    run.pane = name;
    run.base = repeated("r", orchestrator::MaxRefBytes);
    run.branch = repeated("b", orchestrator::MaxRefBytes);
    run.item = JiraRef { .key = "A" + repeated("B", orchestrator::MaxJiraKeyBytes - 4) + "-1" };
    run.repoPath = worstPath(orchestrator::MaxPathBytes);
    run.worktree = worstPath(orchestrator::MaxPathBytes);
    run.url = "https://h/" + repeated("\\", orchestrator::MaxUrlBytes - 10);
    run.prompt = "P" + repeated("\"", orchestrator::MaxTextBytes - 1);
    // Four-byte characters: the most bytes an entry can hold, each written unescaped.
    auto const entry = repeated("\xf0\x9f\x98\x80", orchestrator::MaxEntryChars);
    run.failure = RunFailure { .step = PrepareStep::OpenPane, .why = entry };
    run.lastStatus = AgentStatus::Error;
    run.startedWallMs = UINT64_MAX;
    auto const entries = std::vector<std::string>(orchestrator::MaxStatusEntries + 1, entry);
    if (auto* const finished = std::get_if<Finished>(&run.state))
        finished->entries = entries;
    if (auto* const ending = std::get_if<Ending>(&run.state); ending != nullptr && ending->resumedFrom)
        ending->resumedFrom->entries = entries;
    return run;
}

} // namespace

TEST_CASE("a storable run stays within the store's limit", "[orchestrator][run]")
{
    auto resumed = endingAt(EndStep::Remove, Ended::PreparationFailed);
    resumed.resumedFrom = kept(Ended::PreparationFailed);
    auto finished = kept(Ended::PreparationFailed);
    finished.endedWallMs = UINT64_MAX;
    for (auto const& state: std::vector<RunState> { resumed, finished })
    {
        auto const run = worstCaseRun(state);
        CAPTURE(describe(run.state));
        REQUIRE(orchestrator::isStorable(run));
        auto const json = orchestrator::toJson(run);
        CAPTURE(json.size());
        CHECK(json.size() <= orchestrator::MaxStoredRunBytes);
        auto const read = orchestrator::runFromJson(json);
        REQUIRE(read.has_value());
        CHECK(orchestrator::toJson(*read) == json);
    }
}

TEST_CASE("isStorable refuses what could outgrow the store", "[orchestrator][run]")
{
    struct Row
    {
        std::string_view name;
        void (*change)(Run&);
    };
    constexpr auto Rows = std::array {
        Row { .name = "a control character in the prompt",
              .change = [](Run& run) { run.prompt += "\x1b[2J"; } },
        Row { .name = "a carriage return in the prompt", .change = [](Run& run) { run.prompt += "\r"; } },
        Row { .name = "a prompt beyond Text",
              .change = [](Run& run) { run.prompt.resize(orchestrator::MaxTextBytes + 1, 'x'); } },
        Row { .name = "a long url",
              .change = [](Run& run) { run.url.resize(orchestrator::MaxUrlBytes + 1, 'u'); } },
        Row { .name = "a quote in the url", .change = [](Run& run) { run.url += "\""; } },
        Row { .name = "a control character in a path", .change = [](Run& run) { run.worktree += "\x01"; } },
        Row { .name = "a tab that is no name", .change = [](Run& run) { run.pane = "pane 7"; } },
        Row { .name = "a long reason",
              .change =
                  [](Run& run) {
                      run.failure = RunFailure { .step = PrepareStep::Fetch,
                                                 .why = std::string(orchestrator::MaxEntryChars + 1, 'w') };
                  } },
        Row { .name = "a reason with a line end",
              .change =
                  [](Run& run) { run.failure = RunFailure { .step = PrepareStep::Fetch, .why = "a\nb" }; } },
        Row { .name = "too many entries",
              .change =
                  [](Run& run) {
                      run.state =
                          Finished { .ended = Ended::Completed,
                                     .fate = Fate::Kept,
                                     .entries =
                                         std::vector<std::string>(orchestrator::MaxStatusEntries + 2, "e"),
                                     .seen = Seen::New,
                                     .endedWallMs = 0 };
                  } },
        Row { .name = "a Jira key that is none",
              .change = [](Run& run) { run.item = JiraRef { .key = "ct-1" }; } },
    };
    REQUIRE(orchestrator::isStorable(makeRun(Item::Pr, Running {})));
    for (auto const& row: Rows)
    {
        CAPTURE(row.name);
        auto run = makeRun(Item::Pr, Running {});
        row.change(run);
        CHECK_FALSE(orchestrator::isStorable(run));
        CHECK_FALSE(orchestrator::runFromJson(orchestrator::toJson(run)).has_value());
    }
}

TEST_CASE("a tab id that is no name fails the opening", "[orchestrator][run]")
{
    auto run = makeRun(Item::Pr, Preparing { PrepareStep::OpenPane });
    CHECK(
        describe(orchestrator::advance(run, PaneOpened { .pane = "pane 7\x1b", .monoMs = 1 }, knownContext()))
        == "persist, close-pane");
    CHECK_FALSE(run.pane.has_value());
    CHECK(describe(run.failure) == "open-pane: the tab's id is not a name");
}

TEST_CASE("preparation runs what the provider row of the run's forge names", "[orchestrator][run]")
{
    /// Where the run starts from.
    enum class From : uint8_t
    {
        Dispatch,  ///< beginRun().
        FetchDone, ///< Its fetch exited 0.
    };
    struct Row
    {
        std::string_view name;
        orchestrator::Forge forge;
        From from;
        std::string_view effects;
        std::string_view state;
        std::string_view failure;
    };
    static constexpr auto Rows = std::array {
        Row { .name = "a forge with a row fetches",
              .forge = orchestrator::Forge::GitHub,
              .from = From::Dispatch,
              .effects = "persist, run git-fetch remote",
              .state = "Preparing(fetch)",
              .failure = "" },
        Row { .name = "a forge with a row adds the worktree",
              .forge = orchestrator::Forge::GitHub,
              .from = From::FetchDone,
              .effects = "persist, run git-worktree-add path base",
              .state = "Preparing(worktree-add)",
              .failure = "" },
        Row { .name = "a forge without a row runs nothing",
              .forge = orchestrator::Forge::GitLab,
              .from = From::Dispatch,
              .effects = "persist",
              .state = "Finished(preparation-failed, removed, new)",
              .failure = "fetch: no provider stands for the repository's forge" },
        Row { .name = "a forge without a row checks what the fetch left",
              .forge = orchestrator::Forge::GitLab,
              .from = From::FetchDone,
              .effects = "persist, read-git-file",
              .state = "Ending(verify, preparation-failed)",
              .failure = "worktree-add: no provider stands for the repository's forge" },
    };
    for (auto const& row: Rows)
    {
        CAPTURE(row.name);
        auto run = makeRun(Item::Pr, Preparing { PrepareStep::Fetch });
        run.forge = row.forge;
        auto const effects = row.from == From::Dispatch
                                 ? beginEffects(run)
                                 : orchestrator::advance(run, CommandOk { .stdoutText = "" }, knownContext());
        CHECK(describe(effects) == row.effects);
        CHECK(describe(run.state) == row.state);
        CHECK(describe(run.failure) == row.failure);
    }
}

// }}}
