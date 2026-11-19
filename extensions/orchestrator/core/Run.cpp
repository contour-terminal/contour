// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/core/Run.hpp>

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <glaze/glaze.hpp>
#include <orchestrator/core/ArgumentKinds.hpp>
#include <orchestrator/core/Commands.hpp>
#include <orchestrator/core/Json.hpp>
#include <orchestrator/core/Model.hpp>
#include <orchestrator/core/Naming.hpp>
#include <orchestrator/core/OperationState.hpp>
#include <orchestrator/core/Overloaded.hpp>
#include <orchestrator/core/Provider.hpp>
#include <orchestrator/core/RunJson.hpp>
#include <orchestrator/core/Sanitize.hpp>

using namespace std::string_view_literals;

namespace orchestrator
{

namespace
{
    template <typename T, typename... Ts>
    concept OneOf = (std::same_as<std::remove_cvref_t<T>, Ts> || ...);

    /// The states an agent's reports move between.
    template <typename T>
    concept AgentState = OneOf<T, Running, Blocked, Done>;

    using Effects = std::vector<RunEffect>;

    // {{{ names

    /// One row of a table that names an enumerator: in tags, in entries and in the store.
    template <typename E>
    struct Spelling
    {
        E value;
        std::string_view text;
    };

    template <typename E, std::size_t N>
    [[nodiscard]] constexpr std::string_view spell(std::array<Spelling<E>, N> const& table, E value) noexcept
    {
        auto const row = std::ranges::find(table, value, &Spelling<E>::value);
        return row != table.end() ? row->text : std::string_view {};
    }

    template <typename E, std::size_t N>
    [[nodiscard]] constexpr std::optional<E> parse(std::array<Spelling<E>, N> const& table,
                                                   std::string_view text) noexcept
    {
        auto const row = std::ranges::find(table, text, &Spelling<E>::text);
        return row != table.end() ? std::optional { row->value } : std::nullopt;
    }

    constexpr auto PrepareStepNames = std::array {
        Spelling { PrepareStep::Fetch, "fetch"sv },
        Spelling { PrepareStep::WorktreeAdd, "worktree-add"sv },
        Spelling { PrepareStep::Checkout, "checkout"sv },
        Spelling { PrepareStep::OpenPane, "open-pane"sv },
    };

