// SPDX-License-Identifier: Apache-2.0
#include <core/tui/FuzzyPickerPopup.hpp>

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

FuzzyPickerPopup::FuzzyPickerPopup(): FuzzyPickerPopup(defaultSteadyClock())
{
}

FuzzyPickerPopup::FuzzyPickerPopup(IClock const& clock): FuzzyPickerPopup(BorderStyle::Rounded, clock)
{
}

FuzzyPickerPopup::FuzzyPickerPopup(BorderStyle border, IClock const& clock):
    _border(border), _filterField(clock)
{
    setVisible(false);
}

void FuzzyPickerPopup::render(Canvas& canvas)
{
    if (!visible())
        return;

    auto const& theme = canvas.theme();

    // Clamp visible items to what fits in the canvas (4 rows for borders, filter, separator).
    auto const maxByCanvas = canvas.height() >= 5 ? static_cast<size_t>(canvas.height() - 4) : size_t { 1 };
    auto const visibleCount = std::min({ MaxVisibleItems, _filteredItems.size(), maxByCanvas });
    _selection.setMaxVisible(visibleCount);
    _selection.ensureSelectedVisible(_filteredItems.size());
    auto const pickerWidth = calculateWidth(canvas.width());
    auto const pickerHeight = static_cast<int>(visibleCount) + 4;

    _renderedHeight = pickerHeight;
    _renderedWidth = pickerWidth;

    auto const innerWidth = pickerWidth - 2;

    auto const borderRect = Rect { .x = 0, .y = 0, .width = pickerWidth, .height = pickerHeight };
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
    for (auto const col: std::views::iota(1, std::max(1, pickerWidth - 1)))
        canvas.put(sepRow, col, "\xe2\x94\x80", theme.dialogBorder);

    if (_selection.scrollOffset() > 0)
        canvas.put(sepRow, pickerWidth - 2, "\u25B2", theme.textMuted);

    // Items
    for (auto const i: std::views::iota(std::size_t { 0 }, visibleCount))
    {
        auto const itemIndex = _selection.scrollOffset() + i;
        auto const& filteredItem = _filteredItems[itemIndex];
        auto const isSelected = (itemIndex == _selection.selected());

        auto const row = static_cast<int>(i) + 3;

        auto const& itemStyle = isSelected ? theme.completionSelected : theme.completionItem;
        auto const& matchStyle = theme.completionMatch;

        canvas.fill(Rect { .x = 1, .y = row, .width = innerWidth, .height = 1 }, ' ', itemStyle);

        auto col = 1;
        if (!filteredItem.matchPositions.empty())
        {
            col += putStringWithHighlights(
                canvas, row, col, *filteredItem.item, itemStyle, matchStyle, filteredItem.matchPositions);
        }
        else
        {
            col += canvas.putString(row, col, *filteredItem.item, itemStyle);
        }
        (void) col;
    }

    if (_selection.scrollOffset() + visibleCount < _filteredItems.size())
    {
        auto const bottomRow = pickerHeight - 1;
        canvas.put(bottomRow, pickerWidth - 2, "\u25BC", theme.textMuted);
    }
}

EventResult FuzzyPickerPopup::onEvent(InputEvent const& event)
{
    if (!visible())
        return EventResult::Ignored;

    auto const action = processEvent(event);
    if (action != FuzzyPickerAction::Changed)
        return EventResult::Handled;
    invalidate();
    return EventResult::Handled;
}

Size FuzzyPickerPopup::preferredSize() const
{
    if (!visible())
        return { .width = 0, .height = 0 };

    auto const visibleCount = std::min(MaxVisibleItems, _filteredItems.size());
    auto const height = static_cast<int>(visibleCount) + 4; // borders + filter + separator
    return { .width = MaxPickerWidth, .height = height };
}

// ============================================================================
// Visibility and Items
// ============================================================================

void FuzzyPickerPopup::show(std::vector<std::string> items, std::string_view title)
{
    _allItems = std::move(items);
    _title = title.empty() ? "" : std::string(title);
    _filterField.clear();
    _filterField.setPrompt("> ");
    _selection.reset();
    setVisible(true);

    refilter();
}

