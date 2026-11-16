// SPDX-License-Identifier: Apache-2.0
#include <core/tui/QuestionComponent.hpp>

#include <core/tui/Canvas.hpp>
#include <core/tui/Theme.hpp>

#include <algorithm>
#include <ranges>
#include <utility>

namespace core::tui
{

QuestionComponent::QuestionComponent(QuestionConfig config, IClock const& clock):
    _config(std::move(config)), _inputField(clock)
{
    _inputField.setPrompt("\xe2\x9d\xaf "); // ❯

    if (_config.masked)
        _inputField.setMasked(true);

    if (!isFreeTextOnly())
    {
        auto items = std::vector<ListItem> {};
        items.reserve(_config.options.size() + (_config.allowOther ? 1 : 0));
        for (auto const& opt: _config.options)
            items.push_back(ListItem { .label = opt, .description = {}, .filterText = {} });
        if (_config.allowOther)
            items.push_back(ListItem { .label = "Other...", .description = {}, .filterText = {} });
        _list.setItems(std::move(items));
        _list.setMultiSelect(_config.multiSelect);
    }
    else
    {
        _inputFocused = true;
    }
}

// =============================================================================
// Component Interface
// =============================================================================

void QuestionComponent::render(Canvas& canvas)
{
    auto const& theme = currentTheme();
    auto barStyle = Style {};
    barStyle.fg = theme.agentColors.leftBar;
    auto const bar = BorderChars::fromStyle(_config.border);
    auto labelStyle = Style {};
    labelStyle.fg = theme.agentColors.leftBar;
    auto questionStyle = Style {};
    questionStyle.fg = theme.colors.text;
    auto hintStyle = Style {};
    hintStyle.fg = theme.agentColors.statusText;
    hintStyle.dim = true;

    auto const width = canvas.width();
    auto row = 0;

    // --- Header line: ╭─ question ─────
    canvas.putString(row, 0, bar.topLeft, barStyle);
    canvas.putString(row, 1, bar.horizontal, barStyle);
    canvas.put(row, 2, " ", {});
    auto col = 3 + canvas.putString(row, 3, "question", labelStyle);
    // Fill rest of header with ─
    canvas.drawHLine(row, col + 1, width - col - 1, bar.horizontal, barStyle);
    ++row;

    // --- Question text lines: │  question text (supports multi-line via \n)
    auto remaining = std::string_view(_config.questionText);
    while (!remaining.empty())
    {
        auto const nl = remaining.find('\n');
        auto const line = remaining.substr(0, nl);
        canvas.putString(row, 0, bar.vertical, barStyle);
        canvas.putString(row, LeftBarWidth + BarPadding, line, questionStyle);
        ++row;
        if (nl == std::string_view::npos)
            break;
        remaining.remove_prefix(nl + 1);
    }

    if (isFreeTextOnly())
    {
        // --- Free-text only: ╰─ ❯ [input]
        canvas.putString(row, 0, bar.bottomLeft, barStyle);
        canvas.putString(row, 1, bar.horizontal, barStyle);

        auto const fieldArea = Rect {
            .x = LeftBarWidth + BarPadding,
            .y = row,
            .width = width - LeftBarWidth - BarPadding,
            .height = 1,
        };
        auto fieldCanvas = canvas.subcanvas(fieldArea);
        _inputField.render(fieldCanvas);
    }
    else if (_config.multiSelect)
    {
        // --- Multi-select: │ blank, then list items with │, then optional input, then ╰─ hint
        canvas.putString(row, 0, bar.vertical, barStyle); // blank separator
        ++row;

        // List items
        auto const listHeight = static_cast<int>(_list.items().size());
        for ([[maybe_unused]] auto const item: std::views::iota(0, std::max(0, listHeight)))
        {
            if (row >= canvas.height())
                break;
            canvas.putString(row, 0, bar.vertical, barStyle);
            ++row;
        }

        // Render list into its subcanvas region
        auto const listStartRow = HeaderHeight + questionLineCount() + 1; // header + question lines + blank
        auto const listArea = Rect {
            .x = LeftBarWidth + BarPadding,
            .y = listStartRow,
            .width = width - LeftBarWidth - BarPadding,
            .height = listHeight,
        };
        auto listCanvas = canvas.subcanvas(listArea);
        _list.render(listCanvas);

        // Optional InputField when "Other..." is active
        if (_otherActive)
        {
            canvas.putString(row, 0, bar.vertical, barStyle);
            auto const inputArea = Rect {
                .x = LeftBarWidth + BarPadding,
                .y = row,
                .width = width - LeftBarWidth - BarPadding,
                .height = 1,
            };
            auto inputCanvas = canvas.subcanvas(inputArea);
            _inputField.render(inputCanvas);
            ++row;
        }

        // Bottom bar with hint
        canvas.putString(row, 0, bar.bottomLeft, barStyle);
        canvas.putString(row, 1, bar.horizontal, barStyle);
        canvas.putString(row,
                         LeftBarWidth + BarPadding,
                         "Space toggle \xe2\x94\x82 Enter confirm \xe2\x94\x82 Esc cancel",
                         hintStyle);
    }
    else
    {
        // --- Single-select
        if (!_otherActive)
        {
            // Show list with blank separator
            canvas.putString(row, 0, bar.vertical, barStyle); // blank separator
            ++row;

            auto const listHeight = static_cast<int>(_list.items().size());
            for ([[maybe_unused]] auto const item: std::views::iota(0, std::max(0, listHeight)))
            {
                if (row >= canvas.height())
                    break;
                canvas.putString(row, 0, bar.vertical, barStyle);
                ++row;
            }

            // Render list
            auto const listStartRow =
                HeaderHeight + questionLineCount() + 1; // header + question lines + blank
            auto const listArea = Rect {
                .x = LeftBarWidth + BarPadding,
                .y = listStartRow,
                .width = width - LeftBarWidth - BarPadding,
                .height = listHeight,
            };
            auto listCanvas = canvas.subcanvas(listArea);
            _list.render(listCanvas);

            // Bottom bar with hint
            canvas.putString(row, 0, bar.bottomLeft, barStyle);
            canvas.putString(row, 1, bar.horizontal, barStyle);
            canvas.putString(
                row, LeftBarWidth + BarPadding, "Enter select \xe2\x94\x82 Esc cancel", hintStyle);
        }
        else
        {
            // Other active: show InputField replacing list
            canvas.putString(row, 0, bar.bottomLeft, barStyle);
            canvas.putString(row, 1, bar.horizontal, barStyle);
            auto const fieldArea = Rect {
                .x = LeftBarWidth + BarPadding,
                .y = row,
                .width = width - LeftBarWidth - BarPadding,
                .height = 1,
            };
            auto fieldCanvas = canvas.subcanvas(fieldArea);
            _inputField.render(fieldCanvas);
        }
    }
}

EventResult QuestionComponent::onEvent(InputEvent const& event)
{
    auto const action = processInput(event);
    return action != QuestionAction::None ? EventResult::Handled : EventResult::Ignored;
}

CursorShape QuestionComponent::cursorShape() const
{
    if (_inputFocused || isFreeTextOnly())
        return CursorShape::SteadyBar;
    return CursorShape::Default;
}

Size QuestionComponent::preferredSize() const
{
    auto const height =
        HeaderHeight + questionLineCount() + contentHeight() + 1; // header + question + content + hint/bottom
    return { .width = 60, .height = height };
}

// =============================================================================
// Question API
// =============================================================================

auto QuestionComponent::processInput(InputEvent const& event) -> QuestionAction
{
    if (isFreeTextOnly())
    {
        // Free-text only: intercept Escape, delegate rest to InputField
        if (auto const* key = std::get_if<KeyEvent>(&event))
        {
            if (key->key == KeyCode::Escape)
                return QuestionAction::Cancelled;
        }
        auto const action = _inputField.processEvent(event);
        switch (action)
        {
            case InputFieldAction::Submit: return QuestionAction::Confirmed;
            case InputFieldAction::Abort:
            case InputFieldAction::Eof: return QuestionAction::Cancelled;
            case InputFieldAction::Changed: return QuestionAction::Changed;
            default: return QuestionAction::None;
        }
    }

    if (_config.multiSelect)
    {
        // Multi-select mode
        if (_inputFocused && _otherActive)
        {
            // Input field is focused — handle Tab/Escape to return to list
            if (auto const* key = std::get_if<KeyEvent>(&event))
            {
                if (key->key == KeyCode::Tab || key->key == KeyCode::Escape)
                {
                    _inputFocused = false;
                    return QuestionAction::Changed;
                }
            }
            auto const action = _inputField.processEvent(event);
            switch (action)
            {
                case InputFieldAction::Submit: return QuestionAction::Confirmed;
                case InputFieldAction::Changed: return QuestionAction::Changed;
                default: return QuestionAction::None;
            }
        }

        // List is focused
        auto const action = _list.processEvent(event);
        switch (action)
        {
            case ListAction::Toggled: {
                auto const idx = _list.selectedIndex();
                auto const otherIdx = otherItemIndex();
                if (otherIdx >= 0 && std::cmp_equal(idx, otherIdx))
                {
                    _otherActive = _list.isChecked(idx);
                    if (_otherActive)
                        _inputFocused = true;
                }
                return QuestionAction::Changed;
            }
            case ListAction::Selected: return QuestionAction::Confirmed; // Enter confirms all checked
            case ListAction::Cancelled: return QuestionAction::Cancelled;
            case ListAction::Changed: return QuestionAction::Changed;
            default: {
                // Tab to focus InputField when on Other and it's checked
                if (auto const* key = std::get_if<KeyEvent>(&event))
                {
                    if (key->key == KeyCode::Tab && _otherActive)
                    {
                        _inputFocused = true;
                        return QuestionAction::Changed;
                    }
                }
                return QuestionAction::None;
            }
        }
    }

    // Single-select mode
    if (_otherActive)
    {
        // "Other..." is active — InputField shown
        if (auto const* key = std::get_if<KeyEvent>(&event))
        {
            if (key->key == KeyCode::Escape)
            {
                _otherActive = false;
                _inputFocused = false;
                return QuestionAction::Changed; // Go back to list
            }
        }
        auto const action = _inputField.processEvent(event);
        switch (action)
        {
            case InputFieldAction::Submit: return QuestionAction::Confirmed;
            case InputFieldAction::Changed: return QuestionAction::Changed;
            default: return QuestionAction::None;
        }
    }

    // Single-select, list is active
    auto const action = _list.processEvent(event);
    switch (action)
    {
        case ListAction::Selected: {
            auto const idx = _list.selectedIndex();
            auto const otherIdx = otherItemIndex();
            if (otherIdx >= 0 && std::cmp_equal(idx, otherIdx))
            {
                // Transition to "Other..." InputField
                _otherActive = true;
                _inputFocused = true;
                return QuestionAction::Changed;
            }
            return QuestionAction::Confirmed;
        }
        case ListAction::Cancelled: return QuestionAction::Cancelled;
        case ListAction::Changed: return QuestionAction::Changed;
        default: return QuestionAction::None;
    }
}

std::optional<QuestionResult> QuestionComponent::step(InputEvent const& event)
{
    _lastAction = processInput(event);
    switch (_lastAction)
    {
        case QuestionAction::Confirmed:
            return QuestionResult { .confirmed = true,
                                    .selectedIndex = selectedIndex(),
                                    .checkedIndices = checkedIndices(),
                                    .answer = answer(),
                                    .otherActive = isOtherActive() };
        case QuestionAction::Cancelled: return QuestionResult {}; // confirmed defaults to false
        case QuestionAction::Changed:
        case QuestionAction::None: return std::nullopt;
    }
    return std::nullopt;
}

bool QuestionComponent::stepChangedState() const noexcept
{
    // Redraw only when the last step changed visible state (Changed) — Confirmed
    // ends the modal before a redraw is needed; None/Cancelled need no redraw.
    return _lastAction == QuestionAction::Changed || _lastAction == QuestionAction::Confirmed;
}

auto QuestionComponent::answer() const -> std::string
{
    if (isFreeTextOnly())
        return std::string(_inputField.text());

    if (_config.multiSelect)
    {
        auto result = std::string {};
        auto const checked = _list.checkedIndices();
        auto const otherIdx = otherItemIndex();
        for (auto idx: checked)
        {
            if (otherIdx >= 0 && std::cmp_equal(idx, otherIdx))
            {
                // Use the InputField text for "Other..."
                if (!result.empty())
                    result += ", ";
                result += std::string(_inputField.text());
            }
            else
            {
                if (!result.empty())
                    result += ", ";
                result += _list.items()[idx].label;
            }
        }
        return result;
    }

    // Single-select
    if (_otherActive)
        return std::string(_inputField.text());

    if (auto const item = _list.selectedItem())
        return (*item)->label;

    return {};
}

// =============================================================================
// Private helpers
// =============================================================================

int QuestionComponent::otherItemIndex() const noexcept
{
    if (!_config.allowOther || _config.options.empty())
        return -1;
    return static_cast<int>(_config.options.size()); // "Other..." is appended after all options
}

int QuestionComponent::contentHeight() const noexcept
{
    if (isFreeTextOnly())
        return 0; // InputField is on the same row as the bottom bar

    auto height = 1; // blank separator line
    height += static_cast<int>(_list.items().size());
    if (_config.multiSelect && _otherActive)
        height += 1; // InputField row
    return height;
}

int QuestionComponent::questionLineCount() const noexcept
{
    auto count = 1;
    for (auto ch: _config.questionText)
        if (ch == '\n')
            ++count;
    return count;
}

} // namespace core::tui
