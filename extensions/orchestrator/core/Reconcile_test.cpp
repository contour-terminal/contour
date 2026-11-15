// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <orchestrator/core/Fixtures.hpp>
#include <orchestrator/core/Overloaded.hpp>
#include <orchestrator/core/Reconcile.hpp>
#include <orchestrator/core/Run.hpp>

using namespace std::string_literals;
using namespace std::string_view_literals;

namespace
{

using orchestrator::Adopt;
using orchestrator::AgentStatus;
using orchestrator::ClosePane;
using orchestrator::Decision;
using orchestrator::Ended;
using orchestrator::Ending;
using orchestrator::EndStep;
using orchestrator::Fate;
using orchestrator::Finished;
using orchestrator::FixtureRoot;
using orchestrator::PaneFact;
using orchestrator::PaneListing;
using orchestrator::PaneWentAway;
using orchestrator::PrepareStep;
using orchestrator::Preparing;
using orchestrator::ProgramLiveness;
using orchestrator::Run;
using orchestrator::RunState;
using orchestrator::StatusChanged;
using orchestrator::WorktreeEntry;
using orchestrator::WorktreeKind;

constexpr auto RunIdText = "pr-2334-20261113-150847"sv;
constexpr auto MainCheckout = "/home/u/projects/contour"sv;

// {{{ reconcile

/// The state a row's run is in.
enum class State : uint8_t
{
    PreparingFetch,
    PreparingWorktreeAdd,
    PreparingCheckout,
    PreparingOpenPane,
    Running,
    Blocked,
    Done,
    EndingClosePane,
    EndingVerify,
    EndingStatus,
    FinishedKept,
    FinishedRemoved,
};

/// A status, or none: rows stay constexpr without a std::optional.
enum class Status : uint8_t
{
    None,
    Idle,
    Working,
    Done,
    Blocked,
    Error,
};

/// The panes a row lists.
enum class Listed : uint8_t
{
    Nothing,      ///< No pane at all.
    Alive,        ///< The run's tab, its agent running.
    Gone,         ///< The run's tab, a shell left where its agent was.
    AliveAndGone, ///< Both: a second, leftover tab tagged with the run.
    Stray,        ///< Only a tab whose tag names no run.
};

/// What a row expects.
enum class Expect : uint8_t
{
    Nothing,
    StatusChanged,
    PaneWentAway,
    ClosePane,  ///< The shell left where the agent was.
    CloseAlive, ///< The agent's tab.
    CloseBoth,  ///< Both tabs tagged with the run.
    Adopt,
    AdoptAndStatus,
};

constexpr auto Tagged = PaneListing::Tagged;

/// When the rows' listings are asked for; the rows' runs last changed before.
constexpr auto ListedMs = uint64_t { 100 };

constexpr auto AlivePane = "p-alive"sv;
constexpr auto GonePane = "p-gone"sv;
constexpr auto StrayPane = "p-stray"sv;

[[nodiscard]] std::optional<AgentStatus> toOptional(Status status)
{
    switch (status)
    {
        case Status::None: return std::nullopt;
        case Status::Idle: return AgentStatus::Idle;
        case Status::Working: return AgentStatus::Working;
        case Status::Done: return AgentStatus::Done;
        case Status::Blocked: return AgentStatus::Blocked;
        case Status::Error: return AgentStatus::Error;
    }
    return std::nullopt;
}

[[nodiscard]] Finished finished(Fate fate)
{
    auto entries = std::vector<std::string> {};
    if (fate == Fate::Kept)
        entries.emplace_back("?? notes.txt");
    return Finished { .ended = Ended::Completed,
                      .fate = fate,
                      .entries = std::move(entries),
                      .seen = orchestrator::Seen::New,
                      .endedWallMs = 1 };
}

[[nodiscard]] Ending ending(EndStep step)
{
    return Ending { .step = step,
                    .cause = Ended::EndedByYou,
                    .resumedFrom = std::nullopt,
                    .removal = orchestrator::Removal::NotIssued };
}

[[nodiscard]] RunState stateOf(State state)
{
    switch (state)
    {
        case State::PreparingFetch: return Preparing { .step = PrepareStep::Fetch };
        case State::PreparingWorktreeAdd: return Preparing { .step = PrepareStep::WorktreeAdd };
        case State::PreparingCheckout: return Preparing { .step = PrepareStep::Checkout };
        case State::PreparingOpenPane: return Preparing { .step = PrepareStep::OpenPane };
        case State::Running: return orchestrator::Running {};
        case State::Blocked: return orchestrator::Blocked {};
        case State::Done: return orchestrator::Done {};
        case State::EndingClosePane: return ending(EndStep::ClosePane);
        case State::EndingVerify: return ending(EndStep::Verify);
        case State::EndingStatus: return ending(EndStep::Status);
        case State::FinishedKept: return finished(Fate::Kept);
        case State::FinishedRemoved: return finished(Fate::Removed);
    }
    return orchestrator::Running {};
}

[[nodiscard]] Run makeRun(std::string const& id, RunState state, std::optional<AgentStatus> lastStatus)
{
    auto const usesPane = std::holds_alternative<orchestrator::Running>(state)
                          || std::holds_alternative<orchestrator::Blocked>(state)
                          || std::holds_alternative<orchestrator::Done>(state);
    auto run = Run {
        .id = id,
        .repo = "contour",
        .repoPath = std::string(MainCheckout),
        .forge = orchestrator::Forge::GitHub,
        .remote = "origin",
        .base = "master",
        .item = orchestrator::ChangeRequestRef { .number = 2334 },
        .url = "https://github.com/contour-terminal/contour/pull/2334",
        .task = "review",
        .model = "opus",
        .prompt = "Review https://github.com/contour-terminal/contour/pull/2334",
        .session = "123e4567-e89b-12d3-a456-426614174000",
        .branch = "agent/" + id,
        .worktree = "/home/u/worktrees/contour/" + id,
        .pane = std::nullopt,
        .state = std::move(state),
        .lastStatus = lastStatus,
        .failure = std::nullopt,
        .boardPosts = 0,
        .startedWallMs = 1,
        .runningSinceMs = 0,
        .boardPostedMs = 0,
        .changedMonoMs = 0,
    };
    if (usesPane)
        run.pane = std::string(AlivePane);
    return run;
}

[[nodiscard]] std::vector<PaneFact> panesFor(Listed listed, Status paneStatus)
{
    auto const alive = PaneFact { .pane = std::string(AlivePane),
                                  .tag = std::string(RunIdText),
                                  .status = toOptional(paneStatus),
                                  .program = ProgramLiveness::Alive };
    auto const gone = PaneFact { .pane = std::string(GonePane),
                                 .tag = std::string(RunIdText),
                                 .status = toOptional(paneStatus),
                                 .program = ProgramLiveness::Gone };
    switch (listed)
    {
        case Listed::Nothing: return {};
        case Listed::Alive: return { alive };
        case Listed::Gone: return { gone };
        case Listed::AliveAndGone: return { alive, gone };
        case Listed::Stray:
            return { PaneFact { .pane = std::string(StrayPane),
                                .tag = "issue-17-20261113-150847",
                                .status = AgentStatus::Working,
                                .program = ProgramLiveness::Alive } };
    }
    return {};
}

struct ReconcileRow
{
    std::string_view name;
    State state;
    Status lastStatus;
    Listed listed;
    Status paneStatus;
    Expect expect;
};

// clang-format off
constexpr auto ReconcileRows = std::array {
    // An active run whose agent reports a status the run has not taken in.
    ReconcileRow { "running, new status",          State::Running, Status::Working, Listed::Alive, Status::Blocked, Expect::StatusChanged },
    ReconcileRow { "running, same status",         State::Running, Status::Working, Listed::Alive, Status::Working, Expect::Nothing },
    ReconcileRow { "running, never reported",      State::Running, Status::None,    Listed::Alive, Status::Idle,    Expect::StatusChanged },
    ReconcileRow { "running, pane reports nothing", State::Running, Status::Working, Listed::Alive, Status::None,   Expect::Nothing },
    ReconcileRow { "running, neither reported",    State::Running, Status::None,    Listed::Alive, Status::None,    Expect::Nothing },
    ReconcileRow { "blocked, working again",       State::Blocked, Status::Blocked, Listed::Alive, Status::Working, Expect::StatusChanged },
    ReconcileRow { "blocked, error",               State::Blocked, Status::Blocked, Listed::Alive, Status::Error,   Expect::StatusChanged },
    ReconcileRow { "done stays active",            State::Done,    Status::Done,    Listed::Alive, Status::Done,    Expect::Nothing },
    ReconcileRow { "done, resumed",                State::Done,    Status::Done,    Listed::Alive, Status::Working, Expect::StatusChanged },
    // The pane is no longer listed.
    ReconcileRow { "running, pane absent",         State::Running, Status::Working, Listed::Nothing, Status::None,  Expect::PaneWentAway },
    ReconcileRow { "blocked, pane absent",         State::Blocked, Status::Blocked, Listed::Nothing, Status::None,  Expect::PaneWentAway },
    ReconcileRow { "done, pane absent",            State::Done,    Status::Done,    Listed::Nothing, Status::None,  Expect::PaneWentAway },
    ReconcileRow { "running, only a stray pane",   State::Running, Status::Working, Listed::Stray,   Status::None,  Expect::PaneWentAway },
    ReconcileRow { "never reported, pane absent",  State::Running, Status::None,    Listed::Nothing, Status::None,  Expect::PaneWentAway },
    // A cold restart left a shell: it is closed first, and the run hears of it once it is gone.
    ReconcileRow { "running, shell left",          State::Running, Status::Working, Listed::Gone, Status::Working,  Expect::ClosePane },
    ReconcileRow { "done, shell left",             State::Done,    Status::Done,    Listed::Gone, Status::None,     Expect::ClosePane },
    ReconcileRow { "running, shell left beside",   State::Running, Status::Working, Listed::AliveAndGone, Status::Working, Expect::ClosePane },
    // A run waiting for its tab, whose tab is listed.
    ReconcileRow { "opening, tab listed",          State::PreparingOpenPane, Status::None, Listed::Alive, Status::None,    Expect::Adopt },
    ReconcileRow { "opening, tab reported",        State::PreparingOpenPane, Status::None, Listed::Alive, Status::Working, Expect::AdoptAndStatus },
    ReconcileRow { "opening, shell left",          State::PreparingOpenPane, Status::None, Listed::Gone,  Status::None,    Expect::Nothing },
    ReconcileRow { "opening, nothing listed",      State::PreparingOpenPane, Status::None, Listed::Nothing, Status::None,  Expect::Nothing },
    ReconcileRow { "opening, only a stray pane",   State::PreparingOpenPane, Status::None, Listed::Stray, Status::None,    Expect::Nothing },
    // States reconciliation leaves alone.
    ReconcileRow { "fetching, tab listed",         State::PreparingFetch,       Status::None, Listed::Alive,   Status::Working, Expect::Nothing },
    ReconcileRow { "adding worktree, tab listed",  State::PreparingWorktreeAdd, Status::None, Listed::Alive,   Status::Working, Expect::Nothing },
    ReconcileRow { "checking out, nothing listed", State::PreparingCheckout,    Status::None, Listed::Nothing, Status::None,    Expect::Nothing },
    ReconcileRow { "verifying, nothing listed",    State::EndingVerify,    Status::Done,    Listed::Nothing, Status::None,    Expect::Nothing },
    ReconcileRow { "status check, tab listed",     State::EndingStatus,    Status::Done,    Listed::Alive,   Status::Working, Expect::Nothing },
    ReconcileRow { "kept, tab listed",             State::FinishedKept,    Status::Done,    Listed::Alive,   Status::Working, Expect::Nothing },
    ReconcileRow { "removed, nothing listed",      State::FinishedRemoved, Status::Done,    Listed::Nothing, Status::None,    Expect::Nothing },
    ReconcileRow { "removed, stray pane",          State::FinishedRemoved, Status::Done,    Listed::Stray,   Status::None,    Expect::Nothing },
    // A run whose tab is being closed waits until no tab tagged with it is listed.
    ReconcileRow { "closing, tab listed",          State::EndingClosePane, Status::Working, Listed::Alive,   Status::Working, Expect::CloseAlive },
    ReconcileRow { "closing, shell listed",        State::EndingClosePane, Status::Working, Listed::Gone,    Status::None,    Expect::ClosePane },
    ReconcileRow { "closing, both listed",         State::EndingClosePane, Status::Working, Listed::AliveAndGone, Status::None, Expect::CloseBoth },
    ReconcileRow { "closing, only a stray pane",   State::EndingClosePane, Status::Working, Listed::Stray,   Status::None,    Expect::PaneWentAway },
    ReconcileRow { "closing, tab gone",            State::EndingClosePane, Status::Working, Listed::Nothing, Status::None,    Expect::PaneWentAway },
};
// clang-format on

[[nodiscard]] std::vector<Decision> expected(ReconcileRow const& row)
{
    auto const run = std::string(RunIdText);
    auto const status = toOptional(row.paneStatus).value_or(AgentStatus::Idle);
    switch (row.expect)
    {
        case Expect::Nothing: return {};
        case Expect::StatusChanged: return { StatusChanged { .run = run, .status = status } };
        case Expect::PaneWentAway: return { PaneWentAway { .run = run } };
        case Expect::ClosePane: return { ClosePane { .pane = std::string(GonePane) } };
        case Expect::CloseAlive: return { ClosePane { .pane = std::string(AlivePane) } };
        case Expect::CloseBoth:
            return { ClosePane { .pane = std::string(AlivePane) },
                     ClosePane { .pane = std::string(GonePane) } };
        case Expect::Adopt: return { Adopt { .run = run, .pane = std::string(AlivePane) } };
        case Expect::AdoptAndStatus:
            return { Adopt { .run = run, .pane = std::string(AlivePane) },
                     StatusChanged { .run = run, .status = status } };
    }
    return {};
}

[[nodiscard]] Run rowRun(ReconcileRow const& row)
{
    return makeRun(std::string(RunIdText), stateOf(row.state), toOptional(row.lastStatus));
}

[[nodiscard]] orchestrator::StepContext context(uint64_t monoMs)
{
    return orchestrator::StepContext { .mainCheckout = MainCheckout, .wallMs = 2, .monoMs = monoMs };
}

/// Carries decisions out the way a driver does: runs are fed through advance(), closed panes stop being
/// listed.
void apply(std::vector<Decision> const& decisions,
           std::vector<Run>& runs,
           std::vector<PaneFact>& panes,
           uint64_t monoMs)
{
    auto const feed = [&](std::string_view id, orchestrator::RunInput const& input) {
        for (auto& run: runs)
        {
            if (run.id == id)
                std::ignore = orchestrator::advance(run, input, context(monoMs));
        }
    };
    for (auto const& decision: decisions)
    {
        std::visit(orchestrator::Overloaded {
                       [&](Adopt const& adopt) {
                           feed(adopt.run, orchestrator::PaneOpened { .pane = adopt.pane, .monoMs = monoMs });
                       },
                       [&](StatusChanged const& changed) {
                           feed(changed.run, orchestrator::StatusReported { .status = changed.status });
                       },
                       [&](ClosePane const& close) {
                           std::erase_if(panes,
                                         [&](PaneFact const& pane) { return pane.pane == close.pane; });
                       },
                       [&](PaneWentAway const& away) { feed(away.run, orchestrator::PaneGone {}); },
                   },
                   decision);
    }
}

/// How many rounds of decisions a row takes before reconciling yields nothing.
[[nodiscard]] std::size_t roundsFor(ReconcileRow const& row)
{
    switch (row.expect)
    {
        case Expect::Nothing: return 0;
        case Expect::ClosePane:
            // The run hears of it once the shell is no longer listed, unless its agent's tab still is.
            return row.listed == Listed::AliveAndGone ? 1 : 2;
        case Expect::CloseAlive:
        case Expect::CloseBoth: return 2;
        case Expect::StatusChanged:
        case Expect::PaneWentAway:
        case Expect::Adopt:
        case Expect::AdoptAndStatus: return 1;
    }
    return 0;
}

/// Whether a PaneWentAway is reported for a run while a pane tagged with it is listed.
[[nodiscard]] bool reportsGoneWhileListed(std::vector<Decision> const& decisions,
                                          std::vector<PaneFact> const& panes)
{
    return std::ranges::any_of(decisions, [&](Decision const& decision) {
        auto const* const away = std::get_if<PaneWentAway>(&decision);
        return away != nullptr
               && std::ranges::any_of(panes, [&](PaneFact const& pane) { return pane.tag == away->run; });
    });
}

} // namespace