    constexpr auto AgentStatusNames = std::array {
        Spelling { AgentStatus::Idle, "idle"sv },   Spelling { AgentStatus::Working, "working"sv },
        Spelling { AgentStatus::Done, "done"sv },   Spelling { AgentStatus::Blocked, "blocked"sv },
        Spelling { AgentStatus::Error, "error"sv },
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

    constexpr auto RemovalNames = std::array {
        Spelling { Removal::NotIssued, "not-issued"sv },
        Spelling { Removal::Issued, "issued"sv },
    };

    constexpr auto SeenNames = std::array {
        Spelling { Seen::New, "new"sv },
        Spelling { Seen::Opened, "opened"sv },
    };

    /// The store's names of RunState's alternatives, in their order.
    constexpr auto StateNames = std::array<std::string_view, std::variant_size_v<RunState>> {
        "preparing"sv, "running"sv, "blocked"sv, "done"sv, "ending"sv, "finished"sv,
    };

    /// What a FileReadError that keeps the worktree is called in its entry.
    constexpr auto FileReadReasons = std::array {
        Spelling { FileReadError::DirectoryMissing, "its directory is gone"sv },
        Spelling { FileReadError::NotRegular, "it is not a regular file"sv },
        Spelling { FileReadError::Unreadable, "it cannot be read"sv },
        Spelling { FileReadError::TooLarge, "it is too large"sv },
        Spelling { FileReadError::Escapes, "it leads outside the worktree"sv },
    };

    // }}}
    // {{{ steps

    /// What an ending step awaits. Only a host call has a tag.
    enum class Awaits : uint8_t
    {
        PaneGone,     ///< The agent's tab to be gone: no host call.
        MainCheckout, ///< The repository's main checkout to be known: no host call.
        GitFile,      ///< The `.git` file read.
        Command,      ///< A declared command's result.
    };

    struct EndStepRow
    {
        EndStep step;
        std::string_view name;  ///< In tags and in the store.
        std::string_view label; ///< In a kept run's entry.
        Awaits awaits;
    };

    constexpr auto EndSteps = std::array {
        EndStepRow { .step = EndStep::ClosePane,
                     .name = "close-pane"sv,
                     .label = "closing the agent's tab"sv,
                     .awaits = Awaits::PaneGone },
        EndStepRow { .step = EndStep::MainCheckout,
                     .name = "main-checkout"sv,
                     .label = "waiting for the main checkout"sv,
                     .awaits = Awaits::MainCheckout },
        EndStepRow { .step = EndStep::Verify,
                     .name = "verify"sv,
                     .label = "reading .git"sv,
                     .awaits = Awaits::GitFile },
        EndStepRow { .step = EndStep::CommonDir,
                     .name = "common-dir"sv,
                     .label = "the common-directory check"sv,
                     .awaits = Awaits::Command },
        EndStepRow { .step = EndStep::IndexFlags,
                     .name = "index-flags"sv,
                     .label = "the index check"sv,
                     .awaits = Awaits::Command },
        EndStepRow { .step = EndStep::Reachable,
                     .name = "reachable"sv,
                     .label = "the reachability check"sv,
                     .awaits = Awaits::Command },
        EndStepRow { .step = EndStep::OperationState,
                     .name = "operation-state"sv,
                     .label = "the operation-state check"sv,
                     .awaits = Awaits::Command },
        EndStepRow {
            .step = EndStep::Status, .name = "status"sv, .label = "git status"sv, .awaits = Awaits::Command },
        EndStepRow { .step = EndStep::CleanStatusBoard,
                     .name = "clean-status-board"sv,
                     .label = "removing STATUS.md"sv,
                     .awaits = Awaits::Command },
        EndStepRow { .step = EndStep::Remove,
                     .name = "remove"sv,
                     .label = "git worktree remove"sv,
                     .awaits = Awaits::Command },
    };

    static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, EndSteps.size()),
                                      [](std::size_t index) {
                                          return static_cast<std::size_t>(EndSteps[index].step) == index;
                                      }),
                  "EndSteps lists every EndStep, in order");

    [[nodiscard]] constexpr EndStepRow const& endStep(EndStep step) noexcept
    {
        return EndSteps[static_cast<std::size_t>(step)];
    }

    [[nodiscard]] constexpr std::optional<EndStep> parseEndStep(std::string_view name) noexcept
    {
        auto const row = std::ranges::find(EndSteps, name, &EndStepRow::name);
        return row != EndSteps.end() ? std::optional { row->step } : std::nullopt;
    }

    // }}}
    // {{{ the texts a run keeps

    constexpr auto LostWhy = "the step was lost"sv;
    constexpr auto CwdMissingWhy = "its directory is gone"sv;
    constexpr auto AlteredGitFile = ".git file was changed; nothing was run there"sv;
    constexpr auto UnknownMainCheckout = "the main checkout is not known; nothing was run there"sv;
    constexpr auto RedirectedCommonDir =
        "git uses another directory than the main checkout's .git; nothing more was run there"sv;
    constexpr auto Unreachable = "HEAD is not contained in any ref"sv;
    constexpr auto UnrecognisedStatus = "unrecognised git status"sv;
    constexpr auto PaneIdRefused = "the tab's id is not a name"sv;
    constexpr auto HoldsSubmodule =
        "a submodule is in the index: git status would run git in its own repository, and git worktree "
        "remove refuses it"sv;
    constexpr auto UnrecognisedIndex = "unrecognised git ls-files output"sv;
    constexpr auto HiddenChanges =
        "a file is marked assume-unchanged or skip-worktree, so git status cannot see its changes"sv;
    constexpr auto NoProvider = "no provider stands for the repository's forge"sv;

    /// The one untracked file the extension itself writes, as `git status --porcelain` lists it.
    constexpr auto StatusBoardEntry = "?? STATUS.md"sv;

    // }}}
    // {{{ steps taken

    [[nodiscard]] std::string sanitized(std::string_view text)
    {
        return displayText(text, MaxEntryChars);
    }

    /// @return Why @p owner refuses one of @p arguments, if it does.
    [[nodiscard]] std::optional<std::string> refusal(std::string_view owner,
                                                     std::span<Argument const> arguments)
    {
        for (auto const& argument: arguments)
        {
            auto const* const decl = findArgumentDecl(argument.name);
            if (decl == nullptr || !argumentAccepts(decl->kind, argument.value))
                return std::format("{} refused the value of {{{}}}", owner, argument.name);
        }
        return std::nullopt;
    }

    /// The run's state after it ended. A kept entry checked again keeps its seen mark and its end time.
    /// @p resumedFrom may lie in the run's state: it is read before the state is replaced.
    [[nodiscard]] Effects finish(Run& run,
                                 StepContext const& context,
                                 Ended ended,
                                 Fate fate,
                                 std::vector<std::string> entries,
                                 std::optional<Finished> const& resumedFrom)
    {
        auto finished = Finished { .ended = ended,
                                   .fate = fate,
                                   .entries = std::move(entries),
                                   .seen = Seen::New,
                                   .endedWallMs = context.wallMs };
        if (resumedFrom)
        {
            finished.seen = resumedFrom->seen;
            finished.endedWallMs = resumedFrom->endedWallMs;
        }
        run.state = std::move(finished);
        return { Persist {} };
    }

    [[nodiscard]] Effects keep(Run& run,
                               StepContext const& context,
                               Ending const& ending,
                               std::string_view entry)
    {
        return finish(run, context, ending.cause, Fate::Kept, { sanitized(entry) }, ending.resumedFrom);
    }

    /// How a run whose worktree turned out to be gone ended: a carried failure, the end of a run already
    /// finished, or a run that asked for the removal itself, stays what it was.
    [[nodiscard]] Ended goneEnded(Run const& run, Ending const& ending) noexcept
    {
        if (run.failure || ending.resumedFrom || ending.removal == Removal::Issued)
            return ending.cause;
        return Ended::WorktreeGone;
    }

    /// @p ending at @p step; entering Remove records that the removal was asked for.
    [[nodiscard]] Ending nextStep(Ending ending, EndStep step)
    {
        ending.step = step;
        if (step == EndStep::Remove)
            ending.removal = Removal::Issued;
        return ending;
    }

    /// Whether a check that needs the main checkout must wait for it: it is not known yet.
    [[nodiscard]] constexpr bool awaitsCheckout(StepContext const& context) noexcept
    {
        return !context.mainCheckout && context.unknownCheckout == CheckoutUnknown::Pending;
    }

    /// @p ending back at the first removal check, keeping what it carries. The first checks compare the
    /// worktree against the main checkout: while it is not known yet, the run waits for it instead.
    [[nodiscard]] Effects toVerify(Run& run, Ending ending, StepContext const& context)
    {
        auto const waits = awaitsCheckout(context);
        ending.step = waits ? EndStep::MainCheckout : EndStep::Verify;
        run.state = std::move(ending);
        if (waits)
            return { Persist {} };
        return { Persist {}, ReadGitFile {} };
    }

    /// A check that needs the main checkout, which is not known. While it may still become known, the run
    /// waits for it and starts its checks again. Once it is known that it will not, a kept entry checked
    /// again stays exactly as recorded, its reason included: "not known" is no finding about the worktree.
    [[nodiscard]] Effects withoutCheckout(Run& run, StepContext const& context, Ending& ending)
    {
        if (awaitsCheckout(context))
            return toVerify(run, std::move(ending), context);
        if (!ending.resumedFrom)
            return keep(run, context, ending, UnknownMainCheckout);
        run.state = std::move(*ending.resumedFrom);
        return { Persist {} };
    }

    /// A run that starts its removal checks.
    [[nodiscard]] Ending endingFor(Ended cause, std::optional<Finished> resumedFrom = std::nullopt)
    {
        return Ending { .step = EndStep::Verify,
                        .cause = cause,
                        .resumedFrom = std::move(resumedFrom),
                        .removal = Removal::NotIssued };
    }

    /// Moves to @p next and runs @p name there; a value its kind refuses fails that step instead.
    [[nodiscard]] Effects enterCommand(Run& run,
                                       StepContext const& context,
                                       RunState next,
                                       std::string_view name,
                                       std::vector<Argument> arguments)
    {
        auto const* const command = findCommand(name);
        auto const why =
            command != nullptr ? refusal(name, arguments) : std::format("{} is not declared", name);
        run.state = std::move(next);
        if (why)
            return advance(run, CommandFailed { *why }, context);
        return { Persist {}, RunDeclared { .name = command->name, .arguments = std::move(arguments) } };
    }

    /// Moves to @p next and runs the command that plays @p role in the provider row of the run's forge there.
    [[nodiscard]] Effects enterPreparation(Run& run,
                                           StepContext const& context,
                                           RunState next,
                                           std::string_view Provider::* role,
                                           std::vector<Argument> arguments)
    {
        auto const* const provider = findProvider(run.forge);
        if (provider == nullptr)
        {
            run.state = std::move(next);
            return advance(run, CommandFailed { NoProvider }, context);
        }
        return enterCommand(run, context, std::move(next), provider->*role, std::move(arguments));
    }

    [[nodiscard]] Effects openPane(Run& run, StepContext const& context)
    {
        run.state = Preparing { PrepareStep::OpenPane };
        auto const arguments = std::array {
            Argument { .name = "model", .value = run.model },
            Argument { .name = "session", .value = run.session },
            Argument { .name = "prompt", .value = run.prompt },
        };
        // Refused before any tab was asked for, so there is none to wait for.
        if (auto const why = refusal(AgentPaneTemplate, arguments))
        {
            run.failure = RunFailure { .step = PrepareStep::OpenPane, .why = sanitized(*why) };
            return toVerify(run, endingFor(Ended::PreparationFailed), context);
        }
        return { Persist {}, OpenAgentPane {} };
    }

    [[nodiscard]] Effects prepared(Run& run, PrepareStep step, StepContext const& context)
    {
        auto const* const changeRequest = std::get_if<ChangeRequestRef>(&run.item);
        switch (step)
        {
            case PrepareStep::Fetch: {
                auto const base = std::format("{}/{}", run.remote, run.base);
                if (changeRequest != nullptr)
                    return enterPreparation(
                        run,
                        context,
                        Preparing { PrepareStep::WorktreeAdd },
                        &Provider::addWorktree,
                        { { .name = "path", .value = run.worktree }, { .name = "base", .value = base } });
                return enterPreparation(run,
                                        context,
                                        Preparing { PrepareStep::WorktreeAdd },
                                        &Provider::addBranchWorktree,
                                        { { .name = "branch", .value = run.branch },
                                          { .name = "path", .value = run.worktree },
                                          { .name = "base", .value = base } });
            }
            case PrepareStep::WorktreeAdd:
                if (changeRequest != nullptr)
                    return enterPreparation(
                        run,
                        context,
                        Preparing { PrepareStep::Checkout },
                        &Provider::checkout,
                        { { .name = "number", .value = std::format("{}", changeRequest->number) },
                          { .name = "branch", .value = run.branch } });
                return openPane(run, context);
            case PrepareStep::Checkout: return openPane(run, context);
            case PrepareStep::OpenPane: return {};
        }
        return {};
    }

    /// A step after the fetch may have made a worktree, so its failure goes through the removal checks.
    [[nodiscard]] Effects preparationFailed(Run& run,
                                            PrepareStep step,
                                            std::string_view why,
                                            StepContext const& context)
    {
        run.failure = RunFailure { .step = step, .why = sanitized(why) };
        switch (step)
        {
            case PrepareStep::Fetch:
                return finish(run, context, Ended::PreparationFailed, Fate::Removed, {}, std::nullopt);
            case PrepareStep::WorktreeAdd:
            case PrepareStep::Checkout: return toVerify(run, endingFor(Ended::PreparationFailed), context);
            case PrepareStep::OpenPane:
                // The tab may exist although its opening failed or was lost: nothing touches the worktree
                // until no tab tagged with the run is left.
                run.state = Ending { .step = EndStep::ClosePane,
                                     .cause = Ended::PreparationFailed,
                                     .resumedFrom = std::nullopt };
                return { Persist {}, CloseAgentPane {} };
        }
        return {};
    }

    [[nodiscard]] Effects verified(Run& run,
                                   Ending ending,
                                   GitFileRead const& read,
                                   StepContext const& context)
    {
        if (!read.content)
        {
            if (read.content.error() == FileReadError::DirectoryMissing)
                return finish(run, context, goneEnded(run, ending), Fate::Removed, {}, ending.resumedFrom);
            return keep(run,
                        context,
                        ending,
                        std::format("the .git entry is not a readable file: {}",
                                    spell(FileReadReasons, read.content.error())));
        }
        if (!context.mainCheckout)
            return withoutCheckout(run, context, ending);
        if (!gitFileIsIntact(*read.content, *context.mainCheckout, run.worktree))
            return keep(run, context, ending, AlteredGitFile);
        // The `.git` file is not the only link: git follows the `commondir` file beside the worktree's
        // directory, which the file check cannot read. Before anything else runs there, git says where it
        // leads.
        return enterCommand(
            run, context, nextStep(std::move(ending), EndStep::CommonDir), "git-common-dir", {});
    }

    [[nodiscard]] std::vector<std::string_view> nonEmptyLines(std::string_view text)
    {
        auto lines = std::vector<std::string_view> {};
        for (auto const part: std::views::split(text, '\n'))
            if (!part.empty())
                lines.emplace_back(part.begin(), part.end());
        return lines;
    }

    /// One line of `git ls-files -v --stage`: `<tag> <mode> <object> <stage>\t<path>`.
    struct IndexLine
    {
        char tag;              ///< Lowercase for assume-unchanged, `S` for skip-worktree.
        std::string_view mode; ///< Such as `100644`; `160000` for a gitlink.
    };

    /// @return @p line's tag and mode; std::nullopt for a line of another shape.
    [[nodiscard]] constexpr std::optional<IndexLine> indexLine(std::string_view line) noexcept
    {
        if (line.size() < 3 || line[1] != ' ' || !line.contains('\t'))
            return std::nullopt;
        auto const rest = line.substr(2);
        auto const space = rest.find(' ');
        if (space == std::string_view::npos || space == 0)
            return std::nullopt;
        return IndexLine { .tag = line.front(), .mode = rest.substr(0, space) };
    }

    /// A file whose changes `git status` does not see, which the unforced remove would delete.
    [[nodiscard]] constexpr bool hidesChanges(IndexLine const& entry) noexcept
    {
        return (entry.tag >= 'a' && entry.tag <= 'z') || entry.tag == 'S';
    }

    /// A gitlink: `git status --ignore-submodules=none` enters the submodule's repository, whose own
    /// config and info/attributes the agent wrote and which can name a filter (measured: harness/r11.sh).
    [[nodiscard]] constexpr bool isSubmodule(IndexLine const& entry) noexcept
    {
        return entry.mode == "160000";
    }

    /// What in the index keeps a worktree, checked in order on every line.
    struct IndexRule
    {
        bool (*matches)(IndexLine const&) noexcept;
        std::string_view why;
    };

    constexpr auto IndexRules = std::array {
        IndexRule { .matches = isSubmodule, .why = HoldsSubmodule },
        IndexRule { .matches = hidesChanges, .why = HiddenChanges },
    };

    /// @return Why the index keeps the worktree; std::nullopt when nothing in it does.
    [[nodiscard]] std::optional<std::string_view> indexKeeps(std::string_view output)
    {
        auto const lines = nonEmptyLines(output);
        auto entries = std::vector<IndexLine> {};
        for (auto const line: lines)
        {
            auto const entry = indexLine(line);
            if (!entry)
                return UnrecognisedIndex;
            entries.push_back(*entry);
        }
        for (auto const& rule: IndexRules)
            if (std::ranges::any_of(entries, rule.matches))
                return rule.why;
        return std::nullopt;
    }

    [[nodiscard]] Effects statusRead(Run& run,
                                     Ending ending,
                                     std::string_view porcelain,
                                     StepContext const& context)
    {
        auto const lines = nonEmptyLines(porcelain);
        if (lines.empty())
            return enterCommand(run,
                                context,
                                nextStep(std::move(ending), EndStep::Remove),
                                "git-worktree-remove",
                                { { .name = "path", .value = run.worktree } });
        if (lines.size() == 1 && lines.front() == StatusBoardEntry)
            return enterCommand(
                run, context, nextStep(std::move(ending), EndStep::CleanStatusBoard), "git-clean", {});
        auto entries = std::vector<std::string> {};
        for (auto const line: lines | std::views::take(MaxStatusEntries))
            entries.push_back(sanitized(line));
        if (lines.size() > MaxStatusEntries)
            entries.push_back(std::format("and {} more", lines.size() - MaxStatusEntries));
        return finish(run, context, ending.cause, Fate::Kept, std::move(entries), ending.resumedFrom);
    }

    [[nodiscard]] Effects commandDone(Run& run,
                                      Ending ending,
                                      std::string_view output,
                                      StepContext const& context)
    {
        switch (ending.step)
        {
            case EndStep::CommonDir:
                if (!context.mainCheckout)
                    return withoutCheckout(run, context, ending);
                if (!commonDirIsMain(output, *context.mainCheckout))
                    return keep(run, context, ending, RedirectedCommonDir);
                return enterCommand(
                    run, context, nextStep(std::move(ending), EndStep::IndexFlags), "git-index-flags", {});
            case EndStep::IndexFlags:
                if (auto const why = indexKeeps(output))
                    return keep(run, context, ending, *why);
                return enterCommand(
                    run, context, nextStep(std::move(ending), EndStep::Reachable), "git-head-reachable", {});
            case EndStep::Reachable:
                if (nonEmptyLines(output).empty())
                    return keep(run, context, ending, Unreachable);
                return enterCommand(run,
                                    context,
                                    nextStep(std::move(ending), EndStep::OperationState),
                                    "git-operation-state",
                                    {});
            case EndStep::OperationState: {
                auto const operation = operationInProgress(output);
                if (!operation)
                    return keep(run, context, ending, UnrecognisedStatus);
                if (*operation)
                    return keep(
                        run, context, ending, std::format("{} in progress", operationName(**operation)));
                return enterCommand(
                    run, context, nextStep(std::move(ending), EndStep::Status), "git-status", {});
            }
            case EndStep::Status: return statusRead(run, std::move(ending), output, context);
            case EndStep::CleanStatusBoard:
                return enterCommand(run,
                                    context,
                                    nextStep(std::move(ending), EndStep::Remove),
                                    "git-worktree-remove",
                                    { { .name = "path", .value = run.worktree } });
            case EndStep::Remove:
                return finish(run, context, ending.cause, Fate::Removed, {}, ending.resumedFrom);
            case EndStep::ClosePane:
            case EndStep::MainCheckout:
            case EndStep::Verify: return {};
        }
        return {};
    }

    [[nodiscard]] constexpr bool awaitsCommand(Ending const& ending) noexcept
    {
        return endStep(ending.step).awaits == Awaits::Command;
    }

    // }}}
} // namespace

