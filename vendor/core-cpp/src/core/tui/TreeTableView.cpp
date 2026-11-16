// SPDX-License-Identifier: Apache-2.0
#include <core/tui/TreeTableView.hpp>

#include <core/tui/Canvas.hpp>
#include <core/tui/KeyCode.hpp>
#include <core/tui/Modifier.hpp>
#include <core/tui/Theme.hpp>

#include <algorithm>
#include <ranges>
#include <utility>

namespace core::tui
{

TreeTableView::TreeTableView(TreeTableModel& model, TreeTableConfig config, IClock const& clock) noexcept:
    _model(model), _config(config), _clock(clock)
{
}

void TreeTableView::clampCursor()
{
    auto const count = _model.rows().size();
    if (count == 0)
    {
        _cursor = 0;
        _scroll = 0;
        return;
    }
    _cursor = std::min(_cursor, count - 1);

    // Keep the cursor within the visible window (header takes one line when shown).
    auto const headerLines = _config.showHeader ? 1u : 0u;
    auto const areaHeight = static_cast<unsigned>(std::max(0, area().height));
    auto const viewport = (areaHeight > headerLines) ? areaHeight - headerLines : 1u;
    if (_cursor < _scroll)
        _scroll = _cursor;
    else if (_cursor >= _scroll + viewport)
        _scroll = _cursor - viewport + 1;
}

void TreeTableView::moveCursor(int delta)
{
    auto const count = _model.rows().size();
    if (count == 0)
        return;
    auto const target = static_cast<long long>(_cursor) + delta;
    _cursor = static_cast<std::size_t>(std::clamp<long long>(target, 0, static_cast<long long>(count) - 1));
    clampCursor();
    if (_onChanged)
        _onChanged();
}

void TreeTableView::moveToTop()
{
    _cursor = 0;
    clampCursor();
    if (_onChanged)
        _onChanged();
}

void TreeTableView::moveToBottom()
{
    auto const count = _model.rows().size();
    _cursor = (count == 0) ? 0 : count - 1;
    clampCursor();
    if (_onChanged)
        _onChanged();
}

void TreeTableView::pageBy(int direction)
{
    auto const headerLines = _config.showHeader ? 1 : 0;
    auto const viewport = std::max(1, area().height - headerLines);
    moveCursor(direction * viewport);
}

void TreeTableView::halfPageBy(int direction)
{
    auto const headerLines = _config.showHeader ? 1 : 0;
    auto const viewport = std::max(1, area().height - headerLines);
    moveCursor(direction * std::max(1, viewport / 2));
}

void TreeTableView::descendCursor()
{
    bool valid = false;
    auto const row = cursorRow(valid);
    if (!valid || !_model.canDescend(row))
        return;

    // Remember where we were so ascending restores the cursor.
    _cursorHistory.emplace_back(_model.currentTitle(), _cursor);
    if (_model.descend(row))
    {
        _cursor = 0;
        _scroll = 0;
        clampCursor();
        if (_onChanged)
            _onChanged();
    }
    else
    {
        _cursorHistory.pop_back();
    }
}

void TreeTableView::ascendCursor()
{
    if (!_model.ascend())
        return;

    // Restore the remembered cursor for the node we just returned to.
    _cursor = 0;
    if (!_cursorHistory.empty())
    {
        auto const& [title, idx] = _cursorHistory.back();
        if (title == _model.currentTitle())
        {
            _cursor = idx;
            _cursorHistory.pop_back();
        }
    }
    _scroll = 0;
    clampCursor();
    if (_onChanged)
        _onChanged();
}

void TreeTableView::sortBy(int sortKey)
{
    // Keep the cursor on the same row across the re-sort when possible.
    bool valid = false;
    auto const current = cursorRow(valid);
    _model.sortBy(sortKey);
    if (valid)
    {
        auto const rows = _model.rows();
        if (auto const it = std::ranges::find(rows, current); it != rows.end())
            _cursor = static_cast<std::size_t>(std::distance(rows.begin(), it));
    }
    clampCursor();
    if (_onChanged)
        _onChanged();
}

RowId TreeTableView::cursorRow(bool& valid) const
{
    auto const rows = _model.rows();
    if (_cursor >= rows.size())
    {
        valid = false;
        return 0;
    }
    valid = true;
    return rows[_cursor];
}

EventResult TreeTableView::onEvent(InputEvent const& event)
{
    // The view handles only generic motions; the host maps keys to these calls. We still
    // accept a few intrinsic keys so the component is usable standalone.
    if (auto const* key = std::get_if<KeyEvent>(&event))
    {
        // Ctrl-D / Ctrl-U: half-page down / up (vim).
        if (hasModifier(key->modifiers, Modifier::Ctrl))
        {
            switch (key->codepoint)
            {
                case U'd': halfPageBy(1); return EventResult::Handled;
                case U'u': halfPageBy(-1); return EventResult::Handled;
                default: break;
            }
        }

        switch (key->key)
        {
            case KeyCode::Up: moveCursor(-1); return EventResult::Handled;
            case KeyCode::Down: moveCursor(1); return EventResult::Handled;
            case KeyCode::PageUp: pageBy(-1); return EventResult::Handled;
            case KeyCode::PageDown: pageBy(1); return EventResult::Handled;
            case KeyCode::Enter: descendCursor(); return EventResult::Handled;
            case KeyCode::Backspace: ascendCursor(); return EventResult::Handled;
            default: break;
        }
        switch (key->codepoint)
        {
            case U'j': moveCursor(1); return EventResult::Handled;
            case U'k': moveCursor(-1); return EventResult::Handled;
            case U'l': descendCursor(); return EventResult::Handled;
            case U'h': ascendCursor(); return EventResult::Handled;
            case U'g': moveToTop(); return EventResult::Handled;
            case U'G': moveToBottom(); return EventResult::Handled;
            default: break;
        }
    }
    else if (auto const* mouse = std::get_if<MouseEvent>(&event))
    {
        // The wheel moves the selection. Most terminals translate wheel events to arrow keys in
        // the alternate screen, but some deliver real scroll events — handle both.
        if (mouse->type == MouseEvent::Type::ScrollUp)
        {
            moveCursor(-1);
            return EventResult::Handled;
        }
        if (mouse->type == MouseEvent::Type::ScrollDown)
        {
            moveCursor(1);
            return EventResult::Handled;
        }

        // Left-press selects the row under the pointer; a second left-press on the same row
        // within the double-click interval activates it (descends), like a desktop double-click.
        if (mouse->type == MouseEvent::Type::Press && mouse->button == 0)
        {
            auto const index = rowIndexAt(mouse->y);
            if (!index.has_value())
            {
                _hadClick = false; // a click off the rows breaks any pending double-click
                return EventResult::Ignored;
            }

            auto const now = _clock.now();
            auto const isDoubleClick =
                _hadClick && _lastClickRow == *index && (now - _lastClickTime) <= DoubleClickInterval;

            selectIndex(*index);

            if (isDoubleClick)
            {
                descendCursor();
                _hadClick = false; // reset so a third press starts a fresh click
            }
            else
            {
                _hadClick = true;
                _lastClickRow = *index;
                _lastClickTime = now;
            }
            return EventResult::Handled;
        }
    }
    return EventResult::Ignored;
}

std::optional<std::size_t> TreeTableView::rowIndexAt(int row) const
{
    auto const headerLines = _config.showHeader ? 1 : 0;
    auto const relRow = (row - 1) - headerLines; // mouse row is 1-based; the header sits above
    if (relRow < 0)
        return std::nullopt; // header (or above)
    auto const index = _scroll + static_cast<std::size_t>(relRow);
    if (index >= _model.rows().size())
        return std::nullopt; // empty space below the last row
    return index;
}

void TreeTableView::selectIndex(std::size_t index)
{
    _cursor = index;
    clampCursor();
    if (_onChanged)
        _onChanged();
}

std::vector<int> TreeTableView::computeWidths(int totalWidth) const
{
    auto const cols = _model.columns();
    std::vector<int> widths(cols.size(), 0);

    auto fixedTotal = 0;
    auto flexIndex = std::size_t { cols.size() };
    for (auto const i: std::views::iota(std::size_t { 0 }, cols.size()))
    {
        if (cols[i].width == ColumnWidth::Flex && flexIndex == cols.size())
            flexIndex = i;
        widths[i] = cols[i].size;
        if (cols[i].width == ColumnWidth::Fixed)
            fixedTotal += cols[i].size + 1; // +1 inter-column space
    }

    if (flexIndex != cols.size())
    {
        auto const leftover = totalWidth - fixedTotal - 1;
        widths[flexIndex] = std::max(cols[flexIndex].size, leftover);
    }
    return widths;
}

namespace
{
    /// Pads/truncates @p text to exactly @p width cells with @p align.
    [[nodiscard]] std::string fit(std::string text, int width, ColumnAlign align)
    {
        auto const w = static_cast<std::size_t>(std::max(width, 0));
        if (text.size() > w)
            text.resize(w);
        if (text.size() < w)
        {
            auto const pad = std::string(w - text.size(), ' ');
            text = (align == ColumnAlign::Right) ? pad + text : text + pad;
        }
        return text;
    }
} // namespace

void TreeTableView::renderRow(
    Canvas& canvas, int line, RowId row, bool selected, std::vector<int> const& widths)
{
    auto const cols = _model.columns();
    auto const rowStyle = _model.rowStyle(row, selected);
    auto style = rowStyle.style;
    if (selected)
        style.inverse = true;

    auto col = 0;
    for (auto const i: std::views::iota(std::size_t { 0 }, cols.size()))
    {
        auto cell = _model.cellText(row, i);

        // A designated bar column is filled proportionally by the model's text already
        // (the host formats the bar); the view only lays it out. No special-casing here
        // keeps the view domain-agnostic.
        auto const fitted = fit(std::move(cell), widths[i], cols[i].align);
        col += canvas.putString(line, col, fitted, style);
        col += 1; // inter-column space
    }
}

void TreeTableView::render(Canvas& canvas)
{
    clampCursor();
    auto const cols = _model.columns();
    auto const widths = computeWidths(canvas.width());

    auto line = 0;
    if (_config.showHeader)
    {
        auto headerStyle = canvas.theme().textBold;
        auto col = 0;
        for (auto const i: std::views::iota(std::size_t { 0 }, cols.size()))
        {
            auto const header = fit(std::string { cols[i].header }, widths[i], cols[i].align);
            col += canvas.putString(0, col, header, headerStyle);
            col += 1;
        }
        line = 1;
    }

    auto const rows = _model.rows();
    auto const headerLines = _config.showHeader ? 1 : 0;
    auto const viewport = static_cast<std::size_t>(std::max(0, canvas.height() - headerLines));
    for (auto const i: std::views::iota(std::size_t { 0 }, std::max(std::size_t { 0 }, viewport)))
    {
        auto const rowIndex = _scroll + i;
        if (rowIndex >= rows.size())
            break;
        renderRow(canvas, line + static_cast<int>(i), rows[rowIndex], rowIndex == _cursor, widths);
    }
}

} // namespace core::tui
