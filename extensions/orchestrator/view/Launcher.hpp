// SPDX-License-Identifier: Apache-2.0
//
// The launcher (`n`): one popup that browses the pull requests and issues and starts an agent on one of them,
// or on a topic typed into it.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <orchestrator/core/Engine.hpp>
#include <orchestrator/core/Model.hpp>

namespace orchestrator::view
{

/// Where a candidate is listed, in the order the groups are shown.
enum class CandidateGroup : uint8_t
{
    Topic = 0,      ///< A topic on what was typed.
    NeedsAttention, ///< One of your pull requests whose checks fail or whose reviewer asked for changes.
    MyPullRequests, ///< One of your other pull requests.
    Issues,         ///< An issue without a pull request, from the forge or Jira.
};

/// One row the launcher offers: an item the engine lists, or a topic on the query.
struct Candidate
{
    CandidateGroup group = CandidateGroup::Topic; ///< Where it is listed.
    std::string repo = {};                        ///< Its repository; empty for the topic row.
    ItemRef item = {};                            ///< The item; a TopicRef for the topic row.
    std::string url = {};                         ///< Its web page; empty for the topic row.
    std::string title = {};                       ///< Its title, sanitised; the topic row's: the query.
    CiState ci = CiState::None;                   ///< A pull request's checks.
    ReviewState review = ReviewState::None;       ///< A pull request's review.
    std::vector<std::string> failingChecks = {};  ///< The names of its failing checks.
    std::string activeRun = {};                   ///< The id of an active run on the item; empty: none.
    [[nodiscard]] bool operator==(Candidate const&) const = default;
};

/// What the launcher lists for @p query.
/// @param engine Its lists and runs.
/// @param query What was typed; blanks around it are ignored.
/// @return With an empty query, every item: those that need attention, then the other pull requests, then
///         the issues, each in the engine's order. Otherwise the items whose `#<number> <title>` (or Jira key
///         and title) holds the query, case folded: one the query names exactly (its number, `#number`, URL
///         or Jira key) first, then the topic row on the query, then the rest in group order.
[[nodiscard]] std::vector<Candidate> candidatesOf(Engine const& engine, std::string_view query);

} // namespace orchestrator::view