std::expected<std::vector<RunEffect>, BeginError> beginRun(Run& run, StepContext const& context)
{
    auto const* const preparing = std::get_if<Preparing>(&run.state);
    if (preparing == nullptr || preparing->step != PrepareStep::Fetch)
        return std::unexpected { BeginError::NotPreparing };
    if (!isStorable(run))
        return std::unexpected { BeginError::NotStorable };
    run.changedMonoMs = context.monoMs;
    return enterPreparation(run,
                            context,
                            Preparing { PrepareStep::Fetch },
                            &Provider::fetch,
                            { { .name = "remote", .value = run.remote } });
}

std::vector<RunEffect> advance(Run& run, RunInput const& input, StepContext const& context)
{
    auto const visitor = Overloaded {
        // Preparing
        [&](Preparing preparing, CommandOk const&) { return prepared(run, preparing.step, context); },
        [&](Preparing preparing, CommandFailed const& failed) {
            return preparationFailed(run, preparing.step, failed.why, context);
        },
        [&](Preparing preparing, Lost const&) {
            return preparationFailed(run, preparing.step, LostWhy, context);
        },
        [&](Preparing preparing, PaneOpenFailed const& failed) -> Effects {
            if (preparing.step == PrepareStep::Fetch)
                return {};
            return preparationFailed(run, preparing.step, failed.why, context);
        },
        [&](Preparing preparing, CwdMissing const&) -> Effects {
            // The command could not start, so it made nothing: there is no worktree to check.
            if (preparing.step == PrepareStep::OpenPane)
                return {};
            run.failure = RunFailure { .step = preparing.step, .why = std::string(CwdMissingWhy) };
            return finish(run, context, Ended::PreparationFailed, Fate::Removed, {}, std::nullopt);
        },
        [&](Preparing preparing, PaneOpened const& opened) -> Effects {
            if (preparing.step != PrepareStep::OpenPane)
                return {};
            // The id is stored and passed back to close the tab: one that is no Name is not kept.
            if (!argumentAccepts(ArgumentKind::Name, opened.pane))
                return preparationFailed(run, preparing.step, PaneIdRefused, context);
            run.state = Running {};
            run.pane = std::string(opened.pane);
            run.runningSinceMs = opened.monoMs;
            return { Persist {} };
        },
        // Running, Blocked, Done
        [&](AgentState auto const&, StatusReported const& reported) -> Effects {
            auto next = stateFor(reported.status);
            auto const unchanged = next.index() == run.state.index() && run.lastStatus == reported.status;
            run.state = std::move(next);
            run.lastStatus = reported.status;
            if (unchanged)
                return {};
            return { Persist {} };
        },
        [&](AgentState auto const&, PaneGone const&) {
            run.pane.reset();
            return toVerify(
                run, endingFor(run.lastStatus ? Ended::Completed : Ended::NeverReported), context);
        },
        [&](AgentState auto const&, EndByUser const&) -> Effects {
            // Nothing touches the worktree until the tab is gone.
            run.state = Ending { .step = EndStep::ClosePane,
                                 .cause = Ended::EndedByYou,
                                 .resumedFrom = std::nullopt };
            return { Persist {}, CloseAgentPane {} };
        },
        // Ending
        [&](Ending ending, PaneGone const&) -> Effects {
            if (ending.step != EndStep::ClosePane)
                return {};
            run.pane.reset();
            return toVerify(run, std::move(ending), context);
        },
        [&](Ending ending, Lost const&) -> Effects {
            // A close lost in a restart is asked for again; the run still waits for the tab to be gone.
            if (endStep(ending.step).awaits == Awaits::PaneGone)
                return { Persist {}, CloseAgentPane {} };
            return toVerify(run, std::move(ending), context);
        },
        [&](Ending ending, CheckoutListed const&) -> Effects {
            // Still not known: the run waits on, unchanged.
            if (ending.step != EndStep::MainCheckout || awaitsCheckout(context))
                return {};
            return toVerify(run, std::move(ending), context);
        },
        [&](Ending ending, GitFileRead const& read) -> Effects {
            if (ending.step != EndStep::Verify)
                return {};
            return verified(run, std::move(ending), read, context);
        },
        [&](Ending ending, CommandOk const& ok) -> Effects {
            if (!awaitsCommand(ending))
                return {};
            return commandDone(run, std::move(ending), ok.stdoutText, context);
        },
        [&](Ending const& ending, CommandFailed const& failed) -> Effects {
            if (!awaitsCommand(ending))
                return {};
            return keep(
                run, context, ending, std::format("{} failed: {}", endStep(ending.step).label, failed.why));
        },
        [&](Ending const& ending, CwdMissing const&) -> Effects {
            if (!awaitsCommand(ending))
                return {};
            return finish(run, context, goneEnded(run, ending), Fate::Removed, {}, ending.resumedFrom);
        },
        [&](Ending& ending, MarkSeen const&) -> Effects {
            if (!ending.resumedFrom || ending.resumedFrom->seen == Seen::Opened)
                return {};
            ending.resumedFrom->seen = Seen::Opened;
            return { Persist {} };
        },
        // Finished
        [&](Finished finished, RetryRemoval const&) -> Effects {
            if (finished.fate != Fate::Kept)
                return {};
            auto const ended = finished.ended;
            return toVerify(run, endingFor(ended, std::move(finished)), context);
        },
        [&](Finished& finished, MarkSeen const&) -> Effects {
            if (finished.seen == Seen::Opened)
                return {};
            finished.seen = Seen::Opened;
            return { Persist {} };
        },
        [&](Finished const& finished, Dismiss const&) -> Effects {
            if (finished.fate != Fate::Removed)
                return {};
            return { Forget {} };
        },
        // Every other input leaves the run as it is.
        [](auto const&, auto const&) -> Effects { return {}; },
    };
    auto effects = std::visit(visitor, run.state, input);
    if (!effects.empty())
        run.changedMonoMs = context.monoMs;
    return effects;
}

