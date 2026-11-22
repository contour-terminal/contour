// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/view/BoardMarkdown.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <orchestrator/view/Sections.hpp>

using namespace std::string_view_literals;

namespace orchestrator::view
{

namespace
{
    /// How long a rule is drawn.
    constexpr auto RuleCells = 24;

    /// A list item's markers, and what they become.
    struct ItemMarker
    {
        std::string_view marker; ///< As written, its trailing space included.
        std::string_view drawn;  ///< What it is drawn as.
        Role role;               ///< How the line is drawn.
    };

    /// The first that a line starts with, past its indentation, is its marker: a task box before a plain
    /// item.
    constexpr auto ItemMarkers = std::array {
        ItemMarker { .marker = "- [ ] "sv, .drawn = "☐ "sv, .role = Role::Text },
        ItemMarker { .marker = "- [x] "sv, .drawn = "☑ "sv, .role = Role::Passing },
        ItemMarker { .marker = "- [X] "sv, .drawn = "☑ "sv, .role = Role::Passing },
        ItemMarker { .marker = "* [ ] "sv, .drawn = "☐ "sv, .role = Role::Text },
        ItemMarker { .marker = "* [x] "sv, .drawn = "☑ "sv, .role = Role::Passing },
        ItemMarker { .marker = "- "sv, .drawn = "• "sv, .role = Role::Text },
        ItemMarker { .marker = "* "sv, .drawn = "• "sv, .role = Role::Text },
        ItemMarker { .marker = "+ "sv, .drawn = "• "sv, .role = Role::Text },
        ItemMarker { .marker = "> "sv, .drawn = "│ "sv, .role = Role::Note },
    };

    /// The inline markers dropped from a line's text.
    constexpr auto DroppedMarkers = std::array { "**"sv, "__"sv, "`"sv };

    [[nodiscard]] std::string_view trimmed(std::string_view text) noexcept
    {
        auto const first = text.find_first_not_of(' ');
        if (first == std::string_view::npos)
            return {};
        return text.substr(first, text.find_last_not_of(' ') - first + 1);
    }

    /// @return @p text without its inline markers, and each `[text](target)` as its text.
    [[nodiscard]] std::string inlineText(std::string_view text)
    {
        auto out = std::string {};
        while (!text.empty())
        {
            if (auto const marker = std::ranges::find_if(
                    DroppedMarkers, [&](std::string_view candidate) { return text.starts_with(candidate); });
                marker != DroppedMarkers.end())
            {
                text.remove_prefix(marker->size());
                continue;
            }
            if (text.front() == '[')
            {
                auto const close = text.find("](");
                auto const end = close == std::string_view::npos ? close : text.find(')', close);
                if (end != std::string_view::npos)
                {
                    out += inlineText(text.substr(1, close - 1));
                    text.remove_prefix(end + 1);
                    continue;
                }
            }
            out += text.front();
            text.remove_prefix(1);
        }
        return out;
    }

    [[nodiscard]] bool isRule(std::string_view text) noexcept
    {
        auto const line = trimmed(text);
        return line.size() >= 3 && std::ranges::any_of(std::array { '-', '*', '_' }, [&](char c) {
                   return std::ranges::all_of(line, [&](char each) { return each == c || each == ' '; });
               });
    }

    [[nodiscard]] bool isTableRow(std::string_view text) noexcept
    {
        auto const line = trimmed(text);
        return line.size() >= 2 && line.starts_with('|');
    }

    /// @return The cells of a table row, trimmed, without the outer pipes.
    [[nodiscard]] std::vector<std::string> cellsOf(std::string_view text)
    {
        auto line = trimmed(text);
        line.remove_prefix(1);
        if (line.ends_with('|'))
            line.remove_suffix(1);
        auto cells = std::vector<std::string> {};
        for (auto const cell: std::views::split(line, '|'))
            cells.push_back(inlineText(trimmed(std::string_view { cell.begin(), cell.end() })));
        return cells;
    }

    [[nodiscard]] bool isSeparatorRow(std::vector<std::string> const& cells) noexcept
    {
        return !cells.empty() && std::ranges::all_of(cells, [](std::string const& cell) {
            return !cell.empty()
                   && std::ranges::all_of(cell, [](char c) { return c == '-' || c == ':' || c == ' '; });
        });
    }

