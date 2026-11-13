// SPDX-License-Identifier: Apache-2.0
#include <contour/window/ProgramStatusBadge.hpp>
#include <contour/window/SwitcherStatus.hpp>
#include <contour/window/WorkspaceDirectory.hpp>

#include <vtbackend/screen/GitHead.hpp>

#include <algorithm>
#include <charconv>
#include <format>
#include <tuple>
#include <utility>
#include <vector>

namespace contour::window
{

namespace
{
    /// @param state What a program says it is doing.
    /// @return The signal it gives.
    [[nodiscard]] constexpr RemoteSignal signalOf(vtcontrol::ProgramState state) noexcept
    {
        switch (state)
        {
            case vtcontrol::ProgramState::Idle: return RemoteSignal::None;
            case vtcontrol::ProgramState::Working: return RemoteSignal::Working;
            case vtcontrol::ProgramState::Done: return RemoteSignal::ProgramDone;
            case vtcontrol::ProgramState::Blocked: return RemoteSignal::Blocked;
            case vtcontrol::ProgramState::Error: return RemoteSignal::ProgramError;
        }
        return RemoteSignal::None;
    }

    /// @param kind What a git HEAD names, as the control API spells it.
    /// @return The same, as the sidebar's git text takes it.
    [[nodiscard]] constexpr vtbackend::GitHeadKind gitHeadKindOf(vtcontrol::GitHeadKind kind) noexcept
    {
        switch (kind)
        {
            case vtcontrol::GitHeadKind::Branch: return vtbackend::GitHeadKind::Branch;
            case vtcontrol::GitHeadKind::Detached: return vtbackend::GitHeadKind::Detached;
            case vtcontrol::GitHeadKind::OtherRef: return vtbackend::GitHeadKind::OtherRef;
        }
        return vtbackend::GitHeadKind::Branch;
    }

    /// The same state, as the sidebar's records spell it.
    [[nodiscard]] constexpr vtbackend::ProgramState programStateOf(vtcontrol::ProgramState state) noexcept
    {
        switch (state)
        {
            case vtcontrol::ProgramState::Idle: return vtbackend::ProgramState::Idle;
            case vtcontrol::ProgramState::Working: return vtbackend::ProgramState::Working;
            case vtcontrol::ProgramState::Done: return vtbackend::ProgramState::Done;
            case vtcontrol::ProgramState::Blocked: return vtbackend::ProgramState::Blocked;
            case vtcontrol::ProgramState::Error: return vtbackend::ProgramState::Error;
        }
        return vtbackend::ProgramState::Idle;
    }

    /// The same kind, as the sidebar's records spell it.
    [[nodiscard]] constexpr vtbackend::BlockedKind blockedKindOf(vtcontrol::BlockedKind kind) noexcept
    {
        switch (kind)
        {
            case vtcontrol::BlockedKind::None: return vtbackend::BlockedKind::None;
            case vtcontrol::BlockedKind::Permission: return vtbackend::BlockedKind::Permission;
            case vtcontrol::BlockedKind::Question: return vtbackend::BlockedKind::Question;
            case vtcontrol::BlockedKind::Auth: return vtbackend::BlockedKind::Auth;
        }
        return vtbackend::BlockedKind::None;
    }

    /// @return @p info as the sidebar's own record of it, so its reason and time are judged the same way.
    [[nodiscard]] vtbackend::ProgramStatusRecord recordOf(vtcontrol::ProgramStatusInfo const& info)
    {
        return vtbackend::ProgramStatusRecord {
            .id = info.id,
            .state = programStateOf(info.state),
            .kind = blockedKindOf(info.kind),
            .progress = info.progress,
            .app = info.app,
            .title = info.title,
            .message = info.message,
            .origin = vtbackend::ProgramStatusOrigin::Program,
            .since = parseRfc3339Utc(info.since).value_or(std::chrono::system_clock::time_point {}),
            .generation = info.generation,
        };
    }

    /// @return The badge @p state earns, as BadgeRows has it; None for idle.
    [[nodiscard]] BadgeKind badgeOf(vtbackend::ProgramState state) noexcept
    {
        auto const row = std::ranges::find(BadgeRows, state, &BadgeRow::state);
        return row != BadgeRows.end() ? row->kind : BadgeKind::None;
    }

    /// A status of @p signal's rule with @p reason, its time kept as @p timeKind says from @p since.
    [[nodiscard]] SwitcherStatus statusOf(RemoteSignal signal,
                                          ReasonParts reason,
                                          TimeText time,
                                          std::optional<std::chrono::system_clock::time_point> since)
    {
        auto const& rule = remoteStatusRule(signal);
        return SwitcherStatus { .urgency = rule.urgency,
                                .glyph = rule.glyph,
                                .gitText = {},
                                .reason = std::move(reason),
                                .time = std::move(time),
                                .since = since };
    }