std::optional<std::string> stepTag(Run const& run)
{
    if (auto const* const preparing = std::get_if<Preparing>(&run.state))
        return std::format("{}/{}", run.id, spell(PrepareStepNames, preparing->step));
    if (auto const* const ending = std::get_if<Ending>(&run.state))
    {
        auto const& step = endStep(ending->step);
        if (step.awaits == Awaits::GitFile || step.awaits == Awaits::Command)
            return std::format("{}/{}", run.id, step.name);
    }
    return std::nullopt;
}

bool countsAsActive(Run const& run) noexcept
{
    return std::holds_alternative<Preparing>(run.state) || std::holds_alternative<Running>(run.state)
           || std::holds_alternative<Blocked>(run.state) || std::holds_alternative<Done>(run.state);
}

bool isUnfinished(Run const& run) noexcept
{
    return !std::holds_alternative<Finished>(run.state);
}

std::string_view agentStatusName(AgentStatus status) noexcept
{
    return spell(AgentStatusNames, status);
}

std::optional<AgentStatus> agentStatusNamed(std::string_view name) noexcept
{
    return parse(AgentStatusNames, name);
}

RunState stateFor(AgentStatus status) noexcept
{
    switch (status)
    {
        case AgentStatus::Idle:
        case AgentStatus::Working: return Running {};
        case AgentStatus::Blocked:
        case AgentStatus::Error: return Blocked {};
        case AgentStatus::Done: return Done {};
    }
    return Running {};
}

