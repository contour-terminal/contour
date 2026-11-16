// SPDX-License-Identifier: Apache-2.0
#include <core/tui/CommandPalettePopup.hpp>

#include <core/tui/Box.hpp>
#include <core/tui/Canvas.hpp>
#include <core/tui/PopupKeyDispatch.hpp>
#include <core/tui/Theme.hpp>
#include <core/tui/Unicode.hpp>
#include <core/tui/completer/FuzzyMatch.hpp>

#include <algorithm>
#include <ranges>

namespace core::tui
{

// ============================================================================
// Component Interface
// ============================================================================

CommandPalettePopup::CommandPalettePopup(): CommandPalettePopup(defaultSteadyClock())
{
}

CommandPalettePopup::CommandPalettePopup(IClock const& clock):
    CommandPalettePopup(BorderStyle::Rounded, clock)
{
}

CommandPalettePopup::CommandPalettePopup(BorderStyle border, IClock const& clock):
    _border(border), _filterField(clock)
{
}

void CommandPalettePopup::render(Canvas& canvas)
{
    if (!visible())
        return;

    auto const& theme = canvas.theme();

    auto const visibleCount = std::min(MaxVisibleItems, _filteredItems.size());
    auto const paletteWidth = calculateWidth(canvas.width());
    auto const paletteHeight = static_cast<int>(visibleCount) + 4;

    _renderedHeight = paletteHeight;
    _renderedWidth = paletteWidth;

    auto const innerWidth = paletteWidth - 2;

    auto const borderRect = Rect { .x = 0, .y = 0, .width = paletteWidth, .height = paletteHeight };
    canvas.drawBox(borderRect, _border, theme.dialogBorder);

    // Filter input field
    auto const filterRow = 1;
    auto const filterBg = theme.completionItem;
    canvas.fill(Rect { .x = 1, .y = filterRow, .width = innerWidth, .height = 1 }, ' ', filterBg);

    auto const promptText = _title.empty() ? std::string_view("> ") : std::string_view(_title);
    auto promptStyle = Style {};
    promptStyle.fg = theme.agentColors.leftBar;
    auto promptCol = 1 + canvas.putString(filterRow, 1, promptText, promptStyle);

    auto const filterText = _filterField.text();
    if (!filterText.empty())
        canvas.putString(filterRow, promptCol, filterText, filterBg);

    // Separator
    auto const sepRow = 2;
    for (auto const col: std::views::iota(1, std::max(1, paletteWidth - 1)))
        canvas.put(sepRow, col, "\xe2\x94\x80", theme.dialogBorder);

    if (_selection.scrollOffset() > 0)
        canvas.put(sepRow, paletteWidth - 2, "\u25B2", theme.textMuted);

    // Items
    for (auto const i: std::views::iota(std::size_t { 0 }, visibleCount))
    {
        auto const itemIndex = _selection.scrollOffset() + i;
        auto const& filteredItem = _filteredItems[itemIndex];
        auto const* entry = filteredItem.entry;
        auto const isSelected = (itemIndex == _selection.selected());

        auto const row = static_cast<int>(i) + 3;

        auto const& itemStyle = isSelected ? theme.completionSelected : theme.completionItem;
        auto const& matchStyle = theme.completionMatch;

        canvas.fill(Rect { .x = 1, .y = row, .width = innerWidth, .height = 1 }, ' ', itemStyle);

        auto col = 1;

        if (!entry->category.empty())
        {
            auto const catText = entry->category + ": ";
            col += canvas.putString(row, col, catText, theme.completionDesc);
        }

        if (!filteredItem.matchPositions.empty())
        {
            col += putStringWithHighlights(
                canvas, row, col, entry->label, itemStyle, matchStyle, filteredItem.matchPositions);
        }
        else
        {
            col += canvas.putString(row, col, entry->label, itemStyle);
        }

        if (!entry->keybinding.empty())
        {
            auto const kbWidth = stringWidth(entry->keybinding);
            auto const kbCol = 1 + innerWidth - kbWidth;
            if (kbCol > col + 2)
                canvas.putString(row, kbCol, entry->keybinding, theme.textMuted);
        }
        else if (!entry->description.empty())
        {
            auto const descWidth = stringWidth(entry->description);
            auto const descCol = 1 + innerWidth - descWidth;
            if (descCol > col + 2)
                canvas.putString(row, descCol, entry->description, theme.completionDesc);
        }
    }

    if (_selection.scrollOffset() + visibleCount < _filteredItems.size())
    {
        auto const bottomRow = paletteHeight - 1;
        canvas.put(bottomRow, paletteWidth - 2, "\u25BC", theme.textMuted);
    }
}

EventResult CommandPalettePopup::onEvent(InputEvent const& event)
{
    if (!visible())
        return EventResult::Ignored;

    auto const action = processEvent(event);
    if (action != CommandPaletteAction::Changed)
        return EventResult::Handled;
    invalidate();
    return EventResult::Handled;
}

Size CommandPalettePopup::preferredSize() const
{
    if (!visible())
        return { .width = 0, .height = 0 };

    auto const visibleCount = std::min(MaxVisibleItems, _filteredItems.size());
    auto const height = static_cast<int>(visibleCount) + 4;
    return { .width = MaxPaletteWidth, .height = height };
}

// ============================================================================
// Visibility and Items
// ============================================================================

void CommandPalettePopup::show(CommandRegistry const& registry, CommandContext context)
{
    _allItems.clear();
    _title.clear();

    auto const filtered = registry.commandsForContext(context);
    _allItems.reserve(filtered.size());
    for (auto const* entry: filtered)
        _allItems.push_back(*entry);

    _filterField.clear();
    _filterField.setPrompt("> ");
    _selection.reset();
    _visible = true;

    refilter();
}

void CommandPalettePopup::showSubMenu(std::vector<CommandEntry> items, std::string_view title)
{
    _allItems = std::move(items);
    _title = std::string(title) + "> ";
    _filterField.clear();
    _selection.reset();
    _visible = true;

    refilter();
}

void CommandPalettePopup::hide()
{
    _visible = false;
    _allItems.clear();
    _filteredItems.clear();
    _filterField.clear();
    _title.clear();
    _selection.reset();
}

bool CommandPalettePopup::visible() const noexcept
{
    return _visible;
}

// ============================================================================
// Event Handling
// ============================================================================

CommandPaletteAction CommandPalettePopup::processEvent(InputEvent const& event)
{
    if (!visible())
        return CommandPaletteAction::Dismissed;

    if (auto const* key = std::get_if<KeyEvent>(&event))
        return handleKey(*key);

    if (std::holds_alternative<PasteEvent>(event))
    {
        auto const action = _filterField.processEvent(event);
        if (action == InputFieldAction::Changed)
        {
            refilter();
            return CommandPaletteAction::Changed;
        }
    }

    return CommandPaletteAction::Changed;
}

CommandPaletteAction CommandPalettePopup::handleKey(KeyEvent const& key)
{
    auto const mods = withoutLockKeys(key.modifiers);

    if (key.key == KeyCode::Escape)
    {
        hide();
        return CommandPaletteAction::Dismissed;
    }

    if (key.key == KeyCode::Enter && mods == Modifier::None)
    {
        executeSelected();
        return CommandPaletteAction::Executed;
    }

    // Navigation via shared key dispatch
    auto const nav = classifyNavigationKey(key);
    if (nav != PopupNavAction::None)
    {
        switch (nav)
        {
            case PopupNavAction::SelectNext: _selection.selectNext(_filteredItems.size()); break;
            case PopupNavAction::SelectPrev: _selection.selectPrev(_filteredItems.size()); break;
            case PopupNavAction::PageDown: _selection.pageDown(_filteredItems.size()); break;
            case PopupNavAction::PageUp: _selection.pageUp(_filteredItems.size()); break;
            case PopupNavAction::SelectFirst: _selection.selectFirst(_filteredItems.size()); break;
            case PopupNavAction::SelectLast: _selection.selectLast(_filteredItems.size()); break;
            case PopupNavAction::None: break;
        }
        return CommandPaletteAction::Changed;
    }

    // Forward remaining keys to filter field
    auto const action = _filterField.processEvent(InputEvent { key });
    if (action == InputFieldAction::Changed)
    {
        refilter();
        return CommandPaletteAction::Changed;
    }

    return CommandPaletteAction::Changed;
}

int CommandPalettePopup::renderedHeight() const noexcept
{
    return _renderedHeight;
}

int CommandPalettePopup::renderedWidth() const noexcept
{
    return _renderedWidth;
}

// ============================================================================
// Filtering
// ============================================================================

void CommandPalettePopup::refilter()
{
    _filteredItems.clear();

    auto const filterText = _filterField.text();

    if (filterText.empty())
    {
        for (auto const& item: _allItems)
            _filteredItems.push_back(FilteredItem { .entry = &item, .matchPositions = {}, .score = 0 });
    }
    else
    {
        for (auto const& item: _allItems)
        {
            auto result = completer::FuzzyMatch::matchSmartCase(item.label, filterText);
            if (result.matches)
            {
                auto const score = completer::FuzzyMatch::calculateScore(100, item.label, filterText, result);
                _filteredItems.push_back(FilteredItem {
                    .entry = &item, .matchPositions = std::move(result.positions), .score = score });
                continue;
            }

            auto const combined = item.category + " " + item.label;
            result = completer::FuzzyMatch::matchSmartCase(combined, filterText);
            if (result.matches)
            {
                auto const score = completer::FuzzyMatch::calculateScore(50, combined, filterText, result);
                auto const categoryLen = completer::FuzzyMatch::countGraphemes(item.category) + 1;
                auto labelPositions = std::vector<size_t> {};
                for (auto pos: result.positions)
                {
                    if (pos >= categoryLen)
                        labelPositions.push_back(pos - categoryLen);
                }
                _filteredItems.push_back(FilteredItem {
                    .entry = &item, .matchPositions = std::move(labelPositions), .score = score });
                continue;
            }

            result = completer::FuzzyMatch::matchSmartCase(item.description, filterText);
            if (result.matches)
            {
                auto const score =
                    completer::FuzzyMatch::calculateScore(30, item.description, filterText, result);
                _filteredItems.push_back(
                    FilteredItem { .entry = &item, .matchPositions = {}, .score = score });
            }
        }

        std::ranges::sort(_filteredItems, [](auto const& a, auto const& b) { return a.score > b.score; });
    }

    _selection.reset();
}

int CommandPalettePopup::calculateWidth(int maxWidth) const
{
    auto width = MinPaletteWidth;

    for (auto const& item: _filteredItems)
    {
        auto const* entry = item.entry;
        auto itemWidth = 2;
        if (!entry->category.empty())
            itemWidth += stringWidth(entry->category) + 2;
        itemWidth += stringWidth(entry->label);
        if (!entry->keybinding.empty())
            itemWidth += 2 + stringWidth(entry->keybinding);
        else if (!entry->description.empty())
            itemWidth += 2 + stringWidth(entry->description);

        width = std::max(width, itemWidth);
    }

    return std::min({ width, MaxPaletteWidth, maxWidth });
}

void CommandPalettePopup::executeSelected()
{
    if (_selection.selected() < _filteredItems.size())
    {
        auto action = _filteredItems[_selection.selected()].entry->action;
        hide();
        if (action)
            action();
    }
}

} // namespace core::tui
