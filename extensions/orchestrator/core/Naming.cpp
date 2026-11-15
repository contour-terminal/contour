// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/core/Naming.hpp>

#include <algorithm>
#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <variant>

#include <orchestrator/core/Model.hpp>

namespace orchestrator
{

namespace
{
    [[nodiscard]] constexpr char toLower(char c) noexcept
    {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    }

    [[nodiscard]] std::expected<std::string, NamingError> slugOf(ChangeRequestRef const& ref)
    {
        return std::format("pr-{}", ref.number);
    }

    [[nodiscard]] std::expected<std::string, NamingError> slugOf(IssueRef const& ref)
    {
        return std::format("issue-{}", ref.number);
    }

    [[nodiscard]] std::expected<std::string, NamingError> slugOf(JiraRef const& ref)
    {
        if (!isJiraKey(ref.key))
            return std::unexpected { NamingError::NotJiraKey };
        auto slug = ref.key;
        std::ranges::transform(slug, slug.begin(), toLower);
        return slug;
    }
} // namespace

std::expected<std::string, NamingError> itemSlug(ItemRef const& item)
{
    return std::visit([](auto const& ref) { return slugOf(ref); }, item);
}

std::expected<RunId, NamingError> runId(ItemRef const& item, Stamp const& stamp)
{
    return itemSlug(item).transform([&](std::string const& slug) {
        return RunId { std::format("{}-{:04}{:02}{:02}-{:02}{:02}{:02}",
                                   slug,
                                   stamp.year,
                                   stamp.month,
                                   stamp.day,
                                   stamp.hour,
                                   stamp.minute,
                                   stamp.second) };
    });
}

std::string branchName(RunId const& runId)
{
    return std::format("{}{}", BranchPrefix, runId.text());
}

std::string worktreePath(std::string_view root, std::string_view repo, RunId const& runId)
{
    return std::format("{}/{}/{}", root, repo, runId.text());
}

} // namespace orchestrator