bool commonDirIsMain(std::string_view output, std::string_view mainCheckout) noexcept
{
    if (mainCheckout.empty() || !output.starts_with(mainCheckout))
        return false;
    auto const rest = output.substr(mainCheckout.size());
    return rest == "/.git" || rest == "/.git\n";
}

bool gitFileIsIntact(std::string_view content,
                     std::string_view mainCheckout,
                     std::string_view worktree) noexcept
{
    auto const slash = worktree.rfind('/');
    auto const name = slash == std::string_view::npos ? worktree : worktree.substr(slash + 1);
    if (name.empty() || mainCheckout.empty())
        return false;
    auto rest = content;
    auto const consume = [&rest](std::string_view part) {
        if (!rest.starts_with(part))
            return false;
        rest.remove_prefix(part.size());
        return true;
    };
    return consume("gitdir: ") && consume(mainCheckout) && consume("/.git/worktrees/") && consume(name)
           && (rest.empty() || rest == "\n");
}

// {{{ storage bounds

namespace
{
    /// The shapes a stored field may have. Each bounds how long the field's JSON can grow.
    enum class Shape : uint8_t
    {
        Name,   ///< A Name: never escaped.
        Uuid,   ///< A Uuid: never escaped.
        Ref,    ///< A Ref: never escaped.
        Path,   ///< A Path without a control character: at most twice its MaxPathBytes once escaped.
        Url,    ///< At most MaxUrlBytes URL bytes: at most twice that once escaped.
        Prompt, ///< Text whose only control characters are line ends and tabs: at most twice MaxTextBytes.
        Entry,  ///< At most MaxEntryChars characters, MaxEntryBytes bytes, no control character.
    };

