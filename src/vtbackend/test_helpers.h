/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2023 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <vtbackend/MockTerm.h>
#include <vtbackend/RenderBuffer.h>
#include <vtbackend/Terminal.h>

#include <crispy/escape.h>

#include <catch2/catch.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace terminal::test
{

constexpr line_offset operator""_lineOffset(unsigned long long value) noexcept
{
    return line_offset::cast_from(value);
}

constexpr column_offset operator""_columnOffset(unsigned long long value) noexcept
{
    return column_offset::cast_from(value);
}

constexpr cell_location operator+(line_offset line, column_offset column) noexcept
{
    return { line, column };
}

template <typename S>
[[nodiscard]] inline decltype(auto) e(S const& s)
{
    return crispy::escape(s);
}

/// Takes a textual screenshot using the terminals render buffer.
[[nodiscard]] inline std::vector<std::string> textScreenshot(terminal::Terminal const& terminal)
{
    terminal::RenderBufferRef renderBuffer = terminal.renderBuffer();

    std::vector<std::string> lines;
    lines.resize(terminal.pageSize().lines.as<size_t>());

    terminal::cell_location lastPos = {};
    size_t lastCount = 0;
    for (terminal::RenderCell const& cell: renderBuffer.buffer.cells)
    {
        auto const gap = (cell.position.column + static_cast<int>(lastCount) - 1) - lastPos.column;
        auto& currentLine = lines.at(unbox<size_t>(cell.position.line));
        if (*gap > 0) // Did we jump?
            currentLine.insert(currentLine.end(), unbox<size_t>(gap) - 1, ' ');

        currentLine += unicode::convert_to<char>(std::u32string_view(cell.codepoints));
        lastPos = cell.position;
        lastCount = 1;
    }
    for (terminal::RenderLine const& line: renderBuffer.buffer.lines)
    {
        auto& currentLine = lines.at(unbox<size_t>(line.lineOffset));
        currentLine = line.text;
    }

    return lines;
}

[[nodiscard]] inline std::string trimRight(std::string text)
{
    constexpr auto Whitespaces = std::string_view("\x20\t\r\n");
    while (!text.empty() && Whitespaces.find(text.back()) != std::string_view::npos)
        text.resize(text.size() - 1);
    return text;
}

[[nodiscard]] inline std::string join(std::vector<std::string> const& lines)
{
    std::string output;
    for (std::string const& line: lines)
    {
        output += trimRight(line);
        output += '\n';
    }
    return output;
}

template <typename T>
[[nodiscard]] std::string trimmedTextScreenshot(MockTerm<T> const& mt)
{
    return trimRight(join(textScreenshot(mt.terminal)));
}

template <typename T>
[[nodiscard]] std::string mainPageText(Screen<T> const& screen)
{
    return screen.renderMainPageText();
}

template <typename T>
void logScreenTextAlways(Screen<T> const& screen, std::string const& headline = "")
{
    fmt::print("{}: ZI={} cursor={} HM={}..{}\n",
               headline.empty() ? "screen dump" : headline,
               screen.grid().zero_index(),
               screen.realCursorPosition(),
               screen.margin().horizontal.from,
               screen.margin().horizontal.to);
    fmt::print("{}\n", dumpGrid(screen.grid()));
}

template <typename T>
void logScreenTextAlways(MockTerm<T> const& mock, std::string const& headline = "")
{
    logScreenTextAlways(mock.terminal.primaryScreen(), headline);
}

template <typename T>
void logScreenText(Screen<T> const& screen, std::string const& headline = "")
{
    if (headline.empty())
        UNSCOPED_INFO("dump:");
    else
        UNSCOPED_INFO(headline + ":");

    for (auto const line: ::ranges::views::iota(0, *screen.pageSize().lines))
        UNSCOPED_INFO(fmt::format("[{}] \"{}\"", line, screen.grid().lineText(line_offset::cast_from(line))));
}

inline void logScreenText(terminal::Terminal const& terminal, std::string const& headline = "")
{
    logScreenText(terminal.primaryScreen(), headline);
}

} // namespace terminal::test
