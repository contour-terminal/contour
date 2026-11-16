// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/tui/Buffer.hpp>
#include <core/tui/Canvas.hpp>
#include <core/tui/CompletionPopup.hpp>
#include <core/tui/InputEvent.hpp>
#include <core/tui/Rect.hpp>
#include <core/tui/Theme.hpp>
#include <core/tui/completer/CompletionItem.hpp>

#include <algorithm>
#include <ranges>
#include <string>
#include <tuple>
#include <vector>

namespace core::tui::test
{

/// @brief Extracts text content from a buffer area as a string.
///
/// Extracts the grapheme content from each cell in the specified area,
/// preserving exact spacing (no trimming). Each row is separated by a newline.
///
/// @param buffer The buffer to extract from.
/// @param area The rectangular area to extract. If empty, extracts entire buffer.
/// @return Multi-line string with exact spacing preserved.
inline std::string canvasToString(Buffer const& buffer, Rect area = {})
{
    if (area.empty())
        area = Rect { .x = 0, .y = 0, .width = buffer.cols(), .height = buffer.rows() };

    std::string result;
    for (auto const row: std::views::iota(area.y, std::max(area.y, area.bottom())))
    {
        if (row > area.y)
            result += '\n';

        for (auto const col: std::views::iota(area.x, std::max(area.x, area.right())))
        {
            if (auto const* cell = buffer.cellAt({ .x = col, .y = row }))
            {
                if (cell->isContinuation())
                    continue; // Skip continuation cells (wide chars)
                result += cell->grapheme.empty() ? " " : cell->grapheme;
            }
            else
            {
                result += ' ';
            }
        }
    }
    return result;
}

/// @brief Renders a CompletionPopup to a string.
///
/// Creates a buffer, renders the popup to a canvas, and returns the text content.
///
/// @param popup The popup to render.
/// @param width Width of the buffer.
/// @param height Height of the buffer.
/// @return Multi-line string of the rendered popup.
inline std::string renderPopup(CompletionPopup& popup, int width, int height)
{
    Buffer buffer(height, width);
    buffer.clear();

    Theme const theme;
    Canvas canvas(buffer, Rect { .x = 0, .y = 0, .width = width, .height = height }, theme);
    popup.render(canvas);

    return canvasToString(buffer);
}

/// @brief Returns the grapheme at a specific position in a buffer.
/// @param buffer The buffer to query.
/// @param row The row (0-based).
/// @param col The column (0-based).
/// @return The grapheme string, or empty if out of bounds or continuation.
inline std::string graphemeAt(Buffer const& buffer, int row, int col)
{
    if (auto const* cell = buffer.cellAt({ .x = col, .y = row }))
    {
        if (cell->isContinuation())
            return "";
        return cell->grapheme;
    }
    return "";
}

/// @brief Returns the style at a specific position in a buffer.
/// @param buffer The buffer to query.
/// @param row The row (0-based).
/// @param col The column (0-based).
/// @return The style, or default style if out of bounds.
inline Style styleAt(Buffer const& buffer, int row, int col)
{
    if (auto const* cell = buffer.cellAt({ .x = col, .y = row }))
        return cell->style;
    return {};
}

/// @brief Creates a KeyEvent for a printable character.
/// @param c The character.
/// @param mod Modifiers (default: None).
/// @return The KeyEvent.
inline KeyEvent charKey(char c, Modifier mod = Modifier::None)
{
    return KeyEvent { .key = keyCodeFromCodepoint(static_cast<char32_t>(c)),
                      .modifiers = mod,
                      .codepoint = static_cast<char32_t>(c) };
}

/// @brief Creates a KeyEvent for a special key.
/// @param key The key code.
/// @param mod Modifiers (default: None).
/// @return The KeyEvent.
inline KeyEvent specialKey(KeyCode key, Modifier mod = Modifier::None)
{
    return KeyEvent { .key = key, .modifiers = mod, .codepoint = 0 };
}

/// @brief Creates a vector of CompletionItems from strings.
/// @param texts The completion texts.
/// @return Vector of CompletionItems with text and displayText set.
inline std::vector<completer::CompletionItem> makeItems(std::initializer_list<std::string_view> texts)
{
    std::vector<completer::CompletionItem> items;
    items.reserve(texts.size());
    int score = static_cast<int>(texts.size()) * 10;
    for (auto text: texts)
    {
        items.push_back(completer::CompletionItem { .text = std::string(text),
                                                    .displayText = std::string(text),
                                                    .description = "",
                                                    .detail = {},
                                                    .score = score,
                                                    .matchPositions = {} });
        score -= 10;
    }
    return items;
}

/// @brief Creates a vector of CompletionItems with descriptions.
/// @param items Pairs of (text, description).
/// @return Vector of CompletionItems.
inline std::vector<completer::CompletionItem> makeItemsWithDesc(
    std::initializer_list<std::pair<std::string_view, std::string_view>> items)
{
    std::vector<completer::CompletionItem> result;
    result.reserve(items.size());
    int score = static_cast<int>(items.size()) * 10;
    for (auto const& [text, desc]: items)
    {
        result.push_back(completer::CompletionItem { .text = std::string(text),
                                                     .displayText = std::string(text),
                                                     .description = std::string(desc),
                                                     .detail = {},
                                                     .score = score,
                                                     .matchPositions = {} });
        score -= 10;
    }
    return result;
}

/// @brief Creates a vector of CompletionItems with detail text.
/// @param items Triples of (text, description, detail).
/// @return Vector of CompletionItems.
inline std::vector<completer::CompletionItem> makeItemsWithDetail(
    std::initializer_list<std::tuple<std::string_view, std::string_view, std::string_view>> items)
{
    std::vector<completer::CompletionItem> result;
    result.reserve(items.size());
    int score = static_cast<int>(items.size()) * 10;
    for (auto const& [text, desc, detail]: items)
    {
        result.push_back(completer::CompletionItem { .text = std::string(text),
                                                     .displayText = std::string(text),
                                                     .description = std::string(desc),
                                                     .detail = std::string(detail),
                                                     .score = score,
                                                     .matchPositions = {} });
        score -= 10;
    }
    return result;
}

/// @brief Creates a MouseEvent for a mouse move at the given 1-based position.
/// @param x Column (1-based).
/// @param y Row (1-based).
/// @return The MouseEvent.
inline MouseEvent mouseMove(int x, int y)
{
    return MouseEvent { .type = MouseEvent::Type::Move, .button = 0, .x = x, .y = y };
}

} // namespace core::tui::test