    [[nodiscard]] constexpr bool isControl(char c) noexcept
    {
        auto const byte = static_cast<unsigned char>(c);
        return byte < 0x20 || byte == 0x7f;
    }

    [[nodiscard]] constexpr bool isUtf8Lead(char c) noexcept
    {
        return (static_cast<unsigned char>(c) & 0xC0) != 0x80;
    }

    [[nodiscard]] constexpr bool fits(Shape shape, std::string_view value) noexcept
    {
        switch (shape)
        {
            case Shape::Name: return argumentAccepts(ArgumentKind::Name, value);
            case Shape::Uuid: return argumentAccepts(ArgumentKind::Uuid, value);
            case Shape::Ref: return argumentAccepts(ArgumentKind::Ref, value);
            case Shape::Path:
                return argumentAccepts(ArgumentKind::Path, value) && std::ranges::none_of(value, isControl);
            case Shape::Url: return value.size() <= MaxUrlBytes && std::ranges::all_of(value, isUrlByte);
            case Shape::Prompt:
                return argumentAccepts(ArgumentKind::Text, value) && std::ranges::none_of(value, [](char c) {
                           return isControl(c) && c != '\n' && c != '\t';
                       });
            case Shape::Entry:
                return value.size() <= MaxEntryBytes
                       && std::cmp_less_equal(std::ranges::count_if(value, isUtf8Lead), MaxEntryChars)
                       && std::ranges::none_of(value, isControl);
        }
        return false;
    }