void FuzzyPickerPopup::hide()
{
    setVisible(false);
    _allItems.clear();
    _filteredItems.clear();
    _filterField.clear();
    _title.clear();
    _selection.reset();
}

std::string const* FuzzyPickerPopup::selectedItem() const
{
    if (_selection.selected() < _filteredItems.size())
        return _filteredItems[_selection.selected()].item;
    return nullptr;
}

// ============================================================================
// Event Handling
// ============================================================================

FuzzyPickerAction FuzzyPickerPopup::processEvent(InputEvent const& event)
{
    if (!visible())
        return FuzzyPickerAction::Dismissed;

    if (auto const* key = std::get_if<KeyEvent>(&event))
        return handleKey(*key);

    if (std::holds_alternative<PasteEvent>(event))
    {
        auto const action = _filterField.processEvent(event);
        if (action == InputFieldAction::Changed)
        {
            refilter();
            return FuzzyPickerAction::Changed;
        }
    }

    return FuzzyPickerAction::Changed;
}

FuzzyPickerAction FuzzyPickerPopup::handleKey(KeyEvent const& key)
{
    auto const mods = withoutLockKeys(key.modifiers);

    if (key.key == KeyCode::Escape)
    {
        hide();
        return FuzzyPickerAction::Dismissed;
    }

    if (key.key == KeyCode::Enter && mods == Modifier::None)
        return FuzzyPickerAction::Accepted;

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
        return FuzzyPickerAction::Changed;
    }

    // Forward remaining keys to filter field
    auto const action = _filterField.processEvent(InputEvent { key });
    if (action == InputFieldAction::Changed)
    {
        refilter();
        return FuzzyPickerAction::Changed;
    }

    return FuzzyPickerAction::Changed;
}

int FuzzyPickerPopup::renderedHeight() const noexcept
{
    return _renderedHeight;
}

int FuzzyPickerPopup::renderedWidth() const noexcept
{
    return _renderedWidth;
}

// ============================================================================
// Filtering
// ============================================================================

void FuzzyPickerPopup::refilter()
{
    _filteredItems.clear();

    auto const filterText = _filterField.text();

    if (filterText.empty())
    {
        for (auto const& item: _allItems)
            _filteredItems.push_back(FilteredItem { .item = &item, .matchPositions = {}, .score = 0 });
    }
    else
    {
        for (auto const& item: _allItems)
        {
            auto ciResult = completer::FuzzyMatch::match(item, filterText, /*caseSensitive=*/false);
            if (!ciResult.matches)
                continue;

            auto score = completer::FuzzyMatch::calculateScore(100, item, filterText, ciResult);

            // Tiered bonus: case-sensitive substring > case-insensitive substring > fuzzy
            if (ciResult.isContiguousSubstring())
            {
                auto csResult = completer::FuzzyMatch::match(item, filterText, /*caseSensitive=*/true);
                if (csResult.matches && csResult.isContiguousSubstring())
                    score += 400; // Tier 1: exact case substring match
                else
                    score += 200; // Tier 2: case-insensitive substring match
            }

            // Boost by coverage: more of the path covered = higher rank
            if (ciResult.textGraphemeCount > 0)
            {
                auto const coveragePercent =
                    static_cast<int>((static_cast<double>(ciResult.patternGraphemeCount)
                                      / static_cast<double>(ciResult.textGraphemeCount))
                                     * 150);
                score += coveragePercent;
            }

            _filteredItems.push_back(FilteredItem {
                .item = &item, .matchPositions = std::move(ciResult.positions), .score = score });
        }

        std::ranges::sort(_filteredItems, [](auto const& a, auto const& b) { return a.score > b.score; });
    }

    _selection.reset();
}

int FuzzyPickerPopup::calculateWidth(int maxWidth) const
{
    auto const clampedMax = std::min(MaxPickerWidth, maxWidth);
    auto width = MinPickerWidth;

    for (auto const& item: _filteredItems)
    {
        auto const itemWidth = 2 + stringWidth(*item.item);
        width = std::max(width, itemWidth);
        if (width >= clampedMax)
            return clampedMax;
    }

    return std::min(width, clampedMax);
}

} // namespace core::tui
