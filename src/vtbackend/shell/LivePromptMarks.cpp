// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/shell/LivePromptMarks.hpp>

#include <algorithm>
#include <vector>

namespace vtbackend
{

void LivePromptMarks::markRightPrompt(StableLine line, ColumnOffset column)
{
    marksOf(line).rightPrompt = column;
}

std::optional<ColumnOffset> LivePromptMarks::rightPromptOn(StableLine line) const noexcept
{
    auto const* const marks = find(line);
    return marks ? marks->rightPrompt : std::nullopt;
}

void LivePromptMarks::markInputEnd(StableLine line, InputEnd end)
{
    if (end == InputEnd::AtOutputStart && !find(line))
        return; // what an unmarked line says already
    marksOf(line).inputEnd = end;
}

void LivePromptMarks::follow(RowMove const& move)
{
    // A line the scroll dropped first; then every other one to where its head went, which no two share.
    std::erase_if(_lines, [&move](LineMarks const& marks) { return !move.destinationOf(marks.line.id); });
    for (auto& marks: _lines)
        marks.line.id = move.destinationOf(marks.line.id).value_or(marks.line.id);
}

InputEnd LivePromptMarks::inputEndOn(StableLine line) const noexcept
{
    auto const* const marks = find(line);
    return marks ? marks->inputEnd : InputEnd::AtOutputStart;
}

LivePromptMarks::LineMarks const* LivePromptMarks::find(StableLine line) const noexcept
{
    auto const known = std::ranges::find(_lines, line, &LineMarks::line);
    return known != _lines.end() ? &*known : nullptr;
}

LivePromptMarks::LineMarks& LivePromptMarks::marksOf(StableLine line)
{
    if (auto const known = std::ranges::find(_lines, line, &LineMarks::line); known != _lines.end())
        return *known;
    if (_lines.size() == MaxLines)
        _lines.erase(_lines.begin());
    return _lines.emplace_back(
        LineMarks { .line = line, .rightPrompt = std::nullopt, .inputEnd = InputEnd::AtOutputStart });
}

} // namespace vtbackend