TEST_CASE("Reconcile.rules", "[orchestrator][reconcile]")
{
    for (auto const& row: ReconcileRows)
    {
        CAPTURE(row.name);
        auto const runs = std::vector { rowRun(row) };
        auto const panes = panesFor(row.listed, row.paneStatus);
        auto const decisions = orchestrator::reconcile(runs, panes, ListedMs, Tagged);
        CHECK(decisions == expected(row));
        CHECK_FALSE(reportsGoneWhileListed(decisions, panes));
        // The same facts give the same decisions: reconcile keeps no state of its own.
        CHECK(orchestrator::reconcile(runs, panes, ListedMs, Tagged) == decisions);
    }
}

TEST_CASE("Reconcile.idempotence", "[orchestrator][reconcile]")
{
    // Applying the decisions and reconciling again ends in no decision. A tab that must go takes two rounds:
    // it is closed, and only once it is no longer listed is the run told its pane went away. Each round's
    // listing is asked for after the previous round's changes.
    for (auto const& row: ReconcileRows)
    {
        CAPTURE(row.name);
        auto runs = std::vector { rowRun(row) };
        auto panes = panesFor(row.listed, row.paneStatus);
        auto rounds = std::vector<std::vector<Decision>> {};
        auto listedMs = ListedMs;
        for ([[maybe_unused]] auto const round: std::views::iota(0, 4))
        {
            auto decisions = orchestrator::reconcile(runs, panes, listedMs, Tagged);
            CHECK_FALSE(reportsGoneWhileListed(decisions, panes));
            if (decisions.empty())
                break;
            apply(decisions, runs, panes, listedMs + 50);
            rounds.push_back(std::move(decisions));
            listedMs += 100;
        }
        CHECK(orchestrator::reconcile(runs, panes, listedMs, Tagged).empty());
        auto const expectedRounds = roundsFor(row);
        CHECK(rounds.size() == expectedRounds);
        if (rounds.size() == 2)
            CHECK(rounds[1] == std::vector<Decision> { PaneWentAway { .run = std::string(RunIdText) } });
        REQUIRE(runs.size() == 1);
        CHECK(runs.front().id == RunIdText);
    }
}