    /// The statuses one pane gives: one per program-status record, and its last command where that failed.
    void collectPane(vtcontrol::PaneInfo const& pane, std::vector<SwitcherStatus>& parts)
    {
        for (auto const& info: pane.programStatus)
        {
            auto const signal = signalOf(info.state);
            if (signal == RemoteSignal::None)
                continue;
            auto const record = recordOf(info);
            auto const badge = badgeOf(record.state);
            auto const timeKind = statusTimeKind(record.progress, badge);
            parts.push_back(statusOf(signal,
                                     statusReasonOf(record, badge, MaxRowTextLength),
                                     TimeText { .kind = timeKind, .text = {}, .percent = record.progress },
                                     parseRfc3339Utc(info.since)));
        }
        if (!pane.lastFinished || pane.lastFinished->exitCode.value_or(0) == 0)
            return;
        // As the sidebar's finished reason: the exit and how long it ran, and no age beside it.
        auto const& block = *pane.lastFinished;
        auto const finished =
            vthost::proto::FinishedCommand { .exitCode = block.exitCode,
                                             .durationMs = block.durationMs,
                                             .commandLine = block.command.value_or(std::string {}) };
        parts.push_back(
            statusOf(RemoteSignal::CommandFailed, finishedText(finished), TimeText {}, std::nullopt));
    }

    /// Reads exactly @p width decimal digits at @p at, advancing it; no sign, no fewer digits.
    [[nodiscard]] std::optional<int> digits(std::string_view text, std::size_t& at, std::size_t width)
    {
        if (at + width > text.size())
            return std::nullopt;
        auto const field = text.substr(at, width);
        if (!std::ranges::all_of(field, [](char c) { return c >= '0' && c <= '9'; }))
            return std::nullopt;
        auto value = 0;
        std::ignore = std::from_chars(field.data(), field.data() + field.size(), value);
        at += width;
        return value;
    }

    /// Whether @p text holds @p c at @p at, advancing past it when it does.
    [[nodiscard]] bool expect(std::string_view text, std::size_t& at, char c)
    {
        if (at >= text.size() || text[at] != c)
            return false;
        ++at;
        return true;
    }
} // namespace

SwitcherStatus switcherStatusOf(SidebarRow const& workspaceRow)
{
    return SwitcherStatus { .urgency = workspaceRow.urgency,
                            .glyph = workspaceRow.glyph,
                            .gitText = workspaceRow.gitText,
                            .reason = { .label = workspaceRow.reasonLabel,
                                        .separator = workspaceRow.reasonSeparator,
                                        .command = workspaceRow.reasonCommand },
                            .time = workspaceRow.time,
                            .since = std::nullopt };
}

SwitcherStatus switcherStatusOf(vtcontrol::WorkspaceInfo const& workspace)
{
    auto parts = std::vector<SwitcherStatus> {};
    for (auto const& tab: workspace.tabs)
        for (auto const& pane: tab.panes)
            collectPane(pane, parts);
    auto status = rollUpStatus(parts);
    if (workspace.git)
        status.gitText = gitTextOf(DirectoryGit { .branch = workspace.git->branch,
                                                  .kind = gitHeadKindOf(workspace.git->kind),
                                                  .ahead = workspace.git->ahead,
                                                  .behind = workspace.git->behind });
    return status;
}

SwitcherStatus rollUpStatus(std::span<SwitcherStatus const> parts)
{
    // max_element keeps the first of equals, as the sidebar's roll-up does.
    auto const most = std::ranges::max_element(
        parts, [](SwitcherStatus const& a, SwitcherStatus const& b) { return a.urgency < b.urgency; });
    if (most == parts.end() || most->urgency == Urgency::Idle)
        return SwitcherStatus {};
    return *most;
}

std::string switcherTimeText(SwitcherStatus const& status, std::chrono::system_clock::time_point now)
{
    if (!status.time.text.empty())
        return status.time.text;
    return formatTimeText(status.time.kind, status.since, status.time.percent, now).text;
}

std::optional<std::chrono::system_clock::time_point> parseRfc3339Utc(std::string_view text)
{
    auto at = std::size_t { 0 };
    auto const year = digits(text, at, 4);
    if (!year || !expect(text, at, '-'))
        return std::nullopt;
    auto const month = digits(text, at, 2);
    if (!month || !expect(text, at, '-'))
        return std::nullopt;
    auto const day = digits(text, at, 2);
    if (!day || !expect(text, at, 'T'))
        return std::nullopt;
    auto const hour = digits(text, at, 2);
    if (!hour || !expect(text, at, ':'))
        return std::nullopt;
    auto const minute = digits(text, at, 2);
    if (!minute || !expect(text, at, ':'))
        return std::nullopt;
    auto const second = digits(text, at, 2);
    if (!second)
        return std::nullopt;

    // The fraction, to millisecond precision: digits past the third are read and dropped.
    auto millis = 0;
    if (expect(text, at, '.'))
    {
        auto scale = 100;
        auto const start = at;
        while (at < text.size() && text[at] >= '0' && text[at] <= '9')
        {
            millis += (text[at] - '0') * scale;
            scale /= 10;
            ++at;
        }
        if (at == start)
            return std::nullopt;
    }
    if (!expect(text, at, 'Z') || at != text.size())
        return std::nullopt;

    auto const date = std::chrono::year { *year } / std::chrono::month { static_cast<unsigned>(*month) }
                      / std::chrono::day { static_cast<unsigned>(*day) };
    if (!date.ok() || *hour > 23 || *minute > 59 || *second > 60)
        return std::nullopt;
    return std::chrono::sys_days { date } + std::chrono::hours { *hour } + std::chrono::minutes { *minute }
           + std::chrono::seconds { *second } + std::chrono::milliseconds { millis };
}

} // namespace contour::window
