// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/Clock.hpp>
#include <core/tui/Rect.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <orchestrator/core/Engine.hpp>
#include <orchestrator/core/Model.hpp>
#include <orchestrator/core/Reconcile.hpp>
#include <orchestrator/core/Run.hpp>
#include <orchestrator/core/Uuid.hpp>
#include <orchestrator/view/Dashboard.hpp>

namespace orchestrator::view::test
{

using namespace std::string_view_literals;

/// The worktree root of the world the view tests run in.
inline constexpr auto WorktreeRoot = "/home/u/worktrees"sv;
/// The contour repository's checkout.
inline constexpr auto ContourPath = "/home/u/projects/contour"sv;
/// A second repository's checkout.
inline constexpr auto OtherPath = "/home/u/projects/other"sv;
/// When the engine starts, in monotonic milliseconds.
inline constexpr auto StartMonoMs = uint64_t { 1'000 };
/// 2026-11-13 15:08:47 UTC: when the runs were dispatched.
inline constexpr auto DispatchWallMs = uint64_t { 1'794'582'527'000 };
/// The contour repository alone.
inline constexpr auto OneRepository =
    R"([ { "name": "contour", "path": "~/projects/contour", "forge": "github",
           "issues": [ { "source": "forge", "query": "assignee:@me" } ] } ])"sv;

/// How a list command answers.
struct ListAnswer
{
    std::string fixture = {}; ///< Recorded output under core/testdata/; empty: it fails.
    std::optional<std::string> text = std::nullopt; ///< What it prints instead of @ref fixture.
};

/// How one command answers, whatever World's own fields say: the first whose command and argument match.
struct CommandAnswer
{
    std::string command = {};          ///< The declared command, such as `acli-issues`.
    std::string argumentContains = {}; ///< Some argument's value holds it; empty: any.
    ListAnswer answer = {};            ///< What it prints.
};

/// What the engine a test draws is started from.
struct World
{
    std::optional<std::string> configJson = std::nullopt; ///< The whole configuration; absent: the default
                                                          ///< one with @ref repos.
    std::string repos = std::string { OneRepository };    ///< The `repos` array.
    std::vector<std::string> repoPaths = { std::string { ContourPath } }; ///< Each repository, resolved.
    ListAnswer prs = { "github/prs-open.json" };                          ///< What `gh-prs` prints.
    ListAnswer issues = { "github/issues-open.json" };                    ///< What `gh-issues` prints.
    std::vector<Run> stored = {}; ///< The runs in the store; the active ones have a live tab listed.
    std::string hooks =
        "contour/setup-hooks-present.json"; ///< What `agent-setup-check` prints, under testdata.
    std::map<std::string, std::string, std::less<>>
        hooksBySet = {}; ///< By variable set: what its `agent-setup-check` prints instead of @ref hooks.
    std::string plugins = "claude/plugin-list.json"; ///< What `claude-plugins` prints, under testdata.
    std::vector<WorktreeEntry> worktrees = {};       ///< Listed in the contour repository after its checkout.
    std::vector<std::string> held = {}; ///< Commands left unanswered until DashboardFixture::answerHeld().
    std::vector<HostProgram> selfPrograms = {}; ///< What the host runs as its own executable.
    std::map<std::string, std::string, std::less<>> paneMessages = {}; ///< By run id: the reason line its tab
                                                                       ///< lists with its status.
    std::vector<CommandAnswer> answers = {}; ///< Consulted before every other field.
};

/// The contour repository with a Jira source beside its forge's.
inline constexpr auto WorkRepository =
    R"j([ { "name": "contour", "path": "~/projects/contour", "forge": "github",
           "issues": [ { "source": "forge", "query": "assignee:@me" },
                       { "source": "jira", "jql": "assignee = currentUser()",
                         "site": "https://example.atlassian.net" } ] } ])j"sv;

/// The id of the run the Work world keeps on #2376.
inline constexpr auto WorkRunId = "pr-2376-20261113-150847"sv;

/// @return The Work board's world: WorkRepository, its pull requests one per state (`github/prs-work.json`),
///         its Jira tickets (`jira/issues-work.json`) and the key lookup's (`jira/issues-lookup-work.json`),
///         and a run working on #2376.
[[nodiscard]] World workWorld();

