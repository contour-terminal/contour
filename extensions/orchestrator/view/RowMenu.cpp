// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/view/RowMenu.hpp>

#include <core/tui/Canvas.hpp>
#include <core/tui/KeyCode.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>

#include <orchestrator/view/Sections.hpp>
#include <orchestrator/view/Theme.hpp>

namespace orchestrator::view
{

namespace
{
    /// The cells around an entry's label: the box and a blank on either side.
    constexpr auto SideColumns = 4;
} // namespace

bool menuApplies(RowMenuCommand command, WorkItem const& item) noexcept
{
    switch (command)
    {
        case RowMenuCommand::OpenMr: return item.changeRequest.has_value();
        case RowMenuCommand::OpenTicket: return item.ticket.has_value() && !item.ticket->url.empty();
        case RowMenuCommand::StartAgent:
        case RowMenuCommand::FoldGroup: break;
    }
    return true;
}

WorkItem const* workItemOf(std::span<WorkItem const> items, std::string_view key) noexcept
{
    auto const found =
        std::ranges::find_if(items, [key](WorkItem const& item) { return workItemKey(item) == key; });
    return found != items.end() ? &*found : nullptr;
}

std::optional<RowMenuCommand> applyRowMenuKey(core::tui::KeyEvent const& key,
                                              std::optional<RowMenuState>& menu,
                                              WorkItem const* item)
{
    if (!menu)
        return std::nullopt;
    if (item == nullptr)
    {
        menu.reset();
        return std::nullopt;
    }
    using core::tui::KeyCode;
    auto& highlighted = menu->highlighted;
    switch (key.key)
    {
        case KeyCode::Escape: menu.reset(); return std::nullopt;
        case KeyCode::Up: highlighted = highlighted == 0 ? 0 : highlighted - 1; return std::nullopt;
        case KeyCode::Down:
            highlighted = std::min(highlighted + 1, RowMenuEntries.size() - 1);
            return std::nullopt;
        case KeyCode::Enter: {
            if (highlighted >= RowMenuEntries.size())
                return std::nullopt;
            auto const command = RowMenuEntries[highlighted].command;
            if (!menuApplies(command, *item))
                return std::nullopt;
            menu.reset();
            return command;
        }
        default: break;
    }
    return std::nullopt;
}

void RowMenuView::render(core::tui::Canvas& canvas)
{
    if (!_menu)
        return;
    auto const& theme = canvas.theme();
    canvas.clear(styleOf(theme, Role::Text));
    drawFrame(canvas, {});
    auto inside = insideFrame(canvas);
    auto const hits = HitRecorder { _hits, { .x = screenBounds().x, .y = screenBounds().y } };
    // The menu covers what lies beneath it.
    hits.add(canvas.area(), Hit { .kind = HitKind::Popup });
    auto const* const item = workItemOf(_items, _menu->itemKey);
    for (auto const [index, entry]: std::views::enumerate(RowMenuEntries))
    {
        auto const line = static_cast<int>(index);
        if (line >= inside.height())
            break;
        auto const applies = item != nullptr && menuApplies(entry.command, *item);
        auto const isHighlighted = static_cast<std::size_t>(index) == _menu->highlighted;
        auto style = styleOf(theme, applies ? Role::Text : Role::Dim);
        if (isHighlighted)
        {
            style.bg = styleOf(theme, Role::Selection).bg;
            inside.fill(
                core::tui::Rect { .x = 0, .y = line, .width = inside.width(), .height = 1 }, ' ', style);
        }
        inside.putString(line, 1, cut(entry.label, inside.width() - 2), style);
        hits.shifted(1, 1).add(core::tui::Rect { .x = 0, .y = line, .width = inside.width(), .height = 1 },
                               Hit { .kind = HitKind::MenuEntry, .value = static_cast<std::size_t>(index) });
    }
}

core::tui::Size RowMenuView::preferredSize() const
{
    auto widest = 0;
    for (auto const& entry: RowMenuEntries)
        widest = std::max(widest, columnsOf(entry.label));
    return { .width = widest + SideColumns, .height = static_cast<int>(RowMenuEntries.size()) + 2 };
}

} // namespace orchestrator::view
