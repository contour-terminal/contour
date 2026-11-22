// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/core/GitLab.hpp>

#include <array>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

#include <glaze/glaze.hpp>
#include <orchestrator/core/ArgumentKinds.hpp>
#include <orchestrator/core/GitLabJson.hpp>
#include <orchestrator/core/Json.hpp>
#include <orchestrator/core/Model.hpp>
#include <orchestrator/core/Sanitize.hpp>
#include <orchestrator/core/Spelling.hpp>

using namespace std::string_view_literals;

// GitLab spells its keys in snake_case; glaze finds each table by the spelling `value`, as the standard
// library finds `value_type`.

/// A merge request.
template <>
struct glz::meta<orchestrator::gitlab::detail::JsonMergeRequest>
{
    using T = orchestrator::gitlab::detail::JsonMergeRequest;
    // NOLINTNEXTLINE(readability-identifier-naming)
    static constexpr auto value = glz::object("iid",
                                              &T::iid,
                                              "title",
                                              &T::title,
                                              "web_url",
                                              &T::webUrl,
                                              "source_branch",
                                              &T::sourceBranch,
                                              "target_branch",
                                              &T::targetBranch,
                                              "draft",
                                              &T::draft,
                                              "source_project_id",
                                              &T::sourceProjectId,
                                              "target_project_id",
                                              &T::targetProjectId,
                                              "detailed_merge_status",
                                              &T::detailedMergeStatus);
};

/// An issue.
template <>
struct glz::meta<orchestrator::gitlab::detail::JsonIssue>
{
    using T = orchestrator::gitlab::detail::JsonIssue;
    // NOLINTNEXTLINE(readability-identifier-naming)
    static constexpr auto value = glz::object("iid", &T::iid, "title", &T::title, "web_url", &T::webUrl);
};

/// A project.
template <>
struct glz::meta<orchestrator::gitlab::detail::JsonProject>
{
    using T = orchestrator::gitlab::detail::JsonProject;
    // NOLINTNEXTLINE(readability-identifier-naming)
    static constexpr auto value = glz::object("default_branch", &T::defaultBranch);
};

namespace orchestrator::gitlab
{

namespace
{
    using detail::JsonIssue;
    using detail::JsonMergeRequest;
    using detail::JsonProject;

    /// The input is a string_view: nothing terminates it, and GitLab's objects hold many keys this mapping
    /// does not read.
    constexpr auto ReadOptions = glz::opts { .null_terminated = false, .error_on_unknown_keys = false };

    /// The `detailed_merge_status` values that say something about review; every other one (`mergeable`'s
    /// blockers such as `ci_must_pass` or `draft_status` among them) says nothing about it.
    constexpr auto MergeStatuses = std::array {
        Spelling { .text = "not_approved"sv, .meaning = ReviewState::Required },
        Spelling { .text = "requested_changes"sv, .meaning = ReviewState::ChangesRequested },
        Spelling { .text = "mergeable"sv, .meaning = ReviewState::Approved },
    };

    /// @return @p json in the model.
    ChangeRequest changeRequestOf(JsonMergeRequest const& json)
    {
        return ChangeRequest {
            .number = json.iid,
            .title = displayText(json.title, MaxTitleChars),
            .url = json.webUrl,
            .head = json.sourceBranch,
            .base = json.targetBranch,
            .readiness = json.draft ? Readiness::Draft : Readiness::Ready,
            .origin =
                json.sourceProjectId != json.targetProjectId ? HeadOrigin::Fork : HeadOrigin::SameRepository,
            .ci = CiState::None,
            .failingChecks = {},
            .review = reviewStateOf(json.detailedMergeStatus.value_or(std::string {})),
            .closesIssues = {},
        };
    }
} // namespace

ReviewState reviewStateOf(std::string_view detailedMergeStatus) noexcept
{
    return meaningOf(MergeStatuses, detailedMergeStatus, ReviewState::None);
}

std::expected<Listed<ChangeRequest>, ParseError> changeRequests(std::string_view stdoutText,
                                                                OutputExtent extent)
{
    return json::readList<ReadOptions, JsonMergeRequest>(stdoutText, extent).transform([](auto const& read) {
        auto listed = Listed<ChangeRequest> { .items = {}, .skipped = 0, .completeness = read.completeness };
        for (auto const& entry: read.entries)
        {
            // The base reaches a command line (`git worktree add … {base}`): one that is no Ref is left out.
            if (entry.iid == 0 || !argumentAccepts(ArgumentKind::Ref, entry.targetBranch))
                ++listed.skipped;
            else
                listed.items.push_back(changeRequestOf(entry));
        }
        return listed;
    });
}

std::expected<Listed<Issue>, ParseError> issues(std::string_view stdoutText, OutputExtent extent)
{
    return json::readList<ReadOptions, JsonIssue>(stdoutText, extent).transform([](auto const& read) {
        auto listed = Listed<Issue> { .items = {}, .skipped = 0, .completeness = read.completeness };
        for (auto const& entry: read.entries)
        {
            if (entry.iid == 0)
                ++listed.skipped;
            else
                listed.items.push_back(Issue { .item = IssueRef { .number = entry.iid },
                                               .title = displayText(entry.title, MaxTitleChars),
                                               .url = entry.webUrl });
        }
        return listed;
    });
}

std::expected<std::string, ParseError> defaultBranch(std::string_view stdoutText)
{
    return json::readDocument<ReadOptions, JsonProject>(stdoutText)
        .and_then([](JsonProject project) -> std::expected<std::string, ParseError> {
            if (!argumentAccepts(ArgumentKind::Ref, project.defaultBranch))
                return std::unexpected(ParseError::WrongShape);
            return std::move(project.defaultBranch);
        });
}

} // namespace orchestrator::gitlab
