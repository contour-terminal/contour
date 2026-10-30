// SPDX-License-Identifier: Apache-2.0
#include <contour/cli/ConnectList.hpp>

#include <core/Utils.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <ranges>

#include <vtcontrol/Json.hpp>

namespace contour::cli
{

namespace
{
    /// The words a target kind is also found by, and the name `--list --json` gives it.
    struct KindRow
    {
        config::LaunchTargetKind kind;
        std::string_view jsonName;
        std::string_view keywords;
    };

    constexpr auto KindRows = std::array {
        KindRow { config::LaunchTargetKind::LocalDaemon,
                  "local-daemon",
                  "this computer persistent sessions local daemon" },
        KindRow { config::LaunchTargetKind::InProcess,
                  "in-process",
                  "this computer this window only local no daemon" },
        KindRow { config::LaunchTargetKind::SavedHost, "saved-host", "saved host" },
        KindRow { config::LaunchTargetKind::Address, "address", "address" },
    };

    [[nodiscard]] KindRow const& kindRow(config::LaunchTargetKind kind)
    {
        return *std::ranges::find(KindRows, kind, &KindRow::kind);
    }

    [[nodiscard]] std::string schemeTag(config::HostScheme scheme)
    {
        return core::toUpper(config::hostSchemeName(scheme));
    }

    /// Whether every word of @p filter occurs in @p haystack.
    [[nodiscard]] bool matches(std::string_view haystack, std::string_view filter)
    {
        auto const text = core::toLower(haystack);
        return std::ranges::all_of(
            std::string_view { core::toLower(filter) } | std::views::split(' ')
                | std::views::transform(
                    [](auto word) { return std::string_view { word.begin(), word.end() }; }),
            [&](std::string_view word) { return word.empty() || text.contains(word); });
    }
} // namespace

std::string quickSelectLabel(QuickSelectKey key, std::size_t digit)
{
    return std::format("{}{}", key == QuickSelectKey::Command ? "⌘" : "Alt+", digit);
}

std::string describeLocalDaemon(LocalDaemonState const& state, std::string_view label)
{
    auto const name = label.empty() ? std::string { "Local daemon" } : std::format("Daemon {}", label);
    if (state.run == DaemonRunState::Probing)
        return std::format("{} · checking…", name);
    if (state.run == DaemonRunState::NotRunning)
        return std::format("{} · not running · starts on connect", name);
    if (!state.sessions)
        return std::format("{} · running", name);
    return std::format(
        "{} · running · {} {}", name, *state.sessions, *state.sessions == 1 ? "session" : "sessions");
}

std::vector<ConnectRow> connectRows(ConnectListInputs const& inputs, std::string_view filter)
{
    auto rows = std::vector<ConnectRow> {};

    // An address that parses is offered first, whatever else matches.
    if (auto const address = core::trim(filter); address.contains("://"))
        if (auto const target = config::parseLaunchTarget(address, inputs.hosts);
            target && target->kind == config::LaunchTargetKind::Address)
            rows.push_back(ConnectRow { .target = *target,
                                        .group = ConnectGroup::Typed,
                                        .title = std::format("Connect to {}", target->name),
                                        .detail = "Not saved",
                                        .tag = {},
                                        .text = config::toString(*target),
                                        .quick = std::nullopt });

    auto const local = config::LaunchTarget { .kind = config::LaunchTargetKind::LocalDaemon, .name = {} };
    auto const process = config::LaunchTarget { .kind = config::LaunchTargetKind::InProcess, .name = {} };
    // What a row is found by: its name, its kind's words and, for a host, its address; never a status line,
    // which would make "star" find a daemon that "starts on connect".
    auto const offer = [&](ConnectRow row, std::string_view address) {
        auto const haystack =
            std::format("{} {} {} {}", row.title, address, row.text, kindRow(row.target.kind).keywords);
        if (matches(haystack, filter))
            rows.push_back(std::move(row));
    };
    offer(ConnectRow { .target = local,
                       .group = ConnectGroup::ThisComputer,
                       .title = "Persistent sessions",
                       .detail = describeLocalDaemon(inputs.daemon, local.name),
                       .tag = {},
                       .text = config::toString(local),
                       .quick = std::nullopt },
          {});
    offer(ConnectRow { .target = process,
                       .group = ConnectGroup::ThisComputer,
                       .title = "This window only",
                       .detail = "Without the daemon: its shells end when the window closes",
                       .tag = {},
                       .text = config::toString(process),
                       .quick = std::nullopt },
          {});
    for (auto const& [name, entry]: inputs.hosts)
        offer(ConnectRow { .target = config::LaunchTarget { .kind = config::LaunchTargetKind::SavedHost,
                                                            .name = name },
                           .group = ConnectGroup::SavedHosts,
                           .title = name,
                           .detail = config::toString(entry.url),
                           .tag = schemeTag(entry.url.scheme),
                           .text = name,
                           .quick = std::nullopt },
              config::toString(entry.url));

    for (auto&& [index, row]: core::views::enumerate(rows))
        if (static_cast<std::size_t>(index) < QuickSelectCount)
            row.quick = static_cast<std::size_t>(index) + 1;
    return rows;
}

std::size_t initialSelection(std::vector<ConnectRow> const& rows, std::string_view lastUsed)
{
    auto const found = std::ranges::find(rows, lastUsed, &ConnectRow::text);
    if (lastUsed.empty() || found == rows.end())
        return 0;
    return static_cast<std::size_t>(found - rows.begin());
}

std::optional<std::size_t> rowForQuickSelect(std::vector<ConnectRow> const& rows, std::size_t digit)
{
    auto const found = std::ranges::find(rows, std::optional<std::size_t> { digit }, &ConnectRow::quick);
    if (found == rows.end())
        return std::nullopt;
    return static_cast<std::size_t>(found - rows.begin());
}

std::string renderTargetList(std::vector<ConnectRow> const& rows, ListFormat format)
{
    auto out = std::string {};
    if (format == ListFormat::Text)
    {
        for (auto const& row: rows)
            out += std::format("{}\t{}\n", row.text, row.detail);
        return out;
    }
    out += "[";
    for (auto const [index, row]: core::views::enumerate(rows))
        out += std::format(R"({}{{"target":{},"kind":{},"title":{},"detail":{},"tag":{}}})",
                           index == 0 ? "" : ",",
                           vtcontrol::quoteJsonString(row.text),
                           vtcontrol::quoteJsonString(kindRow(row.target.kind).jsonName),
                           vtcontrol::quoteJsonString(row.title),
                           vtcontrol::quoteJsonString(row.detail),
                           vtcontrol::quoteJsonString(row.tag));
    out += "]\n";
    return out;
}

} // namespace contour::cli