    [[nodiscard]] bool entriesFit(Finished const& finished) noexcept
    {
        return finished.entries.size() <= MaxStatusEntries + 1
               && std::ranges::all_of(finished.entries,
                                      [](std::string const& entry) { return fits(Shape::Entry, entry); });
    }
} // namespace

bool isStorable(Run const& run) noexcept
{
    struct Field
    {
        Shape shape;
        std::string_view value;
    };
    auto const fields = std::array {
        Field { .shape = Shape::Name, .value = run.id },
        Field { .shape = Shape::Name, .value = run.repo },
        Field { .shape = Shape::Path, .value = run.repoPath },
        Field { .shape = Shape::Name, .value = run.remote },
        Field { .shape = Shape::Ref, .value = run.base },
        Field { .shape = Shape::Url, .value = run.url },
        Field { .shape = Shape::Name, .value = run.task },
        Field { .shape = Shape::Name, .value = run.model },
        Field { .shape = Shape::Prompt, .value = run.prompt },
        Field { .shape = Shape::Uuid, .value = run.session },
        Field { .shape = Shape::Ref, .value = run.branch },
        Field { .shape = Shape::Path, .value = run.worktree },
    };
    if (!std::ranges::all_of(fields, [](Field const& field) { return fits(field.shape, field.value); }))
        return false;
    if (run.pane && !fits(Shape::Name, *run.pane))
        return false;
    if (run.near && !fits(Shape::Name, *run.near))
        return false;
    if (run.failure && !fits(Shape::Entry, run.failure->why))
        return false;
    if (auto const* const jira = std::get_if<JiraRef>(&run.item); jira != nullptr && !isJiraKey(jira->key))
        return false;
    if (auto const* const finished = std::get_if<Finished>(&run.state))
        return entriesFit(*finished);
    if (auto const* const ending = std::get_if<Ending>(&run.state); ending != nullptr && ending->resumedFrom)
        return entriesFit(*ending->resumedFrom);
    return true;
}

// }}}
// {{{ storage

namespace
{
    using run::detail::JsonFailure;
    using run::detail::JsonFinished;
    using run::detail::JsonRun;

    /// The input is a string_view: nothing terminates it. Every key toJson writes for a state must be there,
    /// and no other.
    constexpr auto ReadOptions = glz::opts { .null_terminated = false, .error_on_missing_keys = true };

    constexpr auto ChangeRequestKind = "pr"sv;
    constexpr auto IssueKind = "issue"sv;
    constexpr auto JiraKind = "jira"sv;

    [[nodiscard]] JsonFinished jsonOf(Finished const& finished)
    {
        return JsonFinished { .ended = std::string(spell(EndedNames, finished.ended)),
                              .fate = std::string(spell(FateNames, finished.fate)),
                              .entries = finished.entries,
                              .seen = std::string(spell(SeenNames, finished.seen)),
                              .endedWallMs = finished.endedWallMs };
    }

    [[nodiscard]] std::optional<Finished> finishedOf(JsonFinished const& json)
    {
        auto const ended = parse(EndedNames, json.ended);
        auto const fate = parse(FateNames, json.fate);
        auto const seen = parse(SeenNames, json.seen);
        if (!ended || !fate || !seen)
            return std::nullopt;
        return Finished { .ended = *ended,
                          .fate = *fate,
                          .entries = json.entries,
                          .seen = *seen,
                          .endedWallMs = json.endedWallMs };
    }

    [[nodiscard]] std::optional<ItemRef> itemOf(JsonRun const& json)
    {
        if (json.itemKind == JiraKind)
        {
            if (json.itemNumber || !json.itemKey || !isJiraKey(*json.itemKey))
                return std::nullopt;
            return JiraRef { .key = *json.itemKey };
        }
        if (json.itemKey || !json.itemNumber
            || !argumentAccepts(ArgumentKind::Number, std::format("{}", *json.itemNumber)))
            return std::nullopt;
        if (json.itemKind == ChangeRequestKind)
            return ChangeRequestRef { .number = *json.itemNumber };
        if (json.itemKind == IssueKind)
            return IssueRef { .number = *json.itemNumber };
        return std::nullopt;
    }

    /// RunState's index of @p S.
    template <typename S, std::size_t I = 0>
    [[nodiscard]] consteval std::size_t stateIndex() noexcept
    {
        if constexpr (std::is_same_v<std::variant_alternative_t<I, RunState>, S>)
            return I;
        else
            return stateIndex<S, I + 1>();
    }

