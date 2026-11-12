// SPDX-License-Identifier: Apache-2.0
#include <contour/command/CommandPaletteTree.hpp>
#include <contour/window/SidebarTree.hpp>
#include <contour/window/SwitcherText.hpp>

#include <algorithm>
#include <format>
#include <initializer_list>
#include <optional>
#include <ranges>
#include <vector>

#include <vthost/SocketPath.hpp>

namespace contour::window
{

namespace
{
    using vthost::proto::SwitchDaemonKind;

    /// Joins the non-empty parts with LabelSeparator.
    [[nodiscard]] std::string joined(std::initializer_list<std::string_view> parts)
    {
        auto text = std::string {};
        for (auto const part: parts)
        {
            if (part.empty())
                continue;
            if (!text.empty())
                text += LabelSeparator;
            text += part;
        }
        return text;
    }

    /// @return `1 tab`, `4 tabs`: one source string per form.
    [[nodiscard]] std::string tabsText(std::size_t count)
    {
        return count == 1 ? std::string { "1 tab" } : std::format("{} tabs", count);
    }

    /// @return `1 workspace`, `4 workspaces`: one source string per form.
    [[nodiscard]] std::string workspacesText(std::size_t count)
    {
        return count == 1 ? std::string { "1 workspace" } : std::format("{} workspaces", count);
    }

    /// What a saved host's row says while it is not listed, per SwitcherHostLink.
    constexpr auto HostLinkTexts =
        std::array<std::string_view, 3> { "Not connected", "Not connected", "Connected" };
    static_assert(HostLinkTexts.size() == static_cast<std::size_t>(SwitcherHostLink::Connected) + 1,
                  "every SwitcherHostLink needs its text");

    /// @return The name of the daemon @p row is on; empty when the index is out of range.
    [[nodiscard]] std::string daemonNameOf(SwitcherRow const& row, SwitcherInputs const& inputs)
    {
        return row.daemon < inputs.daemons.size() ? switcherDaemonName(inputs.daemons[row.daemon].address)
                                                  : std::string {};
    }

    [[nodiscard]] SwitcherDetail workspaceDetail(SwitcherRow const& row,
                                                 std::chrono::system_clock::time_point now)
    {
        auto const& status = row.status;
        auto const hasReason = !status.reason.label.empty() || !status.reason.command.empty();
        if (switcherDetailRule(status.urgency).reason == ShowReason::No || !hasReason)
            return { .text = joined({ status.gitText, tabsText(row.tabs) }),
                     .separator = {},
                     .command = {},
                     .tail = {} };
        auto text = status.gitText.empty()
                        ? status.reason.label
                        : std::format("{}{}{}", status.gitText, LabelSeparator, status.reason.label);
        auto const time = switcherTimeText(status, now);
        return { .text = std::move(text),
                 .separator = status.reason.command.empty() ? std::string {} : status.reason.separator,
                 .command = status.reason.command,
                 .tail = time.empty() ? std::string {} : std::format("{}{}", LabelSeparator, time) };
    }

    [[nodiscard]] std::string daemonDetail(SwitcherDaemon const& daemon)
    {
        switch (daemon.listing)
        {
            case SwitcherListing::Listed: return workspacesText(daemon.workspaces.size());
            case SwitcherListing::Listing: return "Looking…";
            case SwitcherListing::Failed: return daemon.failure;
            case SwitcherListing::NotListed: break;
        }
        if (daemon.workspaceCount)
            return workspacesText(*daemon.workspaceCount);
        if (daemon.address.kind == SwitchDaemonKind::Host)
            return std::string { HostLinkTexts.at(static_cast<std::size_t>(daemon.link)) };
        return {};
    }

    /// @return The verb the footer names for Escape.
    [[nodiscard]] std::string_view escapeVerbOf(SwitcherCommand escape) noexcept
    {
        switch (escape)
        {
            case SwitcherCommand::ClearFilter: return "Clear";
            case SwitcherCommand::Back: return "Back";
            case SwitcherCommand::Close: return "Close";
            case SwitcherCommand::None:
            case SwitcherCommand::Previous:
            case SwitcherCommand::Next:
            case SwitcherCommand::First:
            case SwitcherCommand::Last:
            case SwitcherCommand::Activate:
            case SwitcherCommand::ActivateInNewWindow:
            case SwitcherCommand::DrillIn:
            case SwitcherCommand::OpenRowMenu: break;
        }
        return {};
    }

