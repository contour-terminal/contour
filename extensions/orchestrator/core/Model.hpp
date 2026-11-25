// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace orchestrator
{

/// The forges a provider row can stand for.
enum class Forge : uint8_t
{
    GitHub, ///< github.com, through `gh`.
    GitLab, ///< A GitLab instance, through `glab`.
};

/// A forge and its name, as the configuration and the store spell it.
struct ForgeSpelling
{
    Forge forge;           ///< The forge.
    std::string_view name; ///< Its name: `forge:` in the configuration, `forge` in a stored run.
};

/// Every forge's name, one row each.
inline constexpr auto ForgeSpellings = std::array {
    ForgeSpelling { .forge = Forge::GitHub, .name = "github" },
    ForgeSpelling { .forge = Forge::GitLab, .name = "gitlab" },
};

/// @return The forge named @p name; std::nullopt for a name no row spells.
[[nodiscard]] constexpr std::optional<Forge> forgeNamed(std::string_view name) noexcept
{
    auto const row = std::ranges::find(ForgeSpellings, name, &ForgeSpelling::name);
    return row != ForgeSpellings.end() ? std::optional { row->forge } : std::nullopt;
}

/// @return The name of @p forge.
[[nodiscard]] constexpr std::string_view forgeName(Forge forge) noexcept
{
    auto const row = std::ranges::find(ForgeSpellings, forge, &ForgeSpelling::forge);
    return row != ForgeSpellings.end() ? row->name : std::string_view {};
}

/// What a change request's checks say, taken together.
enum class CiState : uint8_t
{
    None,    ///< It has no checks.
    Pending, ///< Some check has not finished, or reported a result this mapping does not know.
    Passing, ///< Every check finished and none failed.
    Failing, ///< Some check failed.
};

/// Where a change request stands with its reviewers.
enum class ReviewState : uint8_t
{
    None,             ///< The repository asks for no review.
    Required,         ///< A review is required and has not approved it.
    Approved,         ///< Approved.
    ChangesRequested, ///< A reviewer asked for changes.
};

/// Whether a change request conflicts with its base.
enum class Conflicts : uint8_t
{
    None = 0,    ///< It merges cleanly, or the forge has not yet said.
    Conflicting, ///< It conflicts with its base.
};

/// Where a change request's CI and review state come from.
enum class StateSource : uint8_t
{
    Complete = 0, ///< The forge reported them directly.
    Inferred,     ///< Inferred from GitLab's single `detailed_merge_status` (no matched pipeline).
};

/// Whether a change request is open for review.
enum class Readiness : uint8_t
{
    Ready, ///< Ready for review.
    Draft, ///< A draft.
};

/// Where a change request's head branch lives.
enum class HeadOrigin : uint8_t
{
    SameRepository, ///< In the repository the change request is against.
    Fork,           ///< In another repository.
};

/// A change request (a pull or merge request), by number.
struct ChangeRequestRef
{
    uint64_t number = 0; ///< Its number in its repository.
    [[nodiscard]] bool operator==(ChangeRequestRef const&) const = default;
};

/// A forge issue, by number.
struct IssueRef
{
    uint64_t number = 0; ///< Its number in its repository.
    [[nodiscard]] bool operator==(IssueRef const&) const = default;
};

/// A Jira issue, by key.
struct JiraRef
{
    std::string key; ///< Such as `ABC-123`.
    [[nodiscard]] bool operator==(JiraRef const&) const = default;
};

/// A topic the user typed, by the slug its text makes (topicSlug()); the text itself is Run::topic.
struct TopicRef
{
    std::string slug; ///< `[a-z0-9-]`, at most MaxTopicSlugChars, neither leading nor trailing `-`.
    [[nodiscard]] bool operator==(TopicRef const&) const = default;
};

/// The thing an agent run works on.
using ItemRef = std::variant<ChangeRequestRef, IssueRef, JiraRef, TopicRef>;

/// The most characters a title, or a check's name, keeps once sanitised (see displayText()).
inline constexpr std::size_t MaxTitleChars = 200;

/// The most items one list command asks for: the `--limit` its declared command passes.
inline constexpr std::size_t ListLimit = 100;

/// One open change request, as a provider's mapping returns it.
struct ChangeRequest
{
    uint64_t number = 0;                    ///< Its number in its repository.
    std::string title;                      ///< Sanitised, at most MaxTitleChars.
    std::string url;                        ///< Its web page, as the forge reported it.
    std::string head;                       ///< The head branch, as the forge reported it: unchecked.
    std::string base;                       ///< The base branch; argumentAccepts(ArgumentKind::Ref, base).
    Readiness readiness = Readiness::Ready; ///< Draft or ready.
    HeadOrigin origin = HeadOrigin::SameRepository; ///< Same repository or a fork.
    CiState ci = CiState::None;                     ///< Its checks, taken together.
    std::vector<std::string> failingChecks;         ///< The names of the checks that failed, sanitised.
    ReviewState review = ReviewState::None;         ///< Where it stands with its reviewers.
    std::vector<uint64_t>
        closesIssues; ///< The issues of its own repository it closes when merged, by number.
    Conflicts conflicts = Conflicts::None;      ///< Whether it conflicts with its base.
    uint64_t updatedWallMs = 0;                 ///< When it was last updated, Unix ms; 0: unknown.
    StateSource source = StateSource::Complete; ///< Where @ref ci and @ref review come from.
    std::string headSha = {};                   ///< Its head commit as the forge reported it; empty: unknown.
    [[nodiscard]] bool operator==(ChangeRequest const&) const = default;
};

/// One pipeline of a project, as its pipeline list reports it.
struct PipelineInfo
{
    uint64_t id = 0; ///< Its project-wide id; a later pipeline has a higher one.
    std::string sha; ///< The commit it ran on.
    std::string ref; ///< The branch, tag or merge request ref it ran for, as the forge reported it.
    CiState ci = CiState::None; ///< Its status, in the model.
    [[nodiscard]] bool operator==(PipelineInfo const&) const = default;
};

/// One failed job, by the pipeline it ran in.
struct FailedJob
{
    uint64_t pipelineId = 0; ///< The pipeline it ran in.
    std::string name;        ///< Its name, sanitised, at most MaxTitleChars.
    [[nodiscard]] bool operator==(FailedJob const&) const = default;
};

/// One open issue, as a provider's mapping returns it.
struct Issue
{
    ItemRef item;      ///< Its reference: an IssueRef for a forge issue.
    std::string title; ///< Sanitised, at most MaxTitleChars.
    std::string url;   ///< Its web page, as the forge reported it.
    [[nodiscard]] bool operator==(Issue const&) const = default;
};

/// Whether a process's output was kept in full.
enum class OutputExtent : uint8_t
{
    Whole,     ///< Everything it printed.
    Truncated, ///< Cut at the host's limit.
};

/// Why a provider's mapping refused a command's output.
enum class ParseError : uint8_t
{
    NotJson,    ///< The output is not one complete JSON document, such as a reply cut off midway.
    WrongShape, ///< It is JSON, but not of the shape the command prints.
};

/// Whether a list holds everything there is.
enum class ListCompleteness : uint8_t
{
    Complete, ///< Fewer than ListLimit entries: there is no more.
    AtLimit,  ///< ListLimit entries: the forge may have more than it listed.
    Cut,      ///< The output was cut at the host's limit: these are the entries before the cut.
};

/// The items a list command's output held.
template <typename T>
struct Listed
{
    std::vector<T> items;    ///< The entries that passed every check.
    std::size_t skipped = 0; ///< The entries left out because a check failed.
    ListCompleteness completeness = ListCompleteness::Complete; ///< Whether the forge may have more.
};

/// The issues no change request claims.
/// @param issues Open issues.
/// @param mine The user's open change requests.
/// @return @p issues without each forge issue whose number some change request in @p mine closes, in order.
[[nodiscard]] std::vector<Issue> unclaimed(std::vector<Issue> issues, std::span<ChangeRequest const> mine);

} // namespace orchestrator
