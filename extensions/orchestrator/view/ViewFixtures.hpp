// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/Clock.hpp>
#include <core/tui/Rect.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
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
        "contour/setup-hooks-present.json";    ///< What `agent-setup-check` prints, under testdata.
    std::vector<WorktreeEntry> worktrees = {}; ///< Listed in the contour repository after its checkout.
    std::vector<std::string> held = {}; ///< Commands left unanswered until DashboardFixture::answerHeld().
};

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
