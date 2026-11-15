// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <expected>
#include <optional>
#include <string_view>

#include <orchestrator/core/OperationState.hpp>

namespace
{

using orchestrator::Operation;
using orchestrator::OperationStateError;

using Reading = std::expected<std::optional<Operation>, OperationStateError>;

constexpr auto Unrecognised = Reading { std::unexpect, OperationStateError::Unrecognised };

struct OperationRow
{
    std::string_view state; ///< The state's name in harness/r11.txt.
    std::string_view stdoutText;
    Reading expected;
};

// The standard output of `git -c status.displayCommentPrefix=false -c color.status=false status --long`
// (LC_ALL=C, git 2.56) in every state harness/r11.sh records, copied from harness/r11.txt; hashes are
// elided there as they are here.
constexpr auto RecordedRows = std::array {
    OperationRow { .state = "none",
                   .stdoutText = "On branch agent/x\n"
                                 "nothing to commit, working tree clean\n",
                   .expected = std::nullopt },
    OperationRow { .state = "rebase-conflict",
                   .stdoutText = "interactive rebase in progress; onto <hash>\n"
                                 "Last command done (1 command done):\n"
                                 "   pick <hash> # w1\n"
                                 "Next command to do (1 remaining command):\n"
                                 "   pick <hash> # w2\n"
                                 "  (use \"git rebase --edit-todo\" to view and edit)\n"
                                 "You are currently rebasing branch 'agent/x' on '<hash>'.\n"
                                 "  (fix conflicts and then run \"git rebase --continue\")\n"
                                 "  (use \"git rebase --skip\" to skip this patch)\n"
                                 "  (use \"git rebase --abort\" to check out the original branch)\n"
                                 "\n"
                                 "Unmerged paths:\n"
                                 "  (use \"git restore --staged <file>...\" to unstage)\n"
                                 "  (use \"git add <file>...\" to mark resolution)\n"
                                 "\tboth modified:   a.txt\n"
                                 "\n"
                                 "no changes added to commit (use \"git add\" and/or \"git commit -a\")\n",
                   .expected = Operation::Rebase },
    OperationRow { .state = "rebase-apply-conflict",
                   .stdoutText = "rebase in progress; onto <hash>\n"
                                 "You are currently rebasing branch 'agent/x' on '<hash>'.\n"
                                 "  (fix conflicts and then run \"git rebase --continue\")\n"
                                 "  (use \"git rebase --skip\" to skip this patch)\n"
                                 "  (use \"git rebase --abort\" to check out the original branch)\n"
                                 "\n"
                                 "Unmerged paths:\n"
                                 "  (use \"git restore --staged <file>...\" to unstage)\n"
                                 "  (use \"git add <file>...\" to mark resolution)\n"
                                 "\tboth modified:   a.txt\n"
                                 "\n"
                                 "no changes added to commit (use \"git add\" and/or \"git commit -a\")\n",
                   .expected = Operation::Rebase },
    // `git status --porcelain` is empty here and HEAD lies in a ref: only this check keeps the worktree.
    OperationRow { .state = "rebase-i-break",
                   .stdoutText =
                       "interactive rebase in progress; onto <hash>\n"
                       "Last command done (1 command done):\n"
                       "   break\n"
                       "Next commands to do (2 remaining commands):\n"
                       "   pick <hash> # w1\n"
                       "   pick <hash> # w2\n"
                       "  (use \"git rebase --edit-todo\" to view and edit)\n"
                       "You are currently editing a commit while rebasing branch 'agent/x' on '<hash>'.\n"
                       "  (use \"git commit --amend\" to amend the current commit)\n"
                       "  (use \"git rebase --continue\" once you are satisfied with your changes)\n"
                       "\n"
                       "nothing to commit, working tree clean\n",
                   .expected = Operation::Rebase },
    OperationRow { .state = "merge-conflict",
                   .stdoutText = "On branch agent/x\n"
                                 "You have unmerged paths.\n"
                                 "  (fix conflicts and run \"git commit\")\n"
                                 "  (use \"git merge --abort\" to abort the merge)\n"
                                 "\n"
                                 "Unmerged paths:\n"
                                 "  (use \"git add <file>...\" to mark resolution)\n"
                                 "\tboth modified:   a.txt\n"
                                 "\n"
                                 "no changes added to commit (use \"git add\" and/or \"git commit -a\")\n",
                   .expected = Operation::Merge },
    OperationRow { .state = "merge-no-commit",
                   .stdoutText = "On branch agent/x\n"
                                 "All conflicts fixed but you are still merging.\n"
                                 "  (use \"git commit\" to conclude merge)\n"
                                 "\n"
                                 "Changes to be committed:\n"
                                 "\tnew file:   s.txt\n",
                   .expected = Operation::Merge },
    OperationRow { .state = "cherry-pick-conflict",
                   .stdoutText = "On branch agent/x\n"
                                 "You are currently cherry-picking commit <hash>.\n"
                                 "  (fix conflicts and run \"git cherry-pick --continue\")\n"
                                 "  (use \"git cherry-pick --skip\" to skip this patch)\n"
                                 "  (use \"git cherry-pick --abort\" to cancel the cherry-pick operation)\n"
                                 "\n"
                                 "Unmerged paths:\n"
                                 "  (use \"git add <file>...\" to mark resolution)\n"
                                 "\tboth modified:   a.txt\n"
                                 "\n"
                                 "no changes added to commit (use \"git add\" and/or \"git commit -a\")\n",
                   .expected = Operation::CherryPick },
    OperationRow { .state = "cherry-pick-sequence-resolved",
                   .stdoutText = "On branch agent/x\n"
                                 "Cherry-pick currently in progress.\n"
                                 "  (run \"git cherry-pick --continue\" to continue)\n"
                                 "  (use \"git cherry-pick --skip\" to skip this patch)\n"
                                 "  (use \"git cherry-pick --abort\" to cancel the cherry-pick operation)\n"
                                 "\n"
                                 "nothing to commit, working tree clean\n",
                   .expected = Operation::CherryPick },
    OperationRow { .state = "revert-conflict",
                   .stdoutText = "On branch agent/x\n"
                                 "You are currently reverting commit <hash>.\n"
                                 "  (fix conflicts and run \"git revert --continue\")\n"
                                 "  (use \"git revert --skip\" to skip this patch)\n"
                                 "  (use \"git revert --abort\" to cancel the revert operation)\n"
                                 "\n"
                                 "Unmerged paths:\n"
                                 "  (use \"git restore --staged <file>...\" to unstage)\n"
                                 "  (use \"git add <file>...\" to mark resolution)\n"
                                 "\tboth modified:   a.txt\n"
                                 "\n"
                                 "no changes added to commit (use \"git add\" and/or \"git commit -a\")\n",
                   .expected = Operation::Revert },
    OperationRow { .state = "bisect-started",
                   .stdoutText = "On branch agent/x\n"
                                 "You are currently bisecting, started from branch 'agent/x'.\n"
                                 "  (use \"git bisect reset\" to get back to the original branch)\n"
                                 "\n"
                                 "nothing to commit, working tree clean\n",
                   .expected = Operation::Bisect },
    OperationRow { .state = "bisect-running",
                   .stdoutText = "HEAD detached at <hash>\n"
                                 "You are currently bisecting, started from branch 'agent/x'.\n"
                                 "  (use \"git bisect reset\" to get back to the original branch)\n"
                                 "\n"
                                 "nothing to commit, working tree clean\n",
                   .expected = Operation::Bisect },
    OperationRow { .state = "am-conflict",
                   .stdoutText = "On branch agent/x\n"
                                 "You are in the middle of an am session.\n"
                                 "  (fix conflicts and then run \"git am --continue\")\n"
                                 "  (use \"git am --skip\" to skip this patch)\n"
                                 "  (use \"git am --abort\" to restore the original branch)\n"
                                 "\n"
                                 "nothing to commit, working tree clean\n",
                   .expected = Operation::Am },
    // No state line, but unmerged entries: the `git status --porcelain` step keeps these worktrees.
    OperationRow { .state = "stash-pop-conflict",
                   .stdoutText = "On branch agent/x\n"
                                 "Unmerged paths:\n"
                                 "  (use \"git restore --staged <file>...\" to unstage)\n"
                                 "  (use \"git add <file>...\" to mark resolution)\n"
                                 "\tboth modified:   a.txt\n"
                                 "\n"
                                 "no changes added to commit (use \"git add\" and/or \"git commit -a\")\n",
                   .expected = std::nullopt },
    OperationRow { .state = "checkout-m-conflict",
                   .stdoutText = "On branch other\n"
                                 "Unmerged paths:\n"
                                 "  (use \"git restore --staged <file>...\" to unstage)\n"
                                 "  (use \"git add <file>...\" to mark resolution)\n"
                                 "\tboth modified:   a.txt\n"
                                 "\n"
                                 "no changes added to commit (use \"git add\" and/or \"git commit -a\")\n",
                   .expected = std::nullopt },
};

// Only a line starting in column 0 counts: a file name is indented. The first line must have a shape git
// is known to print there; anything else, such as another git's wording or a comment prefix, is
// unrecognised and keeps the worktree.
constexpr auto StrictnessRows = std::array {
    OperationRow { .state = "empty", .stdoutText = "", .expected = Unrecognised },
    OperationRow { .state = "a reworded branch line",
                   .stdoutText = "Currently on branch agent/x\nYou are currently bisecting\n",
                   .expected = Unrecognised },
    OperationRow { .state = "a translated branch line",
                   .stdoutText =
                       "Auf Branch agent/x\nnichts zu committen, Arbeitsverzeichnis unver\xc3\xa4ndert\n",
                   .expected = Unrecognised },
    OperationRow { .state = "an unknown header above the branch line",
                   .stdoutText = "sequencer in progress\nOn branch agent/x\nnothing to commit\n",
                   .expected = Unrecognised },
    OperationRow { .state = "a state line first",
                   .stdoutText = "You are currently bisecting",
                   .expected = Unrecognised },
    OperationRow { .state = "comment prefix",
                   .stdoutText =
                       "# On branch agent/x\n# You are currently bisecting, started from branch 'x'.\n",
                   .expected = Unrecognised },
    OperationRow { .state = "indented first line",
                   .stdoutText = " On branch agent/x\nnothing to commit, working tree clean\n",
                   .expected = Unrecognised },
    OperationRow {
        .state = "the branch line alone", .stdoutText = "On branch agent/x", .expected = std::nullopt },
    OperationRow { .state = "detached at",
                   .stdoutText = "HEAD detached at 1234567\nnothing to commit, working tree clean\n",
                   .expected = std::nullopt },
    OperationRow { .state = "detached from",
                   .stdoutText =
                       "HEAD detached from 1234567\nYou are currently bisecting, started from branch 'x'.\n",
                   .expected = Operation::Bisect },
    OperationRow { .state = "no branch",
                   .stdoutText = "Not currently on any branch.\nYou are in the middle of an am session.\n",
                   .expected = Operation::Am },
    OperationRow { .state = "indented",
                   .stdoutText = "On branch agent/x\n\tnew file:   You are currently bisecting\n",
                   .expected = std::nullopt },
    OperationRow { .state = "space-indented",
                   .stdoutText = "On branch agent/x\n You are currently rebasing branch 'x'.\n",
                   .expected = std::nullopt },
    OperationRow { .state = "first of two",
                   .stdoutText =
                       "On branch agent/x\nYou are currently reverting commit <hash>.\nYou are currently "
                       "bisecting\n",
                   .expected = Operation::Revert },
    OperationRow { .state = "splitting",
                   .stdoutText =
                       "interactive rebase in progress; onto 1234567\nYou are currently splitting a commit "
                       "while rebasing branch 'x' on 'y'.\n",
                   .expected = Operation::Rebase },
    OperationRow { .state = "revert sequence",
                   .stdoutText = "On branch agent/x\nRevert currently in progress.\n",
                   .expected = Operation::Revert },
    OperationRow { .state = "prefix cut short",
                   .stdoutText = "On branch x\nYou are currently\n",
                   .expected = std::nullopt },
};

struct NameRow
{
    Operation operation;
    std::string_view name;
};

constexpr auto NameRows = std::array {
    NameRow { .operation = Operation::Rebase, .name = "rebase" },
    NameRow { .operation = Operation::Merge, .name = "merge" },
    NameRow { .operation = Operation::CherryPick, .name = "cherry-pick" },
    NameRow { .operation = Operation::Revert, .name = "revert" },
    NameRow { .operation = Operation::Bisect, .name = "bisect" },
    NameRow { .operation = Operation::Am, .name = "am" },
};

} // namespace

TEST_CASE("operationInProgress reads every recorded state", "[orchestrator][run]")
{
    for (auto const& row: RecordedRows)
    {
        CAPTURE(row.state);
        CHECK(orchestrator::operationInProgress(row.stdoutText) == row.expected);
    }
}

TEST_CASE("operationInProgress reads column 0 only", "[orchestrator][run]")
{
    for (auto const& row: StrictnessRows)
    {
        CAPTURE(row.state, row.stdoutText);
        CHECK(orchestrator::operationInProgress(row.stdoutText) == row.expected);
    }
}

TEST_CASE("operationName", "[orchestrator][run]")
{
    for (auto const& row: NameRows)
    {
        CAPTURE(row.name);
        CHECK(orchestrator::operationName(row.operation) == row.name);
    }
}