    /// The state the fields name; each state has exactly the fields toJson writes for it.
    [[nodiscard]] std::optional<RunState> stateOf(JsonRun const& json)
    {
        auto const index =
            static_cast<std::size_t>(std::ranges::find(StateNames, json.state) - StateNames.begin());
        auto const finished = json.finished ? finishedOf(*json.finished) : std::nullopt;
        if ((json.finished && !finished) || (json.removal && index != stateIndex<Ending>()))
            return std::nullopt;
        auto const agentState = [&](RunState state) -> std::optional<RunState> {
            if (json.step || json.cause || json.finished)
                return std::nullopt;
            return state;
        };
        switch (index)
        {
            case stateIndex<Preparing>(): {
                auto const step = json.step ? parse(PrepareStepNames, *json.step) : std::nullopt;
                if (!step || json.cause || json.finished)
                    return std::nullopt;
                return Preparing { *step };
            }
            case stateIndex<Running>(): return agentState(Running {});
            case stateIndex<Blocked>(): return agentState(Blocked {});
            case stateIndex<Done>(): return agentState(Done {});
            case stateIndex<Ending>(): {
                auto const step = json.step ? parseEndStep(*json.step) : std::nullopt;
                auto const cause = json.cause ? parse(EndedNames, *json.cause) : std::nullopt;
                auto const removal = json.removal ? parse(RemovalNames, *json.removal) : std::nullopt;
                if (!step || !cause || !removal)
                    return std::nullopt;
                // Only a kept entry is checked again, and its end is the ending's cause.
                if (finished && (finished->fate != Fate::Kept || finished->ended != *cause))
                    return std::nullopt;
                return Ending {
                    .step = *step, .cause = *cause, .resumedFrom = finished, .removal = *removal
                };
            }
            case stateIndex<Finished>():
                if (json.step || json.cause || !finished)
                    return std::nullopt;
                return *finished;
            default: return std::nullopt;
        }
    }
} // namespace

std::string toJson(Run const& run)
{
    auto json = JsonRun {};
    json.v = RunSchema;
    json.id = run.id;
    json.repo = run.repo;
    json.repoPath = run.repoPath;
    json.forge = forgeName(run.forge);
    json.remote = run.remote;
    json.base = run.base;
    std::visit(Overloaded {
                   [&](ChangeRequestRef const& ref) {
                       json.itemKind = ChangeRequestKind;
                       json.itemNumber = ref.number;
                   },
                   [&](IssueRef const& ref) {
                       json.itemKind = IssueKind;
                       json.itemNumber = ref.number;
                   },
                   [&](JiraRef const& ref) {
                       json.itemKind = JiraKind;
                       json.itemKey = ref.key;
                   },
               },
               run.item);
    json.url = run.url;
    json.task = run.task;
    json.model = run.model;
    json.prompt = run.prompt;
    json.session = run.session;
    json.branch = run.branch;
    json.worktree = run.worktree;
    json.pane = run.pane;
    json.near = run.near;
    json.state = StateNames[run.state.index()];
    std::visit(Overloaded {
                   [&](Preparing const& preparing) { json.step = spell(PrepareStepNames, preparing.step); },
                   [&](Ending const& ending) {
                       json.step = endStep(ending.step).name;
                       json.cause = spell(EndedNames, ending.cause);
                       json.removal = spell(RemovalNames, ending.removal);
                       if (ending.resumedFrom)
                           json.finished = jsonOf(*ending.resumedFrom);
                   },
                   [&](Finished const& finished) { json.finished = jsonOf(finished); },
                   [](AgentState auto const&) {},
               },
               run.state);
    if (run.lastStatus)
        json.lastStatus = spell(AgentStatusNames, *run.lastStatus);
    if (run.failure)
        json.failure = JsonFailure { .step = std::string(spell(PrepareStepNames, run.failure->step)),
                                     .why = run.failure->why };
    json.startedWallMs = run.startedWallMs;
    json.attempt = run.attempt;

    auto text = std::string {};
    if (glz::write_json(json, text))
        return {};
    return text;
}

std::optional<Run> runFromJson(std::string_view text)
{
    auto read = json::readDocument<ReadOptions, JsonRun>(text);
    if (!read)
        return std::nullopt;
    auto& json = *read;
    if (json.v != RunSchema)
        return std::nullopt;

    auto item = itemOf(json);
    auto state = stateOf(json);
    auto const lastStatus = json.lastStatus ? parse(AgentStatusNames, *json.lastStatus) : std::nullopt;
    auto const failureStep = json.failure ? parse(PrepareStepNames, json.failure->step) : std::nullopt;
    auto const forge = forgeNamed(json.forge);
    if (!item || !state || !forge || (json.lastStatus && !lastStatus) || (json.failure && !failureStep))
        return std::nullopt;

    auto run = Run {
        .id = std::move(json.id),
        .repo = std::move(json.repo),
        .repoPath = std::move(json.repoPath),
        .forge = *forge,
        .remote = std::move(json.remote),
        .base = std::move(json.base),
        .item = std::move(*item),
        .url = std::move(json.url),
        .task = std::move(json.task),
        .model = std::move(json.model),
        .prompt = std::move(json.prompt),
        .session = std::move(json.session),
        .branch = std::move(json.branch),
        .worktree = std::move(json.worktree),
        .pane = std::move(json.pane),
        .near = std::move(json.near),
        .state = std::move(*state),
        .lastStatus = lastStatus,
        .failure = json.failure
                       ? std::optional { RunFailure { .step = *failureStep, .why = json.failure->why } }
                       : std::nullopt,
        .startedWallMs = json.startedWallMs,
        .attempt = json.attempt,
        .runningSinceMs = 0,
        .changedMonoMs = 0,
    };
    if (!isStorable(run))
        return std::nullopt;
    return run;
}

// }}}

} // namespace orchestrator
