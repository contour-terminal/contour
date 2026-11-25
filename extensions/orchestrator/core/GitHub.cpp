// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/core/GitHub.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glaze/glaze.hpp>
#include <orchestrator/core/ArgumentKinds.hpp>
#include <orchestrator/core/GitHubJson.hpp>
#include <orchestrator/core/Json.hpp>
#include <orchestrator/core/Model.hpp>
#include <orchestrator/core/Sanitize.hpp>
#include <orchestrator/core/Spelling.hpp>
#include <orchestrator/core/Timestamps.hpp>

using namespace std::string_view_literals;

// The `gh-prs` rows are positional (the command's `--jq` filter, Commands.cpp): each table lists the members
// in the order the filter prints them. Glaze finds a table by the spelling `value`, as the standard library
// finds `value_type`.

/// A `statusCheckRollup` entry: `[__typename, name, context, status, conclusion, state]`.
template <>
struct glz::meta<orchestrator::github::detail::JsonCheck>
{
    using T = orchestrator::github::detail::JsonCheck;
    // NOLINTNEXTLINE(readability-identifier-naming)
    static constexpr auto value =
        glz::array(&T::typeName, &T::name, &T::context, &T::status, &T::conclusion, &T::state);
};

/// A `closingIssuesReferences` entry: `[number, url]`.
template <>
struct glz::meta<orchestrator::github::detail::JsonIssueReference>
{
    using T = orchestrator::github::detail::JsonIssueReference;
    // NOLINTNEXTLINE(readability-identifier-naming)
    static constexpr auto value = glz::array(&T::number, &T::url);
};

/// A pull request.
template <>
struct glz::meta<orchestrator::github::detail::JsonPullRequest>
{
    using T = orchestrator::github::detail::JsonPullRequest;
    // NOLINTNEXTLINE(readability-identifier-naming)
    static constexpr auto value = glz::array(&T::number,
                                             &T::title,
                                             &T::url,
                                             &T::headRefName,
                                             &T::baseRefName,
                                             &T::isDraft,
                                             &T::isCrossRepository,
                                             &T::reviewDecision,
                                             &T::closingIssuesReferences,
                                             &T::statusCheckRollup,
                                             &T::mergeable,
                                             &T::updatedAt);
};

namespace orchestrator::github
{

namespace
{
    using detail::JsonCheck;
    using detail::JsonIssue;
    using detail::JsonPullRequest;
    using detail::JsonRepository;

    /// The input is a string_view: nothing terminates it, and a newer `gh` may add keys.
    constexpr auto ReadOptions = glz::opts { .null_terminated = false, .error_on_unknown_keys = false };

    /// Reads @p text as one document of type @p T.
    template <typename T>
    std::expected<T, ParseError> readJson(std::string_view text)
    {
        return json::readDocument<ReadOptions, T>(text);
    }

    /// The results of a completed CheckRun this mapping knows; any other is Pending.
    constexpr auto CheckRunConclusions = std::array {
        Spelling { .text = "SUCCESS"sv, .meaning = CiState::Passing },
        Spelling { .text = "SKIPPED"sv, .meaning = CiState::Passing },
        Spelling { .text = "NEUTRAL"sv, .meaning = CiState::Passing },
        Spelling { .text = "FAILURE"sv, .meaning = CiState::Failing },
        Spelling { .text = "TIMED_OUT"sv, .meaning = CiState::Failing },
        Spelling { .text = "CANCELLED"sv, .meaning = CiState::Failing },
        Spelling { .text = "ACTION_REQUIRED"sv, .meaning = CiState::Failing },
        Spelling { .text = "STARTUP_FAILURE"sv, .meaning = CiState::Failing },
    };

    /// The states of a StatusContext this mapping knows; any other (PENDING, EXPECTED) is Pending.
    constexpr auto StatusContextStates = std::array {
        Spelling { .text = "SUCCESS"sv, .meaning = CiState::Passing },
        Spelling { .text = "FAILURE"sv, .meaning = CiState::Failing },
        Spelling { .text = "ERROR"sv, .meaning = CiState::Failing },
    };

    constexpr auto ReviewDecisions = std::array {
        Spelling { .text = ""sv, .meaning = ReviewState::None },
        Spelling { .text = "REVIEW_REQUIRED"sv, .meaning = ReviewState::Required },
        Spelling { .text = "APPROVED"sv, .meaning = ReviewState::Approved },
        Spelling { .text = "CHANGES_REQUESTED"sv, .meaning = ReviewState::ChangesRequested },
    };

    /// @return How much @p state weighs when checks are taken together: the heaviest one wins.
    constexpr int weightOf(CiState state) noexcept
    {
        switch (state)
        {
            case CiState::None: return 0;
            case CiState::Passing: return 1;
            case CiState::Pending: return 2;
            case CiState::Failing: return 3;
        }
        return 2;
    }

    /// @return @p text; empty for a key the projection printed as `null`.
    std::string_view textOf(std::optional<std::string> const& text) noexcept
    {
        return text ? std::string_view { *text } : std::string_view {};
    }

    /// @return @p json as a Check whose views point into it.
    Check checkOf(JsonCheck const& json) noexcept
    {
        auto const name = textOf(json.name);
        return Check { .typeName = json.typeName,
                       .status = textOf(json.status),
                       .conclusion = textOf(json.conclusion),
                       .state = textOf(json.state),
                       .name = name.empty() ? textOf(json.context) : name };
    }

