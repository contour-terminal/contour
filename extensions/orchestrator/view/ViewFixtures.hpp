// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <orchestrator/core/Engine.hpp>
#include <orchestrator/core/Model.hpp>
#include <orchestrator/core/Run.hpp>

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
};

/// @param world What the engine is started from.
/// @return The engine, started and with every poll command answered.
[[nodiscard]] Engine bootEngine(World const& world);

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

/// Compares a screen with its golden file `view/testdata/<name>.screen`. With ORCHESTRATOR_UPDATE_GOLDENS set
/// in the environment, (re)writes the file instead.
/// @param name The golden's name.
/// @param screen The screen, as canvasToString() prints it.
void checkGolden(std::string_view name, std::string const& screen);

} // namespace orchestrator::view::test
