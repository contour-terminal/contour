// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/HintModeHandler.h>

#include <algorithm>
#include <cctype>
#include <ranges>

using namespace std;

namespace vtbackend
{

auto utf8ByteOffsetToCodepointIndex(string_view text, size_t byteOffset) noexcept -> size_t
{
    auto const limit = min(byteOffset, text.size());
    // Count bytes that are NOT continuation bytes (10xxxxxx).
    return static_cast<size_t>(ranges::count_if(views::iota(size_t { 0 }, limit), [&](auto i) {
        return (static_cast<char8_t>(text[i]) & 0xC0) != 0x80;
    }));
}

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

                auto const startCol =
                    ColumnOffset::cast_from(utf8ByteOffsetToCodepointIndex(lineText, match.position()));
                auto const endCodepointIndex =
                    utf8ByteOffsetToCodepointIndex(lineText, match.position() + match.length());
                auto const endCol = ColumnOffset::cast_from(endCodepointIndex - 1);

                auto const actionText = pattern.transformer ? pattern.transformer(matchStr) : matchStr;

                _allMatches.push_back(HintMatch {
                    .label = {},
                    .matchedText = actionText,
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
            return std::string(remainder);
        if (auto const pos = remainder.find('/'); pos != std::string::npos)
        {
            // file://host/C:/path → C:/path : strip the leading slash before a Windows drive
            // letter so a host-qualified URL still yields a valid native absolute path.
            auto pathPart = remainder.substr(pos);
            if (pathPart.size() >= 3 && isDriveLetterPath(pathPart.substr(1)))
                return std::string(pathPart.substr(1));
            return std::string(pathPart);
        }
        return {};
    }

    // file:///C:/path → C:/path : strip the leading slash before a Windows drive letter so the
    // resulting string is a valid native absolute path rather than a rooted POSIX-looking one.
    if (remainder.size() >= 3 && remainder[0] == '/' && isDriveLetterPath(remainder.substr(1)))
        return std::string(remainder.substr(1));

    return std::string(remainder);
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
