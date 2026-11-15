// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/proto/Environment.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace std::string_view_literals;

namespace orchestrator::proto
{

namespace
{
    constexpr auto WithheldTable = std::array {
        WithheldVariable { .name = "CONTOUR_"sv, .match = NameMatch::Prefix },
        WithheldVariable { .name = "GIT_"sv, .match = NameMatch::Prefix },
        WithheldVariable { .name = "GH_REPO"sv, .match = NameMatch::Exact },
        WithheldVariable { .name = "GH_HOST"sv, .match = NameMatch::Exact },
        WithheldVariable { .name = "GH_ENTERPRISE_TOKEN"sv, .match = NameMatch::Exact },
        WithheldVariable { .name = "GITHUB_ENTERPRISE_TOKEN"sv, .match = NameMatch::Exact },
    };

    /// Exceptions to WithheldTable, by exact name. These configure git's transport authentication (which
    /// ssh, which key, which credential prompt), not which repository git operates on, and a user's
    /// fetch and push may depend on them.
    constexpr auto PassedThroughTable = std::array {
        "GIT_SSH_COMMAND"sv,
        "GIT_SSH"sv,
        "GIT_ASKPASS"sv,
    };

    /// @return The name of @p entry: what comes before its first `=`; all of it when it has none.
    [[nodiscard]] constexpr std::string_view nameOf(std::string_view entry) noexcept
    {
        return entry.substr(0, entry.find('='));
    }
} // namespace

std::span<WithheldVariable const> withheldVariables() noexcept
{
    return WithheldTable;
}

bool isWithheld(std::string_view entry) noexcept
{
    auto const name = nameOf(entry);
    if (std::ranges::contains(PassedThroughTable, name))
        return false;
    return std::ranges::any_of(WithheldTable, [name](WithheldVariable const& row) {
        return row.match == NameMatch::Exact ? name == row.name : name.starts_with(row.name);
    });
}

std::vector<std::string> withoutWithheld(std::span<std::string const> environment)
{
    auto kept = std::vector<std::string> {};
    for (auto const& entry: environment)
        if (!isWithheld(entry))
            kept.push_back(entry);
    return kept;
}

std::optional<std::string> lookup(std::span<std::string const> environment, std::string_view name)
{
    for (auto const& entry: environment)
        if (nameOf(entry) == name && entry.size() > name.size() + 1)
            return entry.substr(name.size() + 1);
    return std::nullopt;
}

std::vector<std::string> withVariables(std::vector<std::string> base,
                                       std::span<std::span<EnvironmentVariable const> const> layers)
{
    for (auto const layer: layers)
        for (auto const& variable: layer)
        {
            std::erase_if(base, [&](std::string const& entry) { return nameOf(entry) == variable.name; });
            base.push_back(std::format("{}={}", variable.name, variable.value));
        }
    return base;
}

} // namespace orchestrator::proto
