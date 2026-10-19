// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/input/vi/HintModeHandler.hpp>

#include <vtbackend/core/Hyperlink.hpp>

#include <algorithm>
#include <cctype>
#include <optional>
#include <ranges>
#include <utility>

using namespace std;

namespace vtbackend
{

namespace
{
    /// The characters labels are built from — the same set @ref HintModeHandler::processInput accepts.
    constexpr auto Alphabet = string_view { "abcdefghijklmnopqrstuvwxyz" };
} // namespace

HintModeHandler::HintModeHandler(Executor& executor): _executor { executor }
{
}

void HintModeHandler::rescanLines(HintScanArea const& area)
{
    _filter.clear();
    _allMatches.clear();
    _filteredMatches.clear();

    // Match against logical lines, not physical rows: a URL broken across a wrap is one match. The scan text
    // maps every match back to its cells, wide characters and clusters included.
    auto const& table = *_patterns.table;
    auto const taking = [&](LinkCandidate const& candidate) {
        return _patterns.names.empty()
               || std::ranges::find(_patterns.names, table.patterns()[candidate.patternIndex].name)
                      != _patterns.names.end();
    };
    for (auto const& logical: joinLogicalLines(area.rows, area.continuesBelow, MaxScanCells))
    {
        for (auto const& candidate: findLinks(logical, table, PatternSelection::All, LinkOverlap::Keep))
        {
            if (!taking(candidate))
                continue;
            auto const start = CellLocation { .line = logical.lineOf(candidate.first.row),
                                              .column = candidate.first.column };

            // The label is drawn at the match start, so a match starting on a row that cannot carry one could
            // never be selected: do not offer it. Tested before resolving, which may go to the filesystem.
            if (!area.labelableRows.contains(start.line))
                continue;

            // A match whose printed text chose a scheme no link may open is offered to copy all the same, as
            // printed; only a match that may be opened is resolved.
            auto target = candidate.opening == LinkOpening::Refused || !_patterns.resolve
                              ? std::optional { printedLinkTarget(candidate) }
                              : _patterns.resolve(candidate, start.line);
            if (!target)
                continue;

            // Copy, Paste and Select take the text as printed, a path the absolute form it resolved to; only
            // Open takes a template's expansion.
            auto matchedText = candidate.kind == LinkKind::Path ? target->target : candidate.text;
            _allMatches.push_back(HintMatch {
                .label = {},
                .matchedText = std::move(matchedText),
                .target = std::move(*target),
                .opening = candidate.opening,
                .start = start,
                .end = CellLocation { .line = logical.lineOf(candidate.last.row),
                                      .column = candidate.last.column },
            });
        }
    }

    // Sort matches in reading order, longer matches first at the same start. CellLocation's
    // defaulted operator<=> compares line before column, so it already is reading order.
    ranges::sort(_allMatches, [](auto const& a, auto const& b) {
        if (a.start != b.start)
            return a.start < b.start;
        return b.end < a.end; // Longer match first at the same start position.
    });

    // Remove duplicate matches (same text at same position).
    auto const [eraseBegin, eraseEnd] = ranges::unique(
        _allMatches, [](auto const& a, auto const& b) { return a.start == b.start && a.end == b.end; });
    _allMatches.erase(eraseBegin, eraseEnd);

    // Remove overlapping matches — keep the longer (earlier) match at each position. The list is
    // sorted, so only the last kept match can overlap, and comparing whole positions rather than
    // columns makes this hold across a wrap boundary too.
    {
        auto kept = vector<HintMatch>();
        kept.reserve(_allMatches.size());
        for (auto& match: _allMatches)
        {
            if (!kept.empty() && match.start <= kept.back().end)
                continue; // Overlap detected — skip the shorter/later match.
            kept.push_back(std::move(match));
        }
        _allMatches = std::move(kept);
    }

    assignLabels();
    _filteredMatches = _allMatches;
}

void HintModeHandler::activate(HintScanArea const& area, HintPatternSet patterns, HintAction action)
{
    _action = action;
    _patterns = std::move(patterns);
    rescanLines(area);

    _active = true;
    _executor.onHintModeEntered();
    _executor.requestRedraw();
}

void HintModeHandler::refresh(HintScanArea const& area)
{
    rescanLines(area);
    _executor.requestRedraw();
}

void HintModeHandler::applyScroll(LineOffset lines, LineCount historyLineCount)
{
    if (!_active)
        return;

    // The oldest row still in the buffer; anything above it has scrolled out for good.
    auto const top = -historyLineCount.as<LineOffset>();

    // Scrolling changes only positions, never labels, so the label-prefix filter membership is
    // unchanged: shift both lists in place rather than re-filtering _allMatches into _filteredMatches
    // (which would deep-copy every surviving match's strings on each scrolled line).
    auto const shiftAndPrune = [top, lines](std::vector<HintMatch>& matches) {
        for (auto& match: matches)
        {
            match.start.line -= lines;
            match.end.line -= lines;
        }
        std::erase_if(matches, [top](auto const& match) { return match.start.line < top; });
    };
    shiftAndPrune(_allMatches);
    shiftAndPrune(_filteredMatches);

    if (_allMatches.empty())
    {
        deactivate();
        return;
    }

    _executor.requestRedraw();
}

void HintModeHandler::deactivate()
{
    if (!_active)
        return;

    _active = false;
    _filter.clear();
    _allMatches.clear();
    _filteredMatches.clear();
    _executor.onHintModeExited();
    _executor.requestRedraw();
}

bool HintModeHandler::processInput(char32_t ch)
{
    if (!_active)
        return false;

    // Escape cancels hint mode.
    if (ch == U'\x1B')
    {
        deactivate();
        return true;
    }

    // Backspace removes last filter character.
    if (ch == U'\x08' || ch == U'\x7F')
    {
        if (!_filter.empty())
        {
            _filter.pop_back();
            updateFilteredMatches();
            _executor.requestRedraw();
        }
        return true;
    }

    // Only accept lowercase alphabetic characters for label typing.
    if (ch >= U'A' && ch <= U'Z')
        ch = ch - U'A' + U'a'; // Normalize to lowercase.

    if (ch < U'a' || ch > U'z')
        return true; // Ignore non-alphabetic input.

    _filter += static_cast<char>(ch);
    updateFilteredMatches();

    // Auto-select when exactly one match remains.
    if (_filteredMatches.size() == 1 && _filteredMatches[0].label == _filter)
    {
        auto match = std::move(_filteredMatches[0]);
        auto const action = _action;
        deactivate();
        _executor.onHintSelected(match, action);
        return true;
    }

    // If no matches remain, deactivate.
    if (_filteredMatches.empty())
    {
        deactivate();
        return true;
    }

    _executor.requestRedraw();
    return true;
}

void HintModeHandler::assignLabels()
{
    auto const matchCount = _allMatches.size();
    if (matchCount == 0)
        return;

    auto const radix = Alphabet.size();

    // The narrowest width that can name every match, 26 labels per character. Every label gets the
    // same width so none is a prefix of another, which is what lets progressive filtering
    // auto-select the moment the typed prefix is unique.
    auto labelWidth = size_t { 1 };
    for (auto capacity = radix; capacity < matchCount; capacity *= radix)
        ++labelWidth;

    for (auto const i: std::views::iota(size_t { 0 }, matchCount))
    {
        auto label = string(labelWidth, Alphabet.front());
        auto rest = i;
        for (auto digit = labelWidth; digit > 0; --digit)
        {
            label[digit - 1] = Alphabet[rest % radix];
            rest /= radix;
        }
        _allMatches[i].label = std::move(label);
    }
}

void HintModeHandler::updateFilteredMatches()
{
    _filteredMatches.clear();
    std::ranges::copy_if(_allMatches, std::back_inserter(_filteredMatches), [this](auto const& m) {
        return m.label.starts_with(_filter);
    });
}

} // namespace vtbackend
