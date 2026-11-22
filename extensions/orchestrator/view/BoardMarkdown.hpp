// SPDX-License-Identifier: Apache-2.0
//
// A status board's Markdown, drawn as styled lines: what boards use (headings, lists, task boxes, quotes,
// rules, tables, code), from lines that are sanitised already, so that styling never sees an escape sequence.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <orchestrator/view/Theme.hpp>

namespace orchestrator::view
{

/// How a board's line is fitted to a narrower panel.
enum class BoardFit : uint8_t
{
    Wrap = 0, ///< Word-wrapped, its indentation kept: prose.
    Cut,      ///< On one line, cut at the edge: a table row or code, whose spaces a wrap would collapse.
};

/// One drawn line of a board: its text, markers turned into what they stand for, and how it is drawn.
struct BoardLine
{
    std::string text = {};         ///< Sanitised; its indentation kept.
    Role role = Role::Text;        ///< How it is drawn.
    BoardFit fit = BoardFit::Wrap; ///< How it is fitted to a narrower panel.
    [[nodiscard]] bool operator==(BoardLine const&) const = default;
};

/// Renders a board's lines as Markdown. A heading drops its `#`s and is drawn in the accent; a list item's
/// marker becomes `•`, a task box `☐` or `☑` (done is drawn as passing); a quote's `>` becomes `│`; a rule is
/// a line of `─`; a table's cells are aligned and joined by `│`, its separator row dropped and its header in
/// the accent; a fenced block is drawn dim without its fences. Within a line, `**`, `__` and backticks are
/// dropped, and a link shows its text. Anything else is drawn as it is.
/// @param lines The board's lines, as displayLines() made them.
/// @return The drawn lines, one per input line but for a table's separator row and a fence.
[[nodiscard]] std::vector<BoardLine> renderBoardMarkdown(std::span<std::string const> lines);

} // namespace orchestrator::view