TEST_CASE("Reconcile.manyRuns", "[orchestrator][reconcile]")
{
    auto const runs = std::vector {
        makeRun("a-1", orchestrator::Running {}, AgentStatus::Working),
        makeRun("b-2", Preparing { .step = PrepareStep::OpenPane }, std::nullopt),
        makeRun("c-3", orchestrator::Done {}, AgentStatus::Done),
        makeRun("d-4", finished(Fate::Removed), AgentStatus::Done),
    };
    auto const panes = std::vector {
        PaneFact { .pane = "p-b", .tag = "b-2", .status = std::nullopt, .program = ProgramLiveness::Alive },
        PaneFact {
            .pane = "p-x", .tag = "x-9", .status = AgentStatus::Blocked, .program = ProgramLiveness::Alive },
        PaneFact {
            .pane = "p-a", .tag = "a-1", .status = AgentStatus::Blocked, .program = ProgramLiveness::Alive },
        PaneFact {
            .pane = "p-d", .tag = "d-4", .status = AgentStatus::Working, .program = ProgramLiveness::Alive },
    };
    auto const decisions = orchestrator::reconcile(runs, panes, ListedMs, Tagged);
    // In run order; the pane tagged with no run and the finished run's pane are left alone.
    CHECK(decisions
          == std::vector<Decision> {
              StatusChanged { .run = "a-1", .status = AgentStatus::Blocked },
              Adopt { .run = "b-2", .pane = "p-b" },
              PaneWentAway { .run = "c-3" },
          });
    CHECK(orchestrator::reconcile({}, panes, ListedMs, Tagged).empty());
}

