// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/view/Launcher.hpp>

#include <algorithm>
#include <cctype>
#include <format>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include <orchestrator/view/Sections.hpp>

namespace orchestrator::view
{

namespace
{
    /// @return @p text with ASCII letters lower-cased: titles are matched as typed, whatever their case.
    [[nodiscard]] std::string folded(std::string_view text)
    {
        return text | std::views::transform([](char c) {
                   return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
               })
               | std::ranges::to<std::string>();
    }

    /// @return @p text without the blanks around it.
    [[nodiscard]] std::string_view trimmed(std::string_view text) noexcept
    {
        auto const isBlank = [](char c) {
            return c == ' ' || c == '\t';
        };
        while (!text.empty() && isBlank(text.front()))
            text.remove_prefix(1);
        while (!text.empty() && isBlank(text.back()))
            text.remove_suffix(1);
        return text;
    }

    [[nodiscard]] bool needsAttention(ChangeRequest const& request) noexcept
    {
        return request.ci == CiState::Failing || request.review == ReviewState::ChangesRequested;
    }

    /// @return Every item the engine lists, in group order and, within a group, the engine's.
    [[nodiscard]] std::vector<Candidate> itemsOf(Engine const& engine)
    {
        auto items = std::vector<Candidate> {};
        auto const activeRunOf = [&](std::string_view repo, ItemRef const& item) {
            auto const* const run = engine.activeRunOn(repo, item);
            return run != nullptr ? run->id : std::string {};
        };
        for (auto const& repo: engine.repositories())
        {
            for (auto const& request: engine.changeRequests(repo))
            {
                auto const item = ItemRef { ChangeRequestRef { request.number } };
                items.push_back(Candidate { .group = needsAttention(request) ? CandidateGroup::NeedsAttention
                                                                             : CandidateGroup::MyPullRequests,
                                            .repo = repo,
                                            .item = item,
                                            .url = request.url,
                                            .title = request.title,
                                            .ci = request.ci,
                                            .review = request.review,
                                            .failingChecks = request.failingChecks,
                                            .activeRun = activeRunOf(repo, item) });
            }
            for (auto const& issue: engine.unclaimedIssues(repo))
                items.push_back(Candidate { .group = CandidateGroup::Issues,
                                            .repo = repo,
                                            .item = issue.item,
                                            .url = issue.url,
                                            .title = issue.title,
                                            .activeRun = activeRunOf(repo, issue.item) });
        }
        std::ranges::stable_sort(items, {}, &Candidate::group);
        return items;
    }

    /// @return Whether @p query names @p candidate exactly: its number, `#number`, URL or Jira key.
    [[nodiscard]] bool namesExactly(Candidate const& candidate, std::string_view query)
    {
        auto const label = itemLabel(candidate.item);
        return query == candidate.url || folded(query) == folded(label)
               || (label.starts_with('#') && query == std::string_view { label }.substr(1));
    }
} // namespace

std::vector<Candidate> candidatesOf(Engine const& engine, std::string_view query)
{
    auto items = itemsOf(engine);
    auto const typed = trimmed(query);
    if (typed.empty())
        return items;

    auto const needle = folded(typed);
    auto exact = std::vector<Candidate> {};
    auto matching = std::vector<Candidate> {};
    for (auto& candidate: items)
    {
        if (namesExactly(candidate, typed))
            exact.push_back(std::move(candidate));
        else if (folded(std::format("{} {}", itemLabel(candidate.item), candidate.title)).contains(needle))
            matching.push_back(std::move(candidate));
    }
    auto candidates = std::move(exact);
    candidates.push_back(
        Candidate { .group = CandidateGroup::Topic, .item = TopicRef {}, .title = std::string { typed } });
    std::ranges::move(matching, std::back_inserter(candidates));
    return candidates;
}

} // namespace orchestrator::view