/// @param world What the engine is started from.
/// @return The engine, started and with every poll command answered.
[[nodiscard]] Engine bootEngine(World const& world);

/// @param world What the engine is started from.
/// @param held Receives the commands World::held names, unanswered.
/// @return The engine, started and with every other poll command answered.
[[nodiscard]] Engine bootEngine(World const& world, std::vector<RunCommand>& held);

/// A run as the store would hold it.
/// @param state Where it stands.
/// @param id Its id, such as `pr-2375-20261113-150847`.
/// @param item What it works on.
/// @param task Its task's id.
/// @return The run, of the contour repository.
[[nodiscard]] Run storedRun(RunState state,
                            std::string_view id,
                            ItemRef item,
                            std::string_view task = "fix_ci");

/// A run moved outside the configuration: of a repository the configuration no longer names.
/// @param run A run, such as storedRun() makes.
/// @return @p run, of the repository `retired`, with its checkout and worktree named after it.
[[nodiscard]] Run outsideConfiguration(Run run);

/// UUIDs that count: the n-th next() holds n in its fourth byte, every other byte zero.
class CountingUuids final: public IUuidSource
{
  public:
    [[nodiscard]] std::array<uint8_t, 16> next() override
    {
        auto bytes = std::array<uint8_t, 16> {};
        bytes[3] = static_cast<uint8_t>(++taken);
        return bytes;
    }

    std::size_t taken = 0; ///< How many were taken.
};

/// What the guest answers the reads a dashboard asks for.
struct Answers
{
    std::optional<ProcessOutcome> transcript = std::nullopt; ///< A transcript page's; none: left unanswered.
    std::optional<std::expected<std::string, FileReadError>> board =
        std::nullopt;                 ///< A board's; none: left unanswered.
    uint64_t boardModifiedWallMs = 0; ///< The board's modification time.
};

/// @return What the verb printed: @p stdoutText, exit 0.
[[nodiscard]] ProcessOutcome printed(std::string stdoutText);

/// @return What a failed verb printed: @p stderrText, exit 1.
[[nodiscard]] ProcessOutcome failedWith(std::string stderrText);

/// A dashboard over an engine, with the clock it reads and its source of session ids, served in tab Tab.
struct DashboardFixture
{
    /// The tab the dashboard is served in.
    static constexpr auto Tab = std::string_view { "tab-1" };

    /// @param world What the engine is started from.
    /// @param size The tab's size.
    explicit DashboardFixture(World const& world, core::tui::Size size = { .width = 120, .height = 40 });

    /// Lets a held ESC wait past its deadline, and draws: it is then the Escape key.
    /// @return What the Escape key asks, as the guest takes it (Dashboard::dueActions()).
    std::vector<UiAction> escapeTimesOut();

    /// Draws a frame and returns the screen as text.
    [[nodiscard]] std::string screen();

    /// Carries out what the dashboard asked, as the guest does: a transcript page read, a board shown or
    /// hidden, a fold toggled, and the reads they lead to answered as @p answers says (a statistics read
    /// fails); then tells the dashboard the engine changed.
    /// @return The `agent-transcript` commands the engine ran.
    std::vector<RunCommand> carryOut(std::vector<UiAction> const& actions, Answers const& answers = {});

    /// The guest's turn before a frame: what the dashboard asks with no key typed (Dashboard::dueActions()),
    /// carried out.
    /// @return The `agent-transcript` commands the engine ran.
    std::vector<RunCommand> settle(Answers const& answers = {});

    /// @return Now on the fixture's clock, in the engine's monotonic milliseconds.
    [[nodiscard]] uint64_t monoMs() const;

    /// Answers the commands World::held kept back, and what they lead to, as the world says; then tells
    /// the dashboard the engine changed.
    void answerHeld();

    World world;                  ///< What the engine was started from.
    std::vector<RunCommand> held; ///< The commands not answered yet.
    Engine engine;
    core::ManualClock clock;
    CountingUuids ids;
    Dashboard dashboard;
};

/// Compares a screen with its golden file `view/testdata/<name>.screen`. With ORCHESTRATOR_UPDATE_GOLDENS set
/// in the environment, (re)writes the file instead.
/// @param name The golden's name.
/// @param screen The screen, as canvasToString() prints it.
void checkGolden(std::string_view name, std::string const& screen);

} // namespace orchestrator::view::test