namespace
{

struct StaleRow
{
    std::string_view name;
    State state;
    Listed listed;
    uint64_t changedMs;
    Expect expect;
};

// clang-format off
constexpr auto StaleRows = std::array {
    // A listing asked for before the tab opened shows no tab: it must not end the run.
    StaleRow { "opened after the listing was asked for", State::Running, Listed::Nothing, ListedMs + 1, Expect::Nothing },
    StaleRow { "opened as the listing was asked for",   State::Running, Listed::Nothing, ListedMs,     Expect::Nothing },
    StaleRow { "opened before",                         State::Running, Listed::Nothing, ListedMs - 1, Expect::PaneWentAway },
    StaleRow { "a status from before a change",         State::Blocked, Listed::Alive,   ListedMs + 1, Expect::Nothing },
    StaleRow { "a status from after it",                State::Blocked, Listed::Alive,   ListedMs - 1, Expect::StatusChanged },
    StaleRow { "closing since after the listing",       State::EndingClosePane, Listed::Nothing, ListedMs + 1, Expect::Nothing },
    StaleRow { "closing since before",                  State::EndingClosePane, Listed::Nothing, ListedMs - 1, Expect::PaneWentAway },
    StaleRow { "opening since after the listing",       State::PreparingOpenPane, Listed::Alive, ListedMs + 1, Expect::Nothing },
    StaleRow { "opening since before",                  State::PreparingOpenPane, Listed::Alive, ListedMs - 1, Expect::AdoptAndStatus },
};
// clang-format on

} // namespace

TEST_CASE("Reconcile.staleListing", "[orchestrator][reconcile]")
{
    for (auto const& row: StaleRows)
    {
        CAPTURE(row.name);
        auto run = makeRun(std::string(RunIdText), stateOf(row.state), AgentStatus::Blocked);
        run.changedMonoMs = row.changedMs;
        auto const runs = std::vector { run };
        auto const panes = panesFor(row.listed, Status::Working);
        auto const want = expected(ReconcileRow { .name = row.name,
                                                  .state = row.state,
                                                  .lastStatus = Status::Blocked,
                                                  .listed = row.listed,
                                                  .paneStatus = Status::Working,
                                                  .expect = row.expect });
        CHECK(orchestrator::reconcile(runs, panes, ListedMs, Tagged) == want);
    }
}

