// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <ranges>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <orchestrator/core/Engine.hpp>
#include <orchestrator/view/Launcher.hpp>
#include <orchestrator/view/Sections.hpp>
#include <orchestrator/view/ViewFixtures.hpp>

using namespace std::string_view_literals;
using namespace orchestrator;
using namespace orchestrator::view;
using namespace orchestrator::view::test;

namespace
{
[[nodiscard]] std::vector<std::string> labelsOf(std::vector<Candidate> const& candidates)
{
    return candidates | std::views::transform([](Candidate const& candidate) {
               return std::holds_alternative<TopicRef>(candidate.item) ? "topic:" + candidate.title
                                                                       : itemLabel(candidate.item);
           })
           | std::ranges::to<std::vector>();
}
} // namespace

TEST_CASE("the launcher lists what needs attention first, then the pull requests, then the issues",
          "[orchestrator][view][launcher]")
{
    auto const engine = bootEngine(World {});
    auto const candidates = candidatesOf(engine, "");
    auto const requests = engine.changeRequests("contour");
    auto const issues = engine.unclaimedIssues("contour");
    REQUIRE(candidates.size() == requests.size() + issues.size());
    CHECK(std::ranges::is_sorted(candidates, {}, &Candidate::group));
    CHECK(std::ranges::none_of(candidates,
                               [](Candidate const& c) { return c.group == CandidateGroup::Topic; }));
    for (auto const& candidate: candidates)
    {
        INFO(itemLabel(candidate.item));
        auto const needs =
            candidate.ci == CiState::Failing || candidate.review == ReviewState::ChangesRequested;
        if (std::holds_alternative<ChangeRequestRef>(candidate.item))
            CHECK(candidate.group
                  == (needs ? CandidateGroup::NeedsAttention : CandidateGroup::MyPullRequests));
        else
            CHECK(candidate.group == CandidateGroup::Issues);
        CHECK(candidate.repo == "contour");
        CHECK_FALSE(candidate.url.empty());
    }
    // The fixture's two pull requests with failing checks need attention.
    CHECK((labelsOf(candidates) | std::views::take(2) | std::ranges::to<std::vector>())
          == std::vector<std::string> { "#2114", "#1847" });
    // Blanks alone are no query.
    CHECK(candidatesOf(engine, "   ") == candidates);
}

TEST_CASE("a query filters the items, and the topic row stands first unless an item is named exactly",
          "[orchestrator][view][launcher]")
{
    auto const engine = bootEngine(World {});

    auto const flaky = candidatesOf(engine, "FLAKY");
    REQUIRE_FALSE(flaky.empty());
    CHECK(flaky.front().group == CandidateGroup::Topic);
    CHECK(flaky.front().title == "FLAKY");
    CHECK(labelsOf(flaky) == std::vector<std::string> { "topic:FLAKY", "#2338" });

    for (auto const query: { "2375"sv, "#2375"sv, "https://github.com/contour-terminal/contour/pull/2375"sv })
    {
        INFO(query);
        auto const named = candidatesOf(engine, query);
        REQUIRE(named.size() >= 2);
        CHECK(itemLabel(named.front().item) == "#2375");
        CHECK(named[1].group == CandidateGroup::Topic);
    }

    // Nothing matches: the topic row alone.
    CHECK(labelsOf(candidatesOf(engine, " zzz-nothing "))
          == std::vector<std::string> { "topic:zzz-nothing" });
}

TEST_CASE("an item an agent works on says which run", "[orchestrator][view][launcher]")
{
    auto const engine = bootEngine(
        World { .stored = { storedRun(Running {}, "pr-2375-20261113-150847", ChangeRequestRef { 2375 }) } });
    auto const candidates = candidatesOf(engine, "#2375");
    REQUIRE_FALSE(candidates.empty());
    CHECK(candidates.front().activeRun == "pr-2375-20261113-150847");
    CHECK(std::ranges::all_of(candidates | std::views::drop(1),
                              [](Candidate const& c) { return c.activeRun.empty(); }));
}
