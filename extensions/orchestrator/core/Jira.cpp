// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/core/Jira.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>

#include <glaze/glaze.hpp>
#include <orchestrator/core/JiraJson.hpp>
#include <orchestrator/core/Json.hpp>
#include <orchestrator/core/Model.hpp>
#include <orchestrator/core/Naming.hpp>
#include <orchestrator/core/Sanitize.hpp>

using namespace std::string_view_literals;

namespace orchestrator::jira
{

namespace
{
    using detail::JsonWorkItem;

    /// The input is a string_view: nothing terminates it, and a work item holds many keys this mapping does
    /// not read.
    constexpr auto ReadOptions = glz::opts { .null_terminated = false, .error_on_unknown_keys = false };

    constexpr auto Scheme = "https://"sv;

    /// A `statusCategory.key` and the category it names.
    struct StatusCategoryName
    {
        std::string_view key;
        StatusCategory category;
    };

    /// Jira's three status categories; every status a project defines falls into one.
    constexpr auto StatusCategories = std::array {
        StatusCategoryName { .key = "new", .category = StatusCategory::ToDo },
        StatusCategoryName { .key = "indeterminate", .category = StatusCategory::InProgress },
        StatusCategoryName { .key = "done", .category = StatusCategory::Done },
    };

    /// @return Whether @p c may stand in a site's host and port.
    [[nodiscard]] constexpr bool isHostByte(char c) noexcept
    {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.'
               || c == '-' || c == ':';
    }
} // namespace

std::optional<std::string_view> siteOf(std::string_view text) noexcept
{
    if (text.size() > MaxSiteBytes || !text.starts_with(Scheme))
        return std::nullopt;
    auto host = text.substr(Scheme.size());
    if (host.ends_with('/'))
        host.remove_suffix(1);
    if (host.empty() || !std::ranges::all_of(host, isHostByte) || host.front() == '.' || host.front() == ':'
        || host.front() == '-')
        return std::nullopt;
    return text.substr(0, Scheme.size() + host.size());
}

StatusCategory statusCategoryOf(std::string_view key) noexcept
{
    auto const known = std::ranges::find(StatusCategories, key, &StatusCategoryName::key);
    return known == StatusCategories.end() ? StatusCategory::Unknown : known->category;
}

std::string browseUrl(std::string_view site, std::string_view key)
{
    return std::format("{}/browse/{}", site, key);
}

std::expected<Listed<Issue>, ParseError> issues(std::string_view stdoutText,
                                                OutputExtent extent,
                                                IssueSourceContext const& context)
{
    return json::readList<ReadOptions, JsonWorkItem>(stdoutText, extent).transform([&](auto const& read) {
        auto listed = Listed<Issue> { .items = {}, .skipped = 0, .completeness = read.completeness };
        for (auto const& [rank, entry]: std::views::enumerate(read.entries))
        {
            // A run's names are made from the key: one that is no Jira key is left out.
            if (!isJiraKey(entry.key))
            {
                ++listed.skipped;
                continue;
            }
            auto const& status = entry.fields.status;
            listed.items.push_back(Issue {
                .item = JiraRef { .key = entry.key },
                .title = displayText(entry.fields.summary, MaxTitleChars),
                .url = browseUrl(context.site, entry.key),
                .status = status ? displayText(status->name, MaxTitleChars) : std::string {},
                .category = status && status->statusCategory ? statusCategoryOf(status->statusCategory->key)
                                                             : StatusCategory::Unknown,
                .updatedWallMs = 0, // acli's search cannot print `updated`.
                .listRank = static_cast<std::size_t>(rank),
            });
        }
        return listed;
    });
}

} // namespace orchestrator::jira