TEST_CASE("Reconcile.tabOpenedDuringListing", "[orchestrator][reconcile]")
{
    // The listing is asked for at 100; the tab opens and the run starts at 120; the listing, answered at 130,
    // cannot show the tab. Only the next listing may end the run.
    auto run = makeRun(std::string(RunIdText), Preparing { .step = PrepareStep::OpenPane }, std::nullopt);
    std::ignore = orchestrator::advance(
        run, orchestrator::PaneOpened { .pane = AlivePane, .monoMs = 120 }, context(120));
    REQUIRE(std::holds_alternative<orchestrator::Running>(run.state));
    auto const runs = std::vector { run };
    CHECK(orchestrator::reconcile(runs, {}, 100, Tagged).empty());
    CHECK(orchestrator::reconcile(runs, panesFor(Listed::Alive, Status::Working), 140, Tagged)
          == std::vector<Decision> {
              StatusChanged { .run = std::string(RunIdText), .status = AgentStatus::Working } });
}

namespace
{

/// Whether a row's run recorded its tab.
enum class Recorded : uint8_t
{
    No,  ///< Its opening failed or was lost: Run::pane is unset.
    Yes, ///< Run::pane names its tab.
};

struct SourceRow
{
    std::string_view name;
    State state;
    Recorded recorded;
    Listed listed;
    PaneListing listing;
    Expect expect;
};

// clang-format off
constexpr auto SourceRows = std::array {
    SourceRow { "tagged, unrecorded tab gone",       State::EndingClosePane, Recorded::No,  Listed::Nothing, PaneListing::Tagged,         Expect::PaneWentAway },
    SourceRow { "tagged, recorded tab gone",         State::EndingClosePane, Recorded::Yes, Listed::Nothing, PaneListing::Tagged,         Expect::PaneWentAway },
    // The driver's own records cannot see a tab whose opening it never recorded: the run waits.
    SourceRow { "own records, unrecorded tab",       State::EndingClosePane, Recorded::No,  Listed::Nothing, PaneListing::OwnRecordsOnly, Expect::Nothing },
    SourceRow { "own records, recorded tab gone",    State::EndingClosePane, Recorded::Yes, Listed::Nothing, PaneListing::OwnRecordsOnly, Expect::PaneWentAway },
    SourceRow { "own records, tab still listed",     State::EndingClosePane, Recorded::Yes, Listed::Alive,   PaneListing::OwnRecordsOnly, Expect::CloseAlive },
    SourceRow { "own records, agent's tab gone",     State::Running,         Recorded::Yes, Listed::Nothing, PaneListing::OwnRecordsOnly, Expect::PaneWentAway },
    SourceRow { "own records, opening, tab listed",  State::PreparingOpenPane, Recorded::No, Listed::Alive,  PaneListing::OwnRecordsOnly, Expect::Adopt },
};
// clang-format on

} // namespace

TEST_CASE("Reconcile.listingSource", "[orchestrator][reconcile]")
{
    for (auto const& row: SourceRows)
    {
        CAPTURE(row.name);
        auto run = makeRun(std::string(RunIdText), stateOf(row.state), std::nullopt);
        run.pane.reset();
        if (row.recorded == Recorded::Yes)
            run.pane = std::string(AlivePane);
        auto const runs = std::vector { run };
        auto const panes = panesFor(row.listed, Status::None);
        auto const want = expected(ReconcileRow { .name = row.name,
                                                  .state = row.state,
                                                  .lastStatus = Status::None,
                                                  .listed = row.listed,
                                                  .paneStatus = Status::None,
                                                  .expect = row.expect });
        CHECK(orchestrator::reconcile(runs, panes, ListedMs, row.listing) == want);
    }
}

// }}}
// {{{ worktreeEntries

namespace
{

constexpr auto Recorded = "git/worktree-list.porcelain-z"sv;
constexpr auto RecordedBare = "git/worktree-list-bare.porcelain-z"sv;

constexpr auto DetachedPath = "/home/u/worktrees/contour/detached-20261113-150847"sv;
constexpr auto ForgedPath = "/home/u/worktrees/contour/forged\nlocked"sv;
constexpr auto LockedPath = "/home/u/worktrees/contour/issue-12-20261113-150847"sv;
constexpr auto PrunablePath = "/home/u/worktrees/contour/issue-17-20261113-150847"sv;
constexpr auto NewLinePath = "/home/u/worktrees/contour/new\nline"sv;
constexpr auto PrPath = "/home/u/worktrees/contour/pr-2334-20261113-150847"sv;

[[nodiscard]] WorktreeEntry checkout(std::string_view path, std::optional<std::string> branch)
{
    return WorktreeEntry { .path = std::string(path),
                           .branch = std::move(branch),
                           .kind = WorktreeKind::Checkout };
}

[[nodiscard]] std::vector<WorktreeEntry> recordedEntries()
{
    return {
        checkout(MainCheckout, "master"),
        checkout("/home/u/my work/contour feature", "feature/x"),
        checkout(DetachedPath, std::nullopt),
        checkout(ForgedPath, "agent/forged"),
        checkout(LockedPath, "agent/issue-12-20261113-150847"),
        checkout(PrunablePath, "agent/issue-17-20261113-150847"),
        checkout(NewLinePath, "agent/new-line"),
        checkout(PrPath, "agent/pr-2334-20261113-150847"),
    };
}

} // namespace

TEST_CASE("Reconcile.worktreeEntries.recorded", "[orchestrator][reconcile]")
{
    // Recorded by harness/r11.sh: line ends in two paths, one forging a `locked` line, and in a lock reason.
    auto const text = readFixture(FixtureRoot::Testdata, Recorded);
    CHECK(text.contains("locked busy now\nreally"sv));
    auto const entries = orchestrator::worktreeEntries(text);
    CHECK(entries == recordedEntries());
    CHECK(orchestrator::mainCheckout(entries) == MainCheckout);

    auto const bare = orchestrator::worktreeEntries(readFixture(FixtureRoot::Testdata, RecordedBare));
    REQUIRE(bare.size() == 2);
    CHECK(bare.front()
          == WorktreeEntry {
              .path = "/home/u/projects/contour.git", .branch = std::nullopt, .kind = WorktreeKind::Bare });
    CHECK(bare.back().branch == "agent/pr-7-20261113-150847");
    CHECK_FALSE(orchestrator::mainCheckout(bare).has_value());
    CHECK_FALSE(orchestrator::mainCheckout({}).has_value());
}

