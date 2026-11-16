// SPDX-License-Identifier: Apache-2.0
#include <core/tui/Dialog.hpp>

#include <core/tui/Canvas.hpp>
#include <core/tui/Theme.hpp>

#include <algorithm>
#include <utility>

namespace core::tui
{

// =============================================================================
// SelectDialog
// =============================================================================

SelectDialog::SelectDialog(SelectDialogConfig config):
    _config(std::move(config)), _list(std::move(_config.items))
{
    _list.setStyle(defaultListStyle());
}

// --- Component Interface ---

void SelectDialog::render(Canvas& canvas)
{
    auto const termCols = canvas.width();
    auto const termRows = canvas.height();

    // Dim background
    if (_config.dimBackground)
    {
        auto dimStyle = Style {};
        dimStyle.dim = true;
        canvas.fill(canvas.area(), ' ', dimStyle);
    }

    // Calculate dialog bounds
    auto const itemCount = static_cast<int>(_list.visibleItems().size());
    auto const contentHeight = std::min(itemCount, _config.maxHeight - 2);
    auto const dialogHeight = contentHeight + 2; // +2 for borders
    auto const dialogWidth = std::max(0, std::min(_config.width, termCols - 4));
    auto const startRow = (termRows - dialogHeight) / 2;
    auto const startCol = (termCols - dialogWidth) / 2;

    // Draw box. Rect::x is the left column and Rect::y the top row, which is the opposite order
    // from the (row, col) every putString() below takes.
    auto boxRect = Rect { .x = startCol, .y = startRow, .width = dialogWidth, .height = dialogHeight };
    canvas.drawBox(boxRect, _config.border, _config.borderStyle, _config.title, TitleAlign::Center);

    // Render list inside box (inner area)
    auto listRect =
        Rect { .x = startCol + 2, .y = startRow + 1, .width = dialogWidth - 4, .height = contentHeight };
    auto listCanvas = canvas.subcanvas(listRect);
    _list.render(listCanvas);

    // Render hint bar below dialog
    auto const hintRow = startRow + dialogHeight;
    auto const hintText =
        std::string(_config.confirmHint) + " select  " + std::string(_config.cancelHint) + " cancel";
    auto const hintCol = startCol + ((dialogWidth - static_cast<int>(hintText.size())) / 2);

    auto hintStyle = Style {};
    hintStyle.dim = true;
    canvas.putString(hintRow, hintCol, hintText, hintStyle);
}

EventResult SelectDialog::onEvent(InputEvent const& event)
{
    auto result = processEvent(event);

    switch (result)
    {
        case DialogResult::Changed:
        case DialogResult::Confirmed:
        case DialogResult::Cancelled: invalidate(); return EventResult::Handled;
        case DialogResult::None: return EventResult::Ignored;
    }
    return EventResult::Ignored;
}

Size SelectDialog::preferredSize() const
{
    auto const itemCount = static_cast<int>(_list.visibleItems().size());
    auto const contentHeight = std::min(itemCount, _config.maxHeight - 2);
    auto const dialogHeight = contentHeight + 3; // +2 for borders, +1 for hint
    return { .width = _config.width, .height = dialogHeight };
}

auto SelectDialog::processEvent(InputEvent const& event) -> DialogResult
{
    auto const action = _list.processEvent(event);

    switch (action)
    {
        case ListAction::Selected: return DialogResult::Confirmed;
        case ListAction::Cancelled: return DialogResult::Cancelled;
        case ListAction::Changed:
        case ListAction::Toggled: return DialogResult::Changed;
        case ListAction::None: break;
    }

    return DialogResult::None;
}

auto SelectDialog::selectedIndex() const noexcept -> std::size_t
{
    return _list.selectedIndex();
}

auto SelectDialog::selectedItem() const noexcept -> std::optional<ListItem const*>
{
    return _list.selectedItem();
}

void SelectDialog::setFilter(std::string_view filter)
{
    _list.setFilter(filter);
}

void SelectDialog::clearFilter()
{
    _list.clearFilter();
}

void SelectDialog::setConfig(SelectDialogConfig config)
{
    _config = std::move(config);
    _list.setItems(std::move(_config.items));
}

// =============================================================================
// ConfirmDialog
// =============================================================================

ConfirmDialog::ConfirmDialog(ConfirmDialogConfig config):
    _config(std::move(config)), _confirmSelected(_config.defaultConfirm)
{
}

// --- Component Interface ---

void ConfirmDialog::render(Canvas& canvas)
{
    auto const termCols = canvas.width();
    auto const termRows = canvas.height();

    auto const dialogWidth = std::max(0, std::min(_config.width, termCols - 4));
    auto const dialogHeight = 6; // title + padding + message + padding + buttons + border

    auto const startRow = (termRows - dialogHeight) / 2;
    auto const startCol = (termCols - dialogWidth) / 2;

    // Dim background
    if (_config.dimBackground)
    {
        auto dimStyle = Style {};
        dimStyle.dim = true;
        canvas.fill(canvas.area(), ' ', dimStyle);
    }

    // Draw box with title. Rect::x is the left column and Rect::y the top row, which is the
    // opposite order from the (row, col) every putString() below takes.
    auto boxRect = Rect { .x = startCol, .y = startRow, .width = dialogWidth, .height = dialogHeight };
    canvas.drawBox(boxRect, _config.border, _config.borderStyle, _config.title, TitleAlign::Center);

    // Fill box interior
    auto innerRect =
        Rect { .x = startCol + 1, .y = startRow + 1, .width = dialogWidth - 2, .height = dialogHeight - 2 };
    canvas.fill(innerRect, ' ', Style {});

    // Message (with padding)
    auto const messageRow = startRow + 2;
    auto const messageCol = startCol + 3;
    canvas.putString(messageRow, messageCol, _config.message, _config.messageStyle);

    // Buttons
    auto const buttonRow = startRow + dialogHeight - 2;

    auto confirmStyle = Style {};
    auto cancelStyle = Style {};

    if (_confirmSelected)
    {
        confirmStyle.inverse = true;
        confirmStyle.bold = true;
    }
    else
    {
        cancelStyle.inverse = true;
        cancelStyle.bold = true;
    }

    auto const confirmBtn = " " + _config.confirmLabel + " ";
    auto const cancelBtn = " " + _config.cancelLabel + " ";
    auto const buttonsWidth = static_cast<int>(confirmBtn.size() + cancelBtn.size() + 4);
    auto buttonsCol = startCol + ((dialogWidth - buttonsWidth) / 2);

    canvas.putString(buttonRow, buttonsCol, "[", _config.borderStyle);
    buttonsCol += 1;
    buttonsCol += canvas.putString(buttonRow, buttonsCol, confirmBtn, confirmStyle);
    canvas.putString(buttonRow, buttonsCol, "]", _config.borderStyle);
    buttonsCol += 1;
    buttonsCol += canvas.putString(buttonRow, buttonsCol, "  ", Style {});
    canvas.putString(buttonRow, buttonsCol, "[", _config.borderStyle);
    buttonsCol += 1;
    buttonsCol += canvas.putString(buttonRow, buttonsCol, cancelBtn, cancelStyle);
    canvas.putString(buttonRow, buttonsCol, "]", _config.borderStyle);
}

EventResult ConfirmDialog::onEvent(InputEvent const& event)
{
    auto result = processEvent(event);

    switch (result)
    {
        case DialogResult::Changed:
        case DialogResult::Confirmed:
        case DialogResult::Cancelled: invalidate(); return EventResult::Handled;
        case DialogResult::None: return EventResult::Ignored;
    }
    return EventResult::Ignored;
}

Size ConfirmDialog::preferredSize() const
{
    return { .width = _config.width, .height = 6 };
}

auto ConfirmDialog::processEvent(InputEvent const& event) -> DialogResult
{
    if (auto const* key = std::get_if<KeyEvent>(&event))
    {
        switch (key->key)
        {
            case KeyCode::Left: _confirmSelected = true; return DialogResult::Changed;

            case KeyCode::Right:
            case KeyCode::Tab: _confirmSelected = !_confirmSelected; return DialogResult::Changed;

            case KeyCode::Enter: return _confirmSelected ? DialogResult::Confirmed : DialogResult::Cancelled;

            case KeyCode::Escape: return DialogResult::Cancelled;

            default:
                // Handle y/n shortcuts
                if (isTextProducingKey(key->key))
                {
                    if (key->codepoint == 'y' || key->codepoint == 'Y')
                        return DialogResult::Confirmed;
                    if (key->codepoint == 'n' || key->codepoint == 'N')
                        return DialogResult::Cancelled;
                }
                break;
        }
    }

    return DialogResult::None;
}

auto ConfirmDialog::isConfirmSelected() const noexcept -> bool
{
    return _confirmSelected;
}

// =============================================================================
// InputDialog
// =============================================================================

InputDialog::InputDialog(InputDialogConfig config):
    _config(std::move(config)), _value(_config.initialValue), _cursor(_value.size())
{
}

// --- Component Interface ---

void InputDialog::render(Canvas& canvas)
{
    auto const termCols = canvas.width();
    auto const termRows = canvas.height();

    auto const dialogWidth = std::max(0, std::min(_config.width, termCols - 4));
    auto const dialogHeight = 5; // border + prompt + input + border

    auto const startRow = (termRows - dialogHeight) / 2;
    auto const startCol = (termCols - dialogWidth) / 2;

    // Dim background
    if (_config.dimBackground)
    {
        auto dimStyle = Style {};
        dimStyle.dim = true;
        canvas.fill(canvas.area(), ' ', dimStyle);
    }

    // Draw box with title. Rect::x is the left column and Rect::y the top row, which is the
    // opposite order from the (row, col) every putString() below takes.
    auto boxRect = Rect { .x = startCol, .y = startRow, .width = dialogWidth, .height = dialogHeight };
    canvas.drawBox(boxRect, _config.border, _config.borderStyle, _config.title, TitleAlign::Center);

    // Fill box interior
    auto innerRect =
        Rect { .x = startCol + 1, .y = startRow + 1, .width = dialogWidth - 2, .height = dialogHeight - 2 };
    canvas.fill(innerRect, ' ', Style {});

    // Inner content area. The canvas clips a box or a string that does not fit, but the input
    // field's own arithmetic does not: a negative width reached substr() as a huge size_t and
    // threw out of a render() no caller expects to throw.
    auto const inputWidth = std::max(0, dialogWidth - 4);
    auto const contentCol = startCol + 2;

    // Prompt
    auto const promptRow = startRow + 1;
    canvas.putString(promptRow, contentCol, _config.prompt, _config.titleStyle);

    // Input field
    auto const inputRow = promptRow + 1;

    if (_value.empty())
    {
        auto placeholderStyle = Style {};
        placeholderStyle.dim = true;
        canvas.putString(inputRow, contentCol, _config.placeholder, placeholderStyle);
    }
    else
    {
        auto displayValue = _value;
        if (std::cmp_greater(displayValue.size(), inputWidth))
            displayValue = displayValue.substr(displayValue.size() - static_cast<std::size_t>(inputWidth));

        canvas.putString(inputRow, contentCol, displayValue, _config.inputStyle);
    }

    // Position cursor
    auto cursorCol = contentCol + static_cast<int>(_cursor);
    cursorCol = std::min(cursorCol, contentCol + inputWidth - 1);

    canvas.setCursor(inputRow, cursorCol);
}

EventResult InputDialog::onEvent(InputEvent const& event)
{
    auto result = processEvent(event);

    switch (result)
    {
        case DialogResult::Changed:
        case DialogResult::Confirmed:
        case DialogResult::Cancelled: invalidate(); return EventResult::Handled;
        case DialogResult::None: return EventResult::Ignored;
    }
    return EventResult::Ignored;
}

Size InputDialog::preferredSize() const
{
    return { .width = _config.width, .height = 5 };
}

auto InputDialog::processEvent(InputEvent const& event) -> DialogResult
{
    if (auto const* key = std::get_if<KeyEvent>(&event))
    {
        switch (key->key)
        {
            case KeyCode::Enter: return DialogResult::Confirmed;

            case KeyCode::Escape: return DialogResult::Cancelled;

            case KeyCode::Backspace:
                if (_cursor > 0)
                {
                    _value.erase(_cursor - 1, 1);
                    --_cursor;
                    return DialogResult::Changed;
                }
                break;

            case KeyCode::Delete:
                if (_cursor < _value.size())
                {
                    _value.erase(_cursor, 1);
                    return DialogResult::Changed;
                }
                break;

            case KeyCode::Left:
                if (_cursor > 0)
                {
                    --_cursor;
                    return DialogResult::Changed;
                }
                break;

            case KeyCode::Right:
                if (_cursor < _value.size())
                {
                    ++_cursor;
                    return DialogResult::Changed;
                }
                break;

            case KeyCode::Home: _cursor = 0; return DialogResult::Changed;

            case KeyCode::End: _cursor = _value.size(); return DialogResult::Changed;

            default:
                if (isTextProducingKey(key->key))
                {
                    // Insert character at cursor
                    auto utf8 = std::string {};
                    if (key->codepoint < 0x80)
                    {
                        utf8 += static_cast<char>(key->codepoint);
                    }
                    else if (key->codepoint < 0x800)
                    {
                        utf8 += static_cast<char>(0xC0 | (key->codepoint >> 6));
                        utf8 += static_cast<char>(0x80 | (key->codepoint & 0x3F));
                    }
                    else if (key->codepoint < 0x10000)
                    {
                        utf8 += static_cast<char>(0xE0 | (key->codepoint >> 12));
                        utf8 += static_cast<char>(0x80 | ((key->codepoint >> 6) & 0x3F));
                        utf8 += static_cast<char>(0x80 | (key->codepoint & 0x3F));
                    }
                    else
                    {
                        utf8 += static_cast<char>(0xF0 | (key->codepoint >> 18));
                        utf8 += static_cast<char>(0x80 | ((key->codepoint >> 12) & 0x3F));
                        utf8 += static_cast<char>(0x80 | ((key->codepoint >> 6) & 0x3F));
                        utf8 += static_cast<char>(0x80 | (key->codepoint & 0x3F));
                    }

                    _value.insert(_cursor, utf8);
                    _cursor += utf8.size();
                    return DialogResult::Changed;
                }
                break;
        }
    }

    // Handle paste events
    if (auto const* paste = std::get_if<PasteEvent>(&event))
    {
        _value.insert(_cursor, paste->text);
        _cursor += paste->text.size();
        return DialogResult::Changed;
    }

    return DialogResult::None;
}

auto InputDialog::value() const noexcept -> std::string_view
{
    return _value;
}

void InputDialog::setValue(std::string_view value)
{
    _value = value;
    _cursor = _value.size();
}

} // namespace core::tui