    /// Reads a list command's output: a whole one as it is, a cut one as the entries before the cut.
    template <typename T>
    std::expected<json::ReadList<T>, ParseError> readList(std::string_view text, OutputExtent extent)
    {
        return json::readList<ReadOptions, T>(text, extent);
    }

    /// @return The repository part of a forge URL, `https://<host>/<owner>/<repository>/`; empty if @p url
    ///         has no such part.
    constexpr std::string_view repositoryOf(std::string_view url) noexcept
    {
        constexpr auto Scheme = "https://"sv;
        constexpr auto Segments = 3; // host, owner, repository
        if (!url.starts_with(Scheme))
            return {};
        auto end = Scheme.size();
        for ([[maybe_unused]] auto const segment: std::views::iota(0, Segments))
        {
            auto const slash = url.find('/', end);
            if (slash == std::string_view::npos || slash == end)
                return {};
            end = slash + 1;
        }
        return url.substr(0, end);
    }

    /// @return @p json in the model.
    ChangeRequest changeRequestOf(JsonPullRequest const& json)
    {
        auto checks = std::vector<Check> {};
        checks.reserve(json.statusCheckRollup.size());
        auto failingChecks = std::vector<std::string> {};
        for (auto const& entry: json.statusCheckRollup)
        {
            auto const check = checkOf(entry);
            if (checkStateOf(check) == CiState::Failing)
                failingChecks.push_back(displayText(check.name, MaxTitleChars));
            checks.push_back(check);
        }
        // A pull request may close an issue of another repository ("Fixes other/repo#5"); that one's number
        // says nothing about this repository's issue of the same number.
        auto const repository = repositoryOf(json.url);
        auto closesIssues = std::vector<uint64_t> {};
        closesIssues.reserve(json.closingIssuesReferences.size());
        for (auto const& reference: json.closingIssuesReferences)
            if (!repository.empty() && reference.url && repositoryOf(*reference.url) == repository)
                closesIssues.push_back(reference.number);

        return ChangeRequest {
            .number = json.number,
            .title = displayText(json.title, MaxTitleChars),
            .url = json.url,
            .head = json.headRefName,
            .base = json.baseRefName,
            .readiness = json.isDraft ? Readiness::Draft : Readiness::Ready,
            .origin = json.isCrossRepository ? HeadOrigin::Fork : HeadOrigin::SameRepository,
            .ci = ciStateOf(checks),
            .failingChecks = std::move(failingChecks),
            .review = reviewStateOf(json.reviewDecision),
            .closesIssues = std::move(closesIssues),
            .conflicts = json.mergeable == "CONFLICTING" ? Conflicts::Conflicting : Conflicts::None,
            .updatedWallMs = unixMsOf(json.updatedAt).value_or(0),
        };
    }
} // namespace

CiState checkStateOf(Check const& check) noexcept
{
    if (check.typeName == "StatusContext")
        return meaningOf(StatusContextStates, check.state, CiState::Pending);
    if (check.typeName == "CheckRun" && check.status == "COMPLETED")
        return meaningOf(CheckRunConclusions, check.conclusion, CiState::Pending);
    return CiState::Pending;
}

CiState ciStateOf(std::span<Check const> checks) noexcept
{
    auto result = CiState::None;
    for (auto const& check: checks)
        if (auto const state = checkStateOf(check); weightOf(state) > weightOf(result))
            result = state;
    return result;
}

ReviewState reviewStateOf(std::string_view reviewDecision) noexcept
{
    return meaningOf(ReviewDecisions, reviewDecision, ReviewState::Required);
}

std::expected<Listed<ChangeRequest>, ParseError> changeRequests(std::string_view stdoutText,
                                                                OutputExtent extent)
{
    return readList<JsonPullRequest>(stdoutText, extent).transform([](auto const& read) {
        auto listed = Listed<ChangeRequest> { .items = {}, .skipped = 0, .completeness = read.completeness };
        for (auto const& entry: read.entries)
        {
            // The base reaches a command line (`git worktree add … {base}`): one that is no Ref is left out.
            if (entry.number == 0 || !argumentAccepts(ArgumentKind::Ref, entry.baseRefName))
                ++listed.skipped;
            else
                listed.items.push_back(changeRequestOf(entry));
        }
        return listed;
    });
}

std::expected<Listed<Issue>, ParseError> issues(std::string_view stdoutText, OutputExtent extent)
{
    return readList<JsonIssue>(stdoutText, extent).transform([](auto const& read) {
        auto listed = Listed<Issue> { .items = {}, .skipped = 0, .completeness = read.completeness };
        for (auto const& entry: read.entries)
        {
            if (entry.number == 0)
                ++listed.skipped;
            else
                listed.items.push_back(Issue { .item = IssueRef { .number = entry.number },
                                               .title = displayText(entry.title, MaxTitleChars),
                                               .url = entry.url });
        }
        return listed;
    });
}

std::expected<std::string, ParseError> defaultBranch(std::string_view stdoutText)
{
    return readJson<JsonRepository>(stdoutText)
        .and_then([](JsonRepository repository) -> std::expected<std::string, ParseError> {
            if (!argumentAccepts(ArgumentKind::Ref, repository.defaultBranchRef.name))
                return std::unexpected(ParseError::WrongShape);
            return std::move(repository.defaultBranchRef.name);
        });
}

} // namespace orchestrator::github
