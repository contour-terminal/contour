// SPDX-License-Identifier: Apache-2.0
#include <core/tui/List.hpp>

#include <core/tui/Canvas.hpp>
#include <core/tui/Theme.hpp>

#include <algorithm>
#include <cctype>
#include <ranges>
#include <utility>

namespace core::tui
{

namespace
{

    auto toLower(std::string_view str) -> std::string
    {
        auto result = std::string {};
        result.reserve(str.size());
        for (auto ch: str)
            result += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return result;
    }

    auto containsIgnoreCase(std::string_view haystack, std::string_view needle) -> bool
    {
        if (needle.empty())
            return true;

        auto const lowerHaystack = toLower(haystack);
        auto const lowerNeedle = toLower(needle);
        return lowerHaystack.contains(lowerNeedle);
    }

} // namespace

List::List(std::vector<ListItem> items): _items(std::move(items))
{
    _style = defaultListStyle();
    _checked.assign(_items.size(), false);
    rebuildVisibleIndices();
}

// =============================================================================
// Component Interface
// =============================================================================

void List::render(Canvas& canvas)
{
    auto const width = canvas.width();
    auto const maxRows = canvas.height();

    if (_visibleIndices.empty())
    {
        canvas.putString(0, 0, "  (no items)", _style.disabled);
        return;
    }

    ensureSelectionVisible(maxRows);

    auto const visibleCount =
        std::min(static_cast<std::size_t>(maxRows), _visibleIndices.size() - _scrollOffset);

    for (auto const i: std::views::iota(std::size_t { 0 }, static_cast<std::size_t>(std::max(0, maxRows))))
    {
        auto const row = static_cast<int>(i);

        if (i >= visibleCount)
        {
            // Clear remaining rows
            canvas.fill(Rect { .x = 0, .y = row, .width = width, .height = 1 }, ' ', _style.normal);
            continue;
        }

        auto const visibleIdx = _scrollOffset + i;
        auto const itemIdx = _visibleIndices[visibleIdx];
        auto const& item = _items[itemIdx];
        auto const isSelected = (visibleIdx == _selectedVisibleIndex);

        // Determine style
        auto const& itemStyle = [&]() -> Style const& {
            if (!item.enabled)
                return _style.disabled;
            return isSelected ? _style.selected : _style.normal;
        }();

        // Fill row background
        canvas.fill(Rect { .x = 0, .y = row, .width = width, .height = 1 }, ' ', itemStyle);

        // Cursor / checkbox indicator
        auto col = 0;
        if (_multiSelect)
        {
            // Multi-select: show checkbox prefix, highlight current row
            auto const itemChecked = (itemIdx < _checked.size()) && _checked[itemIdx];
            auto const prefix = itemChecked ? _style.checked : _style.unchecked;
            auto const& prefixStyle = isSelected ? _style.selected : _style.normal;
            col = canvas.putString(row, col, prefix, prefixStyle);
        }
        else if (isSelected)
        {
            col = canvas.putString(row, col, _style.cursor, _style.selected);
        }
        else
        {
            col = canvas.putString(row, col, _style.noCursor, _style.normal);
        }

        // Item label
        auto const labelMaxWidth = width - col;
        auto displayLabel = item.label;
        if (std::cmp_greater(displayLabel.size(), labelMaxWidth))
            displayLabel = displayLabel.substr(0, static_cast<std::size_t>(labelMaxWidth - 1)) + "\u2026";

        canvas.putString(row, col, displayLabel, itemStyle);
    }

    // Show scroll indicators if needed
    if (_visibleIndices.size() > static_cast<std::size_t>(maxRows))
    {
        auto const hasMore = (_scrollOffset + static_cast<std::size_t>(maxRows) < _visibleIndices.size());
        auto const hasLess = (_scrollOffset > 0);

        if (hasLess)
        {
            canvas.put(0, width - 2, "\u25B2", _style.disabled); // Up arrow
        }

        if (hasMore)
        {
            canvas.put(maxRows - 1, width - 2, "\u25BC", _style.disabled); // Down arrow
        }
    }
}

EventResult List::onEvent(InputEvent const& event)
{
    auto action = processEvent(event);

    switch (action)
    {
        case ListAction::Changed:
        case ListAction::Selected:
        case ListAction::Cancelled:
        case ListAction::Toggled: invalidate(); return EventResult::Handled;
        case ListAction::None: return EventResult::Ignored;
    }
    return EventResult::Ignored;
}

Size List::preferredSize() const
{
    if (_items.empty())
        return { .width = 20, .height = 1 };

    // Calculate max width from items
    int maxWidth = 0;
    for (auto const& item: _items)
    {
        auto const itemWidth = static_cast<int>(_style.cursor.size()) + static_cast<int>(item.label.size());
        maxWidth = std::max(maxWidth, itemWidth);
    }

    // Height is the number of visible items (capped at a reasonable default)
    auto const height = std::min(static_cast<int>(_visibleIndices.size()), 20);

    return { .width = maxWidth + 2, .height = height }; // +2 for scroll indicator space
}

void List::setItems(std::vector<ListItem> items)
{
    _items = std::move(items);
    _checked.assign(_items.size(), false);
    _selectedVisibleIndex = 0;
    _scrollOffset = 0;
    rebuildVisibleIndices();
}

auto List::items() const noexcept -> std::vector<ListItem> const&
{
    return _items;
}

auto List::size() const noexcept -> std::size_t
{
    return _items.size();
}

auto List::empty() const noexcept -> bool
{
    return _items.empty();
}

auto List::selectedIndex() const noexcept -> std::size_t
{
    if (_visibleIndices.empty() || _selectedVisibleIndex >= _visibleIndices.size())
        return 0;
    return _visibleIndices[_selectedVisibleIndex];
}

auto List::selectedItem() const noexcept -> std::optional<ListItem const*>
{
    if (_visibleIndices.empty() || _selectedVisibleIndex >= _visibleIndices.size())
        return std::nullopt;
    return &_items[_visibleIndices[_selectedVisibleIndex]];
}

void List::setSelectedIndex(std::size_t index)
{
    // Find the visible index corresponding to the item index
    for (auto const i: std::views::iota(std::size_t { 0 }, _visibleIndices.size()))
    {
        if (_visibleIndices[i] == index)
        {
            _selectedVisibleIndex = i;
            return;
        }
    }
}

void List::setFilter(std::string_view filter)
{
    if (_filter == filter)
        return;

    _filter = filter;
    rebuildVisibleIndices();
}

auto List::filter() const noexcept -> std::string_view
{
    return _filter;
}

void List::clearFilter()
{
    setFilter("");
}

auto List::visibleItems() const noexcept -> std::vector<std::size_t> const&
{
    return _visibleIndices;
}

auto List::processEvent(InputEvent const& event) -> ListAction
{
    if (auto const* key = std::get_if<KeyEvent>(&event))
        return handleKey(*key);

    return ListAction::None;
}

auto List::handleKey(KeyEvent const& key) -> ListAction
{
    // Handle vim-style navigation (j/k) and multi-select toggle (Space) for printable keys
    if (isTextProducingKey(key.key) && withoutLockKeys(key.modifiers) == Modifier::None)
    {
        if (key.codepoint == ' ' && _multiSelect && !_visibleIndices.empty())
        {
            toggleChecked();
            return ListAction::Toggled;
        }
        if (key.codepoint == 'j')
        {
            selectNext();
            return ListAction::Changed;
        }
        if (key.codepoint == 'k')
        {
            selectPrevious();
            return ListAction::Changed;
        }
    }

    switch (key.key)
    {
        case KeyCode::Up: selectPrevious(); return ListAction::Changed;

        case KeyCode::Down: selectNext(); return ListAction::Changed;

        case KeyCode::Home: selectFirst(); return ListAction::Changed;

        case KeyCode::End: selectLast(); return ListAction::Changed;

        case KeyCode::PageUp: pageUp(10); return ListAction::Changed;

        case KeyCode::PageDown: pageDown(10); return ListAction::Changed;

        case KeyCode::Enter:
            if (!_visibleIndices.empty())
            {
                auto const idx = selectedIndex();
                if (_items[idx].enabled)
                    return ListAction::Selected;
            }
            break;

        case KeyCode::Escape: return ListAction::Cancelled;

        default: break;
    }

    return ListAction::None;
}

void List::setStyle(ListStyle style)
{
    _style = style;
}

auto List::style() const noexcept -> ListStyle const&
{
    return _style;
}

void List::selectNext()
{
    if (_visibleIndices.empty())
        return;

    // Find next enabled item
    auto start = _selectedVisibleIndex;
    do
    {
        if (_selectedVisibleIndex < _visibleIndices.size() - 1)
            ++_selectedVisibleIndex;
        else
            break; // Don't wrap

        auto const idx = _visibleIndices[_selectedVisibleIndex];
        if (_items[idx].enabled)
            return;
    } while (_selectedVisibleIndex != start);
}

void List::selectPrevious()
{
    if (_visibleIndices.empty())
        return;

    auto start = _selectedVisibleIndex;
    do
    {
        if (_selectedVisibleIndex > 0)
            --_selectedVisibleIndex;
        else
            break; // Don't wrap

        auto const idx = _visibleIndices[_selectedVisibleIndex];
        if (_items[idx].enabled)
            return;
    } while (_selectedVisibleIndex != start);
}

void List::selectFirst()
{
    for (auto const i: std::views::iota(std::size_t { 0 }, _visibleIndices.size()))
    {
        if (_items[_visibleIndices[i]].enabled)
        {
            _selectedVisibleIndex = i;
            _scrollOffset = 0;
            return;
        }
    }
}

void List::selectLast()
{
    for (auto const i: std::views::iota(std::size_t { 0 }, _visibleIndices.size()) | std::views::reverse)
    {
        if (_items[_visibleIndices[i]].enabled)
        {
            _selectedVisibleIndex = i;
            return;
        }
    }
}

void List::pageDown(int pageSize)
{
    for ([[maybe_unused]] auto const i: std::views::iota(0, std::max(0, pageSize)))
        selectNext();
}

void List::pageUp(int pageSize)
{
    for ([[maybe_unused]] auto const i: std::views::iota(0, std::max(0, pageSize)))
        selectPrevious();
}

void List::rebuildVisibleIndices()
{
    _visibleIndices.clear();

    for (auto const i: std::views::iota(std::size_t { 0 }, _items.size()))
    {
        if (matchesFilter(_items[i]))
            _visibleIndices.push_back(i);
    }

    // Reset selection to first enabled item
    _selectedVisibleIndex = 0;
    _scrollOffset = 0;

    for (auto const i: std::views::iota(std::size_t { 0 }, _visibleIndices.size()))
    {
        if (_items[_visibleIndices[i]].enabled)
        {
            _selectedVisibleIndex = i;
            break;
        }
    }
}

void List::ensureSelectionVisible(int maxRows) const
{
    if (_visibleIndices.empty())
        return;

    if (_selectedVisibleIndex < _scrollOffset)
        _scrollOffset = _selectedVisibleIndex;
    else if (_selectedVisibleIndex >= _scrollOffset + static_cast<std::size_t>(maxRows))
        _scrollOffset = _selectedVisibleIndex - static_cast<std::size_t>(maxRows) + 1;
}

auto List::matchesFilter(ListItem const& item) const -> bool
{
    if (_filter.empty())
        return true;

    auto const& text = item.filterText.empty() ? item.label : item.filterText;
    return containsIgnoreCase(text, _filter);
}

void List::setMultiSelect(bool enabled)
{
    _multiSelect = enabled;
}

bool List::multiSelect() const noexcept
{
    return _multiSelect;
}

auto List::checkedIndices() const -> std::vector<std::size_t>
{
    auto result = std::vector<std::size_t> {};
    for (auto const i: std::views::iota(std::size_t { 0 }, _checked.size()))
    {
        if (_checked[i])
            result.push_back(i);
    }
    return result;
}

bool List::isChecked(std::size_t index) const
{
    if (index >= _checked.size())
        return false;
    return _checked[index];
}

void List::setChecked(std::size_t index, bool checked)
{
    if (index < _checked.size())
        _checked[index] = checked;
}

void List::toggleChecked()
{
    if (_visibleIndices.empty() || _selectedVisibleIndex >= _visibleIndices.size())
        return;

    auto const itemIdx = _visibleIndices[_selectedVisibleIndex];
    if (itemIdx < _checked.size() && _items[itemIdx].enabled)
        _checked[itemIdx] = !_checked[itemIdx];
}

auto defaultListStyle() -> ListStyle
{
    auto style = ListStyle {};

    // Normal items: default color
    style.normal = Style {};

    // Selected items: inverse or bold with accent color
    style.selected = Style {};
    style.selected.bold = true;
    style.selected.fg = 0x82B4FF_rgb; // Light blue

    // Disabled items: dim
    style.disabled = Style {};
    style.disabled.dim = true;

    // Description: dim/gray
    style.description = Style {};
    style.description.dim = true;

    return style;
}

} // namespace core::tui
