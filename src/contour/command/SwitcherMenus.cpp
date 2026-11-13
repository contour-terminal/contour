// SPDX-License-Identifier: Apache-2.0
#include <contour/command/SwitcherMenus.hpp>

#include <format>

namespace contour::command
{

namespace
{
    [[nodiscard]] std::string enterText(ShortcutStyle style, vtbackend::Modifiers modifiers = {})
    {
        return shortcutText(ShortcutChord { .modifiers = modifiers, .input = vtbackend::Key::Enter }, style);
    }

} // namespace

std::vector<SwitcherMenuRow> buildSwitcherRowMenu(SwitcherRowMenuState const& state, ShortcutStyle style)
{
    auto rows = std::vector<SwitcherMenuRow> {};
    // Where Return opens a new window already, switching this window is not on offer.
    if (state.returnMode == SwitcherMenuReturn::Switch)
    {
        rows.emplace_back(SwitcherChoiceRow { .choice = SwitcherMenuChoice::Switch,
                                              .title = std::format("Switch to {}", state.name),
                                              .shortcut = enterText(style),
                                              .availability = state.shown == SwitcherMenuShown::ThisWindow
                                                                  ? RowAvailability::Disabled
                                                                  : RowAvailability::Enabled });
        rows.emplace_back(SwitcherChoiceRow { .choice = SwitcherMenuChoice::OpenInNewWindow,
                                              .title = "Open in New Window",
                                              .shortcut = enterText(style, vtbackend::Modifier::Shift),
                                              .availability = RowAvailability::Enabled });
    }
    else
        rows.emplace_back(SwitcherChoiceRow { .choice = SwitcherMenuChoice::OpenInNewWindow,
                                              .title = "Open in New Window",
                                              .shortcut = enterText(style),
                                              .availability = RowAvailability::Enabled });
    if (state.owner == SwitcherMenuOwner::OtherDaemon)
        return rows;

    auto separator = ContextMenuEntry {};
    separator.kind = ContextMenuEntryKind::Separator;
    rows.emplace_back(std::move(separator));
    for (auto& entry: buildWorkspaceMenu(state.workspace))
        rows.emplace_back(std::move(entry));
    return rows;
}

} // namespace contour::command
