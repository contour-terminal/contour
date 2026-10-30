// SPDX-License-Identifier: Apache-2.0
#include <contour/config/LaunchTarget.hpp>

#include <algorithm>
#include <format>
#include <ranges>

namespace contour::config
{

std::string LaunchTargetFailure::describe() const
{
    switch (error)
    {
        case LaunchTargetError::Empty: return "no target given";
        case LaunchTargetError::UnknownName:
            return std::format("'{}' is neither daemon, local, a saved host nor an ssh:// or tls:// address",
                               detail);
        case LaunchTargetError::BadAddress: return detail;
        case LaunchTargetError::LabelNotUsed: return std::format("'{}' takes no label", detail);
        case LaunchTargetError::EmptyLabel: return std::format("'{}:' names no label", detail);
    }
    return {};
}

std::expected<LaunchTarget, LaunchTargetFailure> parseLaunchTarget(
    std::string_view text, std::map<std::string, HostEntry> const& hosts)
{
    if (text.empty())
        return std::unexpected { LaunchTargetFailure { .error = LaunchTargetError::Empty, .detail = {} } };

    if (text.contains("://"))
    {
        auto const url = parseHostUrl(text);
        if (!url)
            return std::unexpected { LaunchTargetFailure {
                .error = LaunchTargetError::BadAddress, .detail = std::string { describe(url.error()) } } };
        return LaunchTarget { .kind = LaunchTargetKind::Address, .name = toString(*url) };
    }

    auto const colon = text.find(':');
    auto const word = text.substr(0, colon);
    if (auto const row = std::ranges::find(LaunchKeywords, word, &LaunchKeyword::keyword);
        row != LaunchKeywords.end())
    {
        if (colon == std::string_view::npos)
            return LaunchTarget { .kind = row->kind, .name = {} };
        if (row->label == KeywordLabel::None)
            return std::unexpected { LaunchTargetFailure { .error = LaunchTargetError::LabelNotUsed,
                                                           .detail = std::string { word } } };
        auto const label = text.substr(colon + 1);
        if (label.empty())
            return std::unexpected { LaunchTargetFailure { .error = LaunchTargetError::EmptyLabel,
                                                           .detail = std::string { word } } };
        return LaunchTarget { .kind = row->kind, .name = std::string { label } };
    }

    if (auto const name = std::string { text }; hosts.contains(name))
        return LaunchTarget { .kind = LaunchTargetKind::SavedHost, .name = name };
    return std::unexpected { LaunchTargetFailure { .error = LaunchTargetError::UnknownName,
                                                   .detail = std::string { text } } };
}

std::string toString(LaunchTarget const& target)
{
    if (target.kind == LaunchTargetKind::SavedHost || target.kind == LaunchTargetKind::Address)
        return target.name;
    auto const row = std::ranges::find(LaunchKeywords, target.kind, &LaunchKeyword::kind);
    if (row == LaunchKeywords.end())
        return {};
    if (target.name.empty())
        return std::string { row->keyword };
    return std::format("{}:{}", row->keyword, target.name);
}

AttachPlan attachPlanOf(LaunchTarget const& target)
{
    switch (target.kind)
    {
        case LaunchTargetKind::InProcess: return AttachPlan { .hosting = Hosting::InProcess };
        case LaunchTargetKind::LocalDaemon:
            return AttachPlan { .hosting = Hosting::LocalDaemon, .label = target.name };
        case LaunchTargetKind::SavedHost:
        case LaunchTargetKind::Address:
            return AttachPlan { .hosting = Hosting::RemoteHost, .host = target.name };
    }
    return {};
}

std::vector<std::string> connectArguments(LaunchTarget const& target, std::string_view configPath)
{
    auto arguments = std::vector<std::string> { "connect" };
    if (!configPath.empty())
    {
        arguments.emplace_back("--config");
        arguments.emplace_back(configPath);
    }
    // After `--`, so that a host named like an option (`list`, `config`) is still a target.
    arguments.emplace_back("--");
    arguments.push_back(toString(target));
    return arguments;
}

} // namespace contour::config