TEST_CASE("Reconcile.worktreeEntries.cutListing", "[orchestrator][reconcile]")
{
    // However the listing is cut, every entry read is one the whole listing has, in the same place: a cut
    // path never becomes an entry of its own.
    auto const text = readFixture(FixtureRoot::Testdata, Recorded);
    auto const whole = orchestrator::worktreeEntries(text);
    REQUIRE(whole.size() == 8);
    for (auto const length: std::views::iota(std::size_t { 0 }, text.size()))
    {
        CAPTURE(length);
        auto const entries = orchestrator::worktreeEntries(std::string_view(text).substr(0, length));
        REQUIRE(entries.size() < whole.size());
        CHECK(std::ranges::equal(entries, whole | std::views::take(entries.size())));
    }
}

namespace
{

/// How many entries a listing yields, and the last one's path.
struct ListingRow
{
    std::string_view name;
    std::string_view text;
    std::size_t count;
    std::string_view lastPath;
};

// Fields end in NUL, entries in an empty field: `"…\0"sv` keeps the NULs.
// clang-format off
constexpr auto ListingRows = std::array {
    ListingRow { "empty",                    ""sv, 0, "" },
    ListingRow { "one entry",                "worktree /r\0HEAD f99237fba641a36fb707af0936fb186b456d6a58\0branch refs/heads/master\0\0"sv, 1, "/r" },
    ListingRow { "unterminated entry",       "worktree /r\0HEAD f99237fba641a36fb707af0936fb186b456d6a58\0branch refs/heads/master\0"sv, 0, "" },
    ListingRow { "cut inside a path",        "worktree /r\0bare\0\0worktree /w/con"sv, 1, "/r" },
    ListingRow { "cut after a path",         "worktree /r\0bare\0\0worktree /w/con\0"sv, 1, "/r" },
    ListingRow { "a path alone",             "worktree /r\0bare\0\0worktree /w\0\0"sv, 1, "/r" },
    ListingRow { "locked without reason",    "worktree /r\0bare\0\0worktree /w/a\0HEAD f99237fba641a36fb707af0936fb186b456d6a58\0branch refs/heads/agent/a\0locked\0prunable\0\0"sv, 2, "/w/a" },
    ListingRow { "bare, locked",             "worktree /r\0bare\0locked\0\0"sv, 1, "/r" },
    ListingRow { "sha-256 head",             "worktree /r\0HEAD f99237fba641a36fb707af0936fb186b456d6a58f99237fba641a36fb707af09\0detached\0\0"sv, 1, "/r" },
    ListingRow { "HEAD alone",               "worktree /r\0HEAD f99237fba641a36fb707af0936fb186b456d6a58\0\0"sv, 1, "/r" },
    // With -z a path or a reason holds any byte but NUL, and forges nothing.
    ListingRow { "a line end in a path",     "worktree /r\0bare\0\0worktree /w/a\nb/c\0HEAD f99237fba641a36fb707af0936fb186b456d6a58\0detached\0\0"sv, 2, "/w/a\nb/c" },
    ListingRow { "a control byte in a path", "worktree /r\0bare\0\0worktree /w/\x1b[31m\0HEAD f99237fba641a36fb707af0936fb186b456d6a58\0detached\0\0"sv, 2, "/w/\x1b[31m" },
    ListingRow { "a forged locked line",     "worktree /r\0bare\0\0worktree /w/x\nlocked\0HEAD f99237fba641a36fb707af0936fb186b456d6a58\0detached\0\0"sv, 2, "/w/x\nlocked" },
    ListingRow { "a reason with a line end", "worktree /r\0bare\0\0worktree /w\0HEAD f99237fba641a36fb707af0936fb186b456d6a58\0detached\0locked a\nworktree /evil\0\0"sv, 2, "/w" },
    ListingRow { "a reason with quotes",     "worktree /r\0bare\0\0worktree /w\0HEAD f99237fba641a36fb707af0936fb186b456d6a58\0detached\0locked busy \"q\"\0prunable gone\0\0"sv, 2, "/w" },
    // Not git's shape: reading stops there, the entries before it kept.
    ListingRow { "a malformed first entry",  "HEAD f99237fba641a36fb707af0936fb186b456d6a58\0worktree /r\0\0"sv, 0, "" },
    ListingRow { "a newline-separated listing", "worktree /r\nbare\n\n"sv, 0, "" },
    ListingRow { "empty path",               "worktree /r\0bare\0\0worktree \0bare\0\0"sv, 1, "/r" },
    ListingRow { "bare word worktree",       "worktree /r\0bare\0\0worktree\0bare\0\0"sv, 1, "/r" },
    ListingRow { "head not hex",             "worktree /r\0bare\0\0worktree /w\0HEAD main\0\0"sv, 1, "/r" },
    ListingRow { "head short",               "worktree /r\0bare\0\0worktree /w\0HEAD f99237f\0\0"sv, 1, "/r" },
    ListingRow { "no blank between entries", "worktree /r\0bare\0worktree /w\0bare\0\0"sv, 0, "" },
    // A field a later git adds is skipped, wherever it stands; the known fields keep their order.
    ListingRow { "an unknown field",         "worktree /r\0bare\0\0worktree /w\0bare\0sparse-checkout yes\0\0"sv, 2, "/w" },
    ListingRow { "unknown between known",    "worktree /r\0bare\0\0worktree /w\0HEAD f99237fba641a36fb707af0936fb186b456d6a58\0shallow\0branch refs/heads/a\0new-field x y\0locked\0\0"sv, 2, "/w" },
    ListingRow { "unknown on every entry",   "worktree /r\0bare\0extra 1\0\0worktree /w\0extra 2\0bare\0\0"sv, 2, "/w" },
    ListingRow { "unknown, no HEAD",         "worktree /r\0bare\0\0worktree /w\0extra\0\0"sv, 1, "/r" },
    ListingRow { "unknown, order still kept", "worktree /r\0bare\0\0worktree /w\0bare\0extra\0HEAD f99237fba641a36fb707af0936fb186b456d6a58\0\0"sv, 1, "/r" },
    ListingRow { "a garbage key",            "worktree /r\0bare\0\0worktree /w\0bare\0Sparse yes\0\0"sv, 1, "/r" },
    ListingRow { "a garbage key, no space",  "worktree /r\0bare\0\0worktree /w\0bare\0b/c\0\0"sv, 1, "/r" },
    ListingRow { "a key starting with -",    "worktree /r\0bare\0\0worktree /w\0bare\0-x\0\0"sv, 1, "/r" },
    ListingRow { "empty branch",             "worktree /r\0bare\0\0worktree /w\0HEAD f99237fba641a36fb707af0936fb186b456d6a58\0branch \0\0"sv, 1, "/r" },
    ListingRow { "branch before HEAD",       "worktree /r\0bare\0\0worktree /w\0branch refs/heads/a\0HEAD f99237fba641a36fb707af0936fb186b456d6a58\0\0"sv, 1, "/r" },
    ListingRow { "two branches",             "worktree /r\0bare\0\0worktree /w\0HEAD f99237fba641a36fb707af0936fb186b456d6a58\0branch refs/heads/a\0branch refs/heads/b\0\0"sv, 1, "/r" },
    ListingRow { "branch and detached",      "worktree /r\0bare\0\0worktree /w\0HEAD f99237fba641a36fb707af0936fb186b456d6a58\0branch refs/heads/a\0detached\0\0"sv, 1, "/r" },
    ListingRow { "bare and HEAD",            "worktree /r\0bare\0\0worktree /w\0bare\0HEAD f99237fba641a36fb707af0936fb186b456d6a58\0\0"sv, 1, "/r" },
    ListingRow { "locked before HEAD",       "worktree /r\0bare\0\0worktree /w\0locked\0HEAD f99237fba641a36fb707af0936fb186b456d6a58\0\0"sv, 1, "/r" },
    ListingRow { "prunable before locked",   "worktree /r\0bare\0\0worktree /w\0bare\0prunable\0locked\0\0"sv, 1, "/r" },
    ListingRow { "locked twice",             "worktree /r\0bare\0\0worktree /w\0bare\0locked\0locked\0\0"sv, 1, "/r" },
    ListingRow { "a value on detached",      "worktree /r\0bare\0\0worktree /w\0HEAD f99237fba641a36fb707af0936fb186b456d6a58\0detached yes\0\0"sv, 1, "/r" },
    ListingRow { "a good entry after a bad", "worktree /r\0bare\0\0worktree /w\0HEAD xyz\0\0worktree /v\0bare\0\0"sv, 1, "/r" },
};
// clang-format on

} // namespace

