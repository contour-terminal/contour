// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/HintModeHandler.h>

#include <algorithm>
#include <cctype>
#include <ranges>

using namespace std;

namespace vtbackend
{

namespace
{
    /// The characters labels are built from — the same set @ref HintModeHandler::processInput accepts.
    constexpr auto Alphabet = string_view { "abcdefghijklmnopqrstuvwxyz" };
} // namespace

auto Utf8CodepointCursor::codepointIndexAt(size_t byteOffset) noexcept -> size_t
{
    auto const limit = min(byteOffset, _text.size());
    // Count bytes that are NOT continuation bytes (10xxxxxx), resuming where the last call stopped.
    for (; _byteOffset < limit; ++_byteOffset)
        if ((static_cast<char8_t>(_text[_byteOffset]) & 0xC0) != 0x80)
            ++_codepointIndex;
    return _codepointIndex;
}

auto utf8ByteOffsetToCodepointIndex(string_view text, size_t byteOffset) noexcept -> size_t
{
    return Utf8CodepointCursor { text }.codepointIndexAt(byteOffset);
}

auto buildLogicalLines(vector<HintScanRow> const& rows) -> vector<HintLogicalLine>
{
    auto result = vector<HintLogicalLine>();

    for (auto const& row: rows)
    {
        auto const codepoints = utf8ByteOffsetToCodepointIndex(row.text, row.text.size());

        // A continuation row extends the logical line above it. Two cases start a new one anyway:
        // there is no row above (the head scrolled out of the scanned range, so this row is all we
        // can see of its logical line), and a gap in the row offsets (which would break the
        // firstLine + rowIndex arithmetic gridPositionOf() relies on).
        auto const continuesPrevious =
            row.continuation == LineContinuation::Yes && !result.empty()
            && row.line
                   == result.back().firstLine + LineOffset::cast_from(result.back().rowCodepointEnds.size());

        if (continuesPrevious)
        {
            auto& logical = result.back();
            logical.text += row.text;
            logical.rowCodepointEnds.push_back(logical.rowCodepointEnds.back() + codepoints);
            continue;
        }

        result.push_back(HintLogicalLine {
            .text = row.text,
            .firstLine = row.line,
            .rowCodepointEnds = { codepoints },
        });
    }

    return result;
}

auto gridPositionOf(HintLogicalLine const& logical, size_t codepointIndex) noexcept -> CellLocation
{
    auto const& ends = logical.rowCodepointEnds;

    // The first row whose exclusive end is past the index is the row the index falls on.
    auto const it = ranges::upper_bound(ends, codepointIndex);
    if (it != ends.end())
    {
        auto const rowIndex = static_cast<size_t>(std::distance(ends.begin(), it));
        auto const rowStart = rowIndex == 0 ? size_t { 0 } : ends[rowIndex - 1];
        return CellLocation { .line = logical.firstLine + LineOffset::cast_from(rowIndex),
                              .column = ColumnOffset::cast_from(codepointIndex - rowStart) };
    }

    // Past the end: clamp to the last cell that exists. Asking for the final codepoint takes the
    // branch above, so this recurses exactly once.
    if (ends.empty() || ends.back() == 0)
        return CellLocation { .line = logical.firstLine, .column = ColumnOffset(0) };
    return gridPositionOf(logical, ends.back() - 1);
}

HintModeHandler::HintModeHandler(Executor& executor): _executor { executor }
{
}

void HintModeHandler::rescanLines(HintScanArea const& area)
{
    _filter.clear();
    _allMatches.clear();
    _filteredMatches.clear();

    // Match against logical lines, not physical rows: a URL broken across a wrap is one match.
    //
    // A row that wrapped mid-wide-character left a pad space in its last cell, and joining the rows
    // therefore inserts that space inside such a match. Every terminal that pads this way has the
    // same limitation; the alternative is to guess which trailing blanks are padding.
    for (auto const& logical: buildLogicalLines(area.rows))
    {
        for (auto const& pattern: _patterns)
        {
            auto matchIter = sregex_iterator(logical.text.begin(), logical.text.end(), pattern.regex);
            auto const matchEnd = sregex_iterator();

            // One forward pass per pattern: sregex_iterator yields non-overlapping matches in
            // increasing position order, so every offset this converts is non-decreasing.
            auto codepointIndexOf = Utf8CodepointCursor { logical.text };

            for (; matchIter != matchEnd; ++matchIter)
            {
                auto const& match = *matchIter;
                if (match.empty())
                    continue;

                auto const startIndex =
                    codepointIndexOf.codepointIndexAt(static_cast<size_t>(match.position()));
                auto const start = gridPositionOf(logical, startIndex);

                // The label is drawn at the match start, so a match starting on a row that cannot
                // carry one could never be selected: do not offer it. Tested before the validator,
                // which may go to the filesystem.
                if (!area.labelableRows.contains(start.line))
                    continue;

                auto const matchStr = match.str();

                // Apply pattern-specific validator (e.g. filesystem existence check).
                if (pattern.validator && !pattern.validator(matchStr))
                    continue;

                auto const endIndex =
                    codepointIndexOf.codepointIndexAt(static_cast<size_t>(match.position() + match.length()));

                _allMatches.push_back(HintMatch {
                    .label = {},
                    .matchedText = pattern.transformer ? pattern.transformer(matchStr) : matchStr,
                    .start = start,
                    .end = gridPositionOf(logical, endIndex - 1),
                });
            }
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

void HintModeHandler::activate(HintScanArea const& area,
                               vector<HintPattern> const& patterns,
                               HintAction action)
{
    _action = action;
    _patterns = patterns;
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
        _executor.onHintSelected(match.matchedText, action, match.start, match.end);
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

vector<HintPattern> HintModeHandler::builtinPatterns()
{
    static auto const cached = vector<HintPattern> {
        HintPattern { .name = "url",
                      .regex = regex(R"(https?://[^\s<>\"'\])\}]+)",
                                     regex_constants::ECMAScript | regex_constants::optimize),
                      .validator = {},
                      .transformer = {} },
        HintPattern { .name = "filepath",
                      .regex = regex(R"((?:~?/[\w./-]+|\.{1,2}/[\w./-]+|[\w][\w.-]*/[\w./-]+))",
                                     regex_constants::ECMAScript | regex_constants::optimize),
                      .validator = {},
                      .transformer = {} },
        HintPattern {
            .name = "githash",
            .regex = regex(R"(\b[0-9a-f]{7,40}\b)", regex_constants::ECMAScript | regex_constants::optimize),
            .validator = {},
            .transformer = {} },
        HintPattern { .name = "ipv4",
                      .regex = regex(R"(\b\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}(?::\d+)?\b)",
                                     regex_constants::ECMAScript | regex_constants::optimize),
                      .validator = {},
                      .transformer = {} },
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
            .validator = {},
            .transformer = {} },
    };
    return cached;
}

auto extractPathFromFileUrl(std::string const& url) -> std::string
{
    constexpr auto Prefix = std::string_view("file://");
    if (!url.starts_with(Prefix))
        return url;
    auto remainder = url.substr(Prefix.size());

    // A Windows drive-letter authority (e.g. file://C:/path) is not a real host: keep it.
    auto const isDriveLetterPath = [](std::string_view path) {
        return path.size() >= 2 && (std::isalpha(static_cast<unsigned char>(path[0])) != 0) && path[1] == ':';
    };

    // file:///path → /path  ;  file://host/path → /path  ;  file://C:/path → C:/path
    if (!remainder.empty() && remainder[0] != '/')
    {
        if (isDriveLetterPath(remainder))
            return remainder;
        if (auto const pos = remainder.find('/'); pos != std::string::npos)
        {
            // file://host/C:/path → C:/path : strip the leading slash before a Windows drive
            // letter so a host-qualified URL still yields a valid native absolute path.
            auto pathPart = remainder.substr(pos);
            if (pathPart.size() >= 3 && isDriveLetterPath(pathPart.substr(1)))
                return pathPart.substr(1);
            return pathPart;
        }
        return {};
    }

    // file:///C:/path → C:/path : strip the leading slash before a Windows drive letter so the
    // resulting string is a valid native absolute path rather than a rooted POSIX-looking one.
    if (remainder.size() >= 3 && remainder[0] == '/' && isDriveLetterPath(remainder.substr(1)))
        return remainder.substr(1);

    return remainder;
}

namespace
{
    /// The DNS label before the first '.', lower-cased: the bare machine name of a possibly-qualified
    /// host, so "fedora" and "fedora.corp.example" compare equal.
    [[nodiscard]] std::string bareHostLabel(std::string_view host)
    {
        auto label = std::string(host.substr(0, host.find('.')));
        std::ranges::transform(
            label, label.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return label;
    }
} // namespace

auto localWorkingDirectory(std::string const& url, std::string_view localHost) -> std::optional<std::string>
{
    constexpr auto Prefix = std::string_view("file://");
    if (url.starts_with(Prefix))
    {
        auto const remainder = std::string_view(url).substr(Prefix.size());

        // The host is the authority up to the first '/'. file:///path is rooted (no host), and a Windows
        // drive-letter authority (file://C:/path) is a path, not a host.
        auto const isDriveLetter = remainder.size() >= 2
                                   && (std::isalpha(static_cast<unsigned char>(remainder[0])) != 0)
                                   && remainder[1] == ':';
        auto host = std::string_view {};
        if (!remainder.empty() && remainder.front() != '/' && !isDriveLetter)
            host = remainder.substr(0, remainder.find('/'));

        if (!host.empty())
        {
            auto const label = bareHostLabel(host);
            if (label != "localhost" && label != bareHostLabel(localHost))
                return std::nullopt; // a different host: this is a remote working directory
        }
    }

    auto path = extractPathFromFileUrl(url);
    if (path.empty())
        return std::nullopt;
    return path;
}

} // namespace vtbackend