    /// Appends a table's rows, its cells padded to their column's widest.
    void appendTable(std::vector<BoardLine>& out, std::span<std::string const> rows)
    {
        auto table = std::vector<std::vector<std::string>> {};
        auto header = true;
        auto headerRows = std::size_t { 0 };
        for (auto const& row: rows)
        {
            auto cells = cellsOf(row);
            if (isSeparatorRow(cells))
            {
                header = false;
                continue;
            }
            if (header)
                ++headerRows;
            table.push_back(std::move(cells));
        }
        // A table without a separator row has no header.
        if (header)
            headerRows = 0;
        auto widths = std::vector<int> {};
        for (auto const& cells: table)
            for (auto const [column, cell]: std::views::enumerate(cells))
            {
                if (std::cmp_greater_equal(column, widths.size()))
                    widths.resize(static_cast<std::size_t>(column) + 1, 0);
                widths[static_cast<std::size_t>(column)] =
                    std::max(widths[static_cast<std::size_t>(column)], columnsOf(cell));
            }
        for (auto const [index, cells]: std::views::enumerate(table))
        {
            auto text = std::string {};
            for (auto const [column, cell]: std::views::enumerate(cells))
            {
                if (column > 0)
                    text += " │ ";
                text += cell;
                // The last cell needs no padding.
                if (std::cmp_less(column + 1, cells.size()))
                    text += std::string(
                        static_cast<std::size_t>(widths[static_cast<std::size_t>(column)] - columnsOf(cell)),
                        ' ');
            }
            out.push_back(BoardLine { .text = std::move(text),
                                      .role = std::cmp_less(index, headerRows) ? Role::Accent : Role::Text,
                                      .fit = BoardFit::Cut });
        }
    }

    /// @return A line that is no table row, fence or code, drawn.
    [[nodiscard]] BoardLine lineOf(std::string_view text)
    {
        auto const first = text.find_first_not_of(' ');
        auto const indentation = first == std::string_view::npos ? text.size() : first;
        auto const indent = std::string(indentation, ' ');
        auto const rest = text.substr(indentation);
        if (rest.starts_with('#'))
        {
            auto const hashes = rest.find_first_not_of('#');
            if (hashes != std::string_view::npos && rest[hashes] == ' ')
                return BoardLine { .text = indent + inlineText(trimmed(rest.substr(hashes))),
                                   .role = Role::Accent };
        }
        if (isRule(rest))
        {
            auto rule = std::string {};
            for ([[maybe_unused]] auto const cell: std::views::iota(0, RuleCells))
                rule += "─";
            return BoardLine { .text = indent + rule, .role = Role::Dim };
        }
        if (auto const marker = std::ranges::find_if(
                ItemMarkers, [&](ItemMarker const& candidate) { return rest.starts_with(candidate.marker); });
            marker != ItemMarkers.end())
            return BoardLine { .text = std::format("{}{}{}",
                                                   indent,
                                                   marker->drawn,
                                                   inlineText(rest.substr(marker->marker.size()))),
                               .role = marker->role };
        return BoardLine { .text = indent + inlineText(rest), .role = Role::Text };
    }
} // namespace

std::vector<BoardLine> renderBoardMarkdown(std::span<std::string const> lines)
{
    auto out = std::vector<BoardLine> {};
    auto inCode = false;
    auto index = std::size_t { 0 };
    while (index < lines.size())
    {
        auto const& line = lines[index];
        if (trimmed(line).starts_with("```"))
        {
            inCode = !inCode;
            ++index;
            continue;
        }
        if (inCode)
        {
            out.push_back(BoardLine { .text = line, .role = Role::Dim, .fit = BoardFit::Cut });
            ++index;
            continue;
        }
        if (isTableRow(line))
        {
            auto const end = std::ranges::find_if_not(lines.subspan(index), isTableRow) - lines.begin();
            appendTable(out, lines.subspan(index, static_cast<std::size_t>(end) - index));
            index = static_cast<std::size_t>(end);
            continue;
        }
        out.push_back(lineOf(line));
        ++index;
    }
    return out;
}

} // namespace orchestrator::view