TEST_CASE("Reconcile.worktreeEntries.listings", "[orchestrator][reconcile]")
{
    for (auto const& row: ListingRows)
    {
        CAPTURE(row.name, row.text);
        auto const entries = orchestrator::worktreeEntries(row.text);
        CHECK(entries.size() == row.count);
        if (!entries.empty())
        {
            CHECK(entries.back().path == row.lastPath);
            CHECK(entries.front().path == "/r");
        }
    }

    auto const outsideHeads = orchestrator::worktreeEntries(
        "worktree /r\0HEAD f99237fba641a36fb707af0936fb186b456d6a58\0branch refs/remotes/origin/x\0\0"sv);
    REQUIRE(outsideHeads.size() == 1);
    CHECK(outsideHeads.front().branch == "refs/remotes/origin/x");
}

TEST_CASE("Reconcile.worktreeEntries.hostileEntryKeepsMainCheckout", "[orchestrator][reconcile]")
{
    // An agent can shape its own worktree's entry, never the main worktree's, which git lists first.
    auto const text = std::string(readFixture(FixtureRoot::Testdata, Recorded))
                      + "worktree /home/u/worktrees/contour/evil\0HEAD nothex\0\0"s;
    auto const entries = orchestrator::worktreeEntries(text);
    CHECK(entries == recordedEntries());
    CHECK(orchestrator::mainCheckout(entries) == MainCheckout);
}

// }}}
// {{{ orphans