    [[nodiscard]] std::string keyText(vtbackend::Key input,
                                      command::ShortcutStyle style,
                                      vtbackend::Modifiers modifiers = {})
    {
        return command::shortcutText(command::ShortcutChord { .modifiers = modifiers, .input = input },
                                     style);
    }

} // namespace

SwitcherDetail switcherDetail(SwitcherRow const& row,
                              SwitcherInputs const& inputs,
                              std::chrono::system_clock::time_point now)
{
    switch (row.kind)
    {
        case SwitcherRowKind::Workspace: return workspaceDetail(row, now);
        case SwitcherRowKind::Daemon:
            if (row.daemon < inputs.daemons.size())
                return { .text = daemonDetail(inputs.daemons[row.daemon]),
                         .separator = {},
                         .command = {},
                         .tail = {} };
            return {};
        case SwitcherRowKind::NewWorkspace:
            return { .text = std::format("on {}", daemonNameOf(row, inputs)), .separator = {}, .command = {}, .tail = {} };
        case SwitcherRowKind::NewDaemon:
        case SwitcherRowKind::AllDaemons:
        case SwitcherRowKind::Discovering:
        case SwitcherRowKind::Connecting:
        case SwitcherRowKind::LabelAction:
        case SwitcherRowKind::Failure: break;
    }
    return {};
}

std::string switcherDescription(SwitcherRow const& row,
                                SwitcherInputs const& inputs,
                                SwitcherView const& view,
                                session::SwitchMode mode)
{
    auto const daemon = daemonNameOf(row, inputs);
    switch (row.kind)
    {
        case SwitcherRowKind::Workspace:
            if (mode == session::SwitchMode::Open)
                return std::format("Open {} in a new window", row.title);
            if (view.level == SwitcherLevel::Top && view.origin == SwitcherOrigin::Chip
                && view.scope == SwitcherScope::OneDaemon)
                return joined({ row.title, row.status.gitText });
            if (row.daemon != inputs.windowDaemon)
                return std::format("Move this window to {}{}{}", daemon, LabelSeparator, row.title);
            if (!view.filter.empty())
                return std::format("Switch this window to {} on {}", row.title, daemon);
            return std::format("Switch this window to {}", row.title);
        case SwitcherRowKind::Daemon: return std::format("Show the workspaces on {}", daemon);
        case SwitcherRowKind::NewWorkspace: return std::format("Create a workspace on {}", daemon);
        case SwitcherRowKind::NewDaemon: return "Starts a new daemon with one workspace";
        case SwitcherRowKind::LabelAction:
            // What Return does with the label: start a daemon under it, or switch to the one running.
            if (switcherLabelVerdict(row.target.daemon, switcherRunningLabels(inputs))
                == LabelVerdict::Running)
                return switcherLabelActionTitle(row.target.daemon, LabelVerdict::Running);
            return "Starts a new daemon with one workspace";
        case SwitcherRowKind::AllDaemons: return "Show every daemon";
        case SwitcherRowKind::Discovering:
        case SwitcherRowKind::Connecting:
        case SwitcherRowKind::Failure: break;
    }
    return {};
}

SwitcherHintSubject switcherHintSubject(SwitcherRow const& row) noexcept
{
    return switcherRowKindRule(row.kind).hints;
}

std::vector<std::string> switcherFooterHintSets(SwitcherHintSubject subject,
                                                SwitcherCommand escape,
                                                command::ShortcutStyle style,
                                                session::SwitchMode mode)
{
    auto row = SwitcherHintRows.at(static_cast<std::size_t>(subject));
    // A pick an Open asked for: Return opens a new window already, and Shift+Return does nothing more.
    if (mode == session::SwitchMode::Open && !row.openVerb.empty())
    {
        row.enterVerb = row.openVerb;
        row.shiftEnterVerb = {};
    }
    auto const hintOf = [&](SwitcherHintKey key) -> std::string {
        auto const spelled = [&](std::string const& keyName, std::string_view verb) {
            return verb.empty() ? std::string {} : std::format("{} {}", keyName, verb);
        };
        switch (key)
        {
            case SwitcherHintKey::Enter: return spelled(keyText(vtbackend::Key::Enter, style), row.enterVerb);
            case SwitcherHintKey::ShiftEnter:
                return spelled(keyText(vtbackend::Key::Enter, style, vtbackend::Modifier::Shift),
                               row.shiftEnterVerb);
            case SwitcherHintKey::Right:
                return spelled(keyText(vtbackend::Key::RightArrow, style), row.rightVerb);
            case SwitcherHintKey::Escape:
                return spelled(keyText(vtbackend::Key::Escape, style), escapeVerbOf(escape));
        }
        return {};
    };
    // The hints a footer that gave up every rank up to @p dropped shows.
    auto const hintsUpTo = [&](uint8_t dropped) {
        auto text = std::string {};
        for (auto const& rule: SwitcherHintKeyRules)
        {
            if (rule.dropRank != 0 && rule.dropRank <= dropped)
                continue;
            auto const hint = hintOf(rule.key);
            if (hint.empty())
                continue;
            if (!text.empty())
                text += command::HintSeparator;
            text += hint;
        }
        return text;
    };
    auto const lastRank =
        std::ranges::max(SwitcherHintKeyRules | std::views::transform(&SwitcherHintKeyRule::dropRank));
    auto sets = std::vector<std::string> {};
    for (auto const dropped: std::views::iota(0, lastRank + 1))
    {
        auto hints = hintsUpTo(static_cast<uint8_t>(dropped));
        if (sets.empty() || sets.back() != hints)
            sets.push_back(std::move(hints));
    }
    return sets;
}

std::string switcherFooterHints(SwitcherHintSubject subject,
                                SwitcherCommand escape,
                                command::ShortcutStyle style,
                                session::SwitchMode mode)
{
    return switcherFooterHintSets(subject, escape, style, mode).front();
}

SwitcherLabelState switcherLabelState(std::string_view label,
                                      std::span<std::string const> runningLabels,
                                      command::ShortcutStyle style)
{
    auto const verdict = switcherLabelVerdict(label, runningLabels);
    auto state = SwitcherLabelState { .verdict = verdict,
                                      .message = {},
                                      .actionTitle = switcherLabelActionTitle(label, verdict),
                                      .equivalentCommand = {},
                                      .subject = SwitcherHintSubject::None };
    switch (verdict)
    {
        case LabelVerdict::Empty: break;
        case LabelVerdict::Available:
            state.message = "Available";
            state.equivalentCommand = std::format("contour daemon --label {}", label);
            state.subject = SwitcherHintSubject::CreateDaemon;
            break;
        case LabelVerdict::Running:
            state.message = std::format("A daemon named “{}” is already running — {} switches to it instead",
                                        label,
                                        keyText(vtbackend::Key::Enter, style));
            state.subject = SwitcherHintSubject::SwitchToRunning;
            break;
        case LabelVerdict::Invalid:
            state.message = vthost::describe(vthost::validateSocketLabel(label).error());
            break;
    }
    return state;
}

std::string switcherHintText(SwitcherView const& view, std::string_view daemonName)
{
    switch (view.level)
    {
        case SwitcherLevel::NewDaemon: return "Label, such as work2";
        case SwitcherLevel::Daemon: return std::format("Filter workspaces on {}…", daemonName);
        case SwitcherLevel::Top: break;
    }
    if (view.scope == SwitcherScope::OneDaemon)
        return std::format("Filter workspaces on {}…", daemonName);
    return "Switch workspace or daemon…";
}

std::string switcherEmptyText(std::string_view filter)
{
    return std::format("No workspace matches “{}”", filter);
}

std::string switcherLevelTitle(SwitcherView const& view, std::string_view daemonName)
{
    switch (view.level)
    {
        case SwitcherLevel::Daemon: return std::string { daemonName };
        case SwitcherLevel::NewDaemon: return "New Daemon";
        case SwitcherLevel::Top: break;
    }
    return {};
}

} // namespace contour::window
