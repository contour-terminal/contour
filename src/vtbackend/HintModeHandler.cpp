// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/HintModeHandler.h>

#include <algorithm>
#include <ranges>

using namespace std;

namespace vtbackend
{

HintModeHandler::HintModeHandler(Executor& executor): _executor { executor }
{
}

void HintModeHandler::rescanLines(vector<string> const& visibleLines, PageSize pageSize)
{
    _filter.clear();
    _allMatches.clear();
    _filteredMatches.clear();

    // Scan each visible line for regex matches.
    auto const lineCount = std::min(visibleLines.size(), static_cast<size_t>(unbox<int>(pageSize.lines)));
    for (auto const lineIdx: std::views::iota(size_t { 0 }, lineCount))
    {
        auto const& lineText = visibleLines[lineIdx];
        auto const lineOffset = LineOffset(static_cast<int>(lineIdx));

        for (auto const& pattern: _patterns)
        {
            auto matchIter = sregex_iterator(lineText.begin(), lineText.end(), pattern.regex);
            auto const matchEnd = sregex_iterator();

            for (; matchIter != matchEnd; ++matchIter)
            {
                auto const& match = *matchIter;
                if (match.empty())
                    continue;

                auto const matchStr = match.str();

                // Apply pattern-specific validator (e.g. filesystem existence check).
                if (pattern.validator && !pattern.validator(matchStr))
                    continue;

                auto const startCol = ColumnOffset(static_cast<int>(match.position()));
                auto const endCol = ColumnOffset(static_cast<int>(match.position() + match.length() - 1));

                _allMatches.push_back(HintMatch {
                    .label = {},
                    .matchedText = matchStr,
                    .start = CellLocation { .line = lineOffset, .column = startCol },
                    .end = CellLocation { .line = lineOffset, .column = endCol },
                });
            }
        }
    }

    // Sort matches top-to-bottom, left-to-right, longer matches first at same start.
    ranges::sort(_allMatches, [](auto const& a, auto const& b) {
        if (a.start.line != b.start.line)
            return a.start.line < b.start.line;
        if (a.start.column != b.start.column)
            return a.start.column < b.start.column;
        return a.end.column > b.end.column; // Longer match first at same start position.
    });

    // Remove duplicate matches (same text at same position).
    auto const [eraseBegin, eraseEnd] = ranges::unique(
        _allMatches, [](auto const& a, auto const& b) { return a.start == b.start && a.end == b.end; });
    _allMatches.erase(eraseBegin, eraseEnd);

    // Remove overlapping matches — keep the longer (earlier) match at each position.
    {
        auto kept = vector<HintMatch>();
        kept.reserve(_allMatches.size());
        for (auto& match: _allMatches)
        {
            if (!kept.empty() && kept.back().start.line == match.start.line
                && match.start.column <= kept.back().end.column)
                continue; // Overlap detected — skip the shorter/later match.
            kept.push_back(std::move(match));
        }
        _allMatches = std::move(kept);
    }

    assignLabels();
    _filteredMatches = _allMatches;
}

void HintModeHandler::activate(vector<string> const& visibleLines,
                               PageSize pageSize,
                               vector<HintPattern> const& patterns,
                               HintAction action)
{
    _action = action;
    _patterns = patterns;
    rescanLines(visibleLines, pageSize);

    _active = true;
    _executor.onHintModeEntered();
    _executor.requestRedraw();
}

void HintModeHandler::refresh(vector<string> const& visibleLines, PageSize pageSize)
{
    rescanLines(visibleLines, pageSize);
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
        auto const matchedText = _filteredMatches[0].matchedText;
        auto const action = _action;
        deactivate();
        _executor.onHintSelected(matchedText, action);
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

    auto const useTwoChar = matchCount > 26;

    for (auto const i: std::views::iota(size_t { 0 }, matchCount))
    {
        if (useTwoChar)
        {
            auto const first = static_cast<char>('a' + static_cast<int>(i / 26));
            auto const second = static_cast<char>('a' + static_cast<int>(i % 26));
            _allMatches[i].label = string { first, second };
        }
        else
        {
            _allMatches[i].label = string { static_cast<char>('a' + static_cast<int>(i)) };
        }
    }
}

void HintModeHandler::updateFilteredMatches()
{
    _filteredMatches.clear();
    std::ranges::copy_if(_allMatches, std::back_inserter(_filteredMatches), [this](auto const& m) {
        return m.label.starts_with(_filter);
    });
}

vector<HintPattern> HintModeHandler::builtinPatterns()
{
    static auto const cached = vector<HintPattern> {
        HintPattern { .name = "url",
                      .regex = regex(R"(https?://[^\s<>\"'\])\}]+)",
                                     regex_constants::ECMAScript | regex_constants::optimize),
                      .validator = {} },
        HintPattern { .name = "filepath",
                      .regex = regex(R"((?:~?/[\w./-]+|\.{1,2}/[\w./-]+|[\w][\w.-]*/[\w./-]+))",
                                     regex_constants::ECMAScript | regex_constants::optimize),
                      .validator = {} },
        HintPattern {
            .name = "githash",
            .regex = regex(R"(\b[0-9a-f]{7,40}\b)", regex_constants::ECMAScript | regex_constants::optimize),
            .validator = {} },
        HintPattern { .name = "ipv4",
                      .regex = regex(R"(\b\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}(?::\d+)?\b)",
                                     regex_constants::ECMAScript | regex_constants::optimize),
                      .validator = {} },
        HintPattern {
            .name = "ipv6",
            .regex =
                regex(R"((?:)"
                      R"(\b[0-9a-fA-F]{1,4}(?::[0-9a-fA-F]{1,4}){7}\b)"
                      R"(|\b(?:[0-9a-fA-F]{1,4}:)*[0-9a-fA-F]{1,4}::(?:[0-9a-fA-F]{1,4}:)*[0-9a-fA-F]{1,4}\b)"
                      R"(|::(?:[0-9a-fA-F]{1,4}:)*[0-9a-fA-F]{1,4}\b)"
                      R"(|\b(?:[0-9a-fA-F]{1,4}:)+:(?![0-9a-fA-F:]))"
                      R"())",
                      regex_constants::ECMAScript | regex_constants::optimize),
            .validator = {} },
    };
    return cached;
}

auto extractPathFromFileUrl(std::string const& url) -> std::string
{
    constexpr auto Prefix = std::string_view("file://");
    if (!url.starts_with(Prefix))
        return url;
    auto remainder = url.substr(Prefix.size());
    // file:///path → /path  ;  file://host/path → /path
    if (!remainder.empty() && remainder[0] != '/')
    {
        if (auto const pos = remainder.find('/'); pos != std::string::npos)
            return std::string(remainder.substr(pos));
        return {};
    }
    return std::string(remainder);
}

} // namespace vtbackend