namespace
{

constexpr auto WorktreeRoot = "/home/u/worktrees"sv;

/// How a row's run names the pr-2334 worktree.
enum class Names : uint8_t
{
    NoRun,         ///< There is no run.
    PathAndBranch, ///< Its path and its branch.
    BranchOnly,    ///< Its branch; its stored path went through a symlinked root.
    PathOnly,      ///< Its path; it is on another branch.
    OtherRepo,     ///< Both, but the run was filed under another name for the repository.
    Neither,       ///< Another worktree's path and branch.
};

/// Whether the row's worktree root covers the detached worktree, which is on no agent branch.
enum class Covers : uint8_t
{
    No,
    Yes,
};

/// Whether the row's run accounts for the pr-2334 worktree.
enum class Accounted : uint8_t
{
    No,
    Yes,
};

struct OrphanRow
{
    std::string_view name;
    Names names;
    State state;
    std::string_view root;
    Covers covers;
    Accounted accounted;
};

// clang-format off
constexpr auto OrphanRows = std::array {
    OrphanRow { "no run",                    Names::NoRun,         State::Running,           WorktreeRoot,         Covers::Yes, Accounted::No },
    OrphanRow { "running run",               Names::PathAndBranch, State::Running,           WorktreeRoot,         Covers::Yes, Accounted::Yes },
    OrphanRow { "symlinked root",            Names::BranchOnly,    State::Running,           WorktreeRoot,         Covers::Yes, Accounted::Yes },
    OrphanRow { "path only",                 Names::PathOnly,      State::PreparingCheckout, WorktreeRoot,         Covers::Yes, Accounted::Yes },
    OrphanRow { "another name for the repo", Names::OtherRepo,     State::Running,           WorktreeRoot,         Covers::Yes, Accounted::Yes },
    OrphanRow { "another worktree's run",    Names::Neither,       State::Running,           WorktreeRoot,         Covers::Yes, Accounted::No },
    OrphanRow { "preparing run",             Names::PathAndBranch, State::PreparingFetch,    WorktreeRoot,         Covers::Yes, Accounted::Yes },
    OrphanRow { "ending run",                Names::PathAndBranch, State::EndingStatus,      WorktreeRoot,         Covers::Yes, Accounted::Yes },
    OrphanRow { "kept run",                  Names::PathAndBranch, State::FinishedKept,      WorktreeRoot,         Covers::Yes, Accounted::Yes },
    // A guest fault between storing the removal and the command can leave a worktree behind it.
    OrphanRow { "removed run",               Names::PathAndBranch, State::FinishedRemoved,   WorktreeRoot,         Covers::Yes, Accounted::No },
    OrphanRow { "root with a slash",         Names::PathAndBranch, State::Running,           "/home/u/worktrees/", Covers::Yes, Accounted::Yes },
    // Only agent branches count outside the root.
    OrphanRow { "root elsewhere",            Names::NoRun,         State::Running,           "/srv/wt",            Covers::No,  Accounted::No },
    OrphanRow { "root a sibling prefix",     Names::NoRun,         State::Running,           "/home/u/work",       Covers::No,  Accounted::No },
    OrphanRow { "no root",                   Names::NoRun,         State::Running,           "",                   Covers::No,  Accounted::No },
};
// clang-format on

[[nodiscard]] std::vector<Run> orphanRuns(OrphanRow const& row)
{
    if (row.names == Names::NoRun)
        return {};
    auto run = makeRun(std::string(RunIdText), stateOf(row.state), AgentStatus::Done);
    switch (row.names)
    {
        case Names::NoRun:
        case Names::PathAndBranch: break;
        case Names::BranchOnly: run.worktree = "/data/worktrees/contour/" + std::string(RunIdText); break;
        case Names::PathOnly: run.branch = "agent/pr-2334-20261113-150848"; break;
        case Names::OtherRepo: run.repo = "contour-old"; break;
        case Names::Neither:
            run.worktree = "/home/u/worktrees/contour/pr-1-20261113-150847";
            run.branch = "agent/pr-1-20261113-150847";
            break;
    }
    return { run };
}

[[nodiscard]] std::vector<std::string_view> wantedOrphans(OrphanRow const& row)
{
    auto wanted = std::vector<std::string_view> {};
    if (row.covers == Covers::Yes)
        wanted.push_back(DetachedPath);
    for (auto const path: { ForgedPath, LockedPath, PrunablePath, NewLinePath })
        wanted.push_back(path);
    if (row.accounted == Accounted::No)
        wanted.push_back(PrPath);
    return wanted;
}

} // namespace

TEST_CASE("Reconcile.orphans", "[orchestrator][reconcile]")
{
    auto const entries = recordedEntries();
    for (auto const& row: OrphanRows)
    {
        CAPTURE(row.name, row.root);
        auto const runs = orphanRuns(row);
        auto const found = orchestrator::orphans(runs, row.root, entries);
        auto paths = std::vector<std::string_view> {};
        for (auto const& orphan: found)
            paths.emplace_back(orphan.path);
        CHECK(paths == wantedOrphans(row));
    }
}

TEST_CASE("Reconcile.orphans.edges", "[orchestrator][reconcile]")
{
    // The main worktree is never an orphan, even on an agent branch under the root; nothing listed, nothing
    // found.
    auto const entries = std::vector<WorktreeEntry> {
        checkout("/home/u/worktrees/contour/main", "agent/x"),
        checkout("/elsewhere/a", "agent/y"),
        checkout("/elsewhere/b", "agentx/y"),
        checkout("/elsewhere/c", "agent/"),
        checkout("/home/u/worktrees", "feature"),
    };
    auto const found = orchestrator::orphans({}, WorktreeRoot, entries);
    REQUIRE(found.size() == 1);
    CHECK(found.front().path == "/elsewhere/a");
    CHECK(orchestrator::orphans({}, WorktreeRoot, {}).empty());
}

TEST_CASE("Reconcile.orphanText", "[orchestrator][reconcile]")
{
    CHECK(orchestrator::orphanText(checkout("/w/a b", "agent/x")) == "/w/a b on agent/x");
    CHECK(orchestrator::orphanText(checkout("/w/d", std::nullopt)) == "/w/d on a detached HEAD");
    CHECK(orchestrator::orphanText(checkout(NewLinePath, "agent/new-line"))
          == "/home/u/worktrees/contour/new line on agent/new-line");
    // U+202E RIGHT-TO-LEFT OVERRIDE, spelled byte by byte.
    constexpr auto RightToLeftOverride = std::array { '\xe2', '\x80', '\xae' };
    auto const branch = "agent/" + std::string(RightToLeftOverride.data(), RightToLeftOverride.size()) + "x";
    CHECK(orchestrator::orphanText(checkout("/w/\x1b[31mred", branch)) == "/w/[31mred on agent/x");
}

// }}}
