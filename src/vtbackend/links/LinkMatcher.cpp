// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/links/LinkMatcher.hpp>

#include <vtbackend/Logging.hpp>
#include <vtbackend/core/Utf8Prefix.hpp>

#include <libunicode/codepoint_properties.h>
#include <libunicode/utf8.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <optional>
#include <ranges>
#include <regex>
#include <tuple>
#include <variant>

namespace vtbackend
{

namespace
{
    constexpr auto KeptPunctuation = std::string_view { "/&-*_" };

    struct Bracket
    {
        char32_t open;
        char32_t close;
    };
    constexpr auto Brackets =
        std::array { Bracket { U'(', U')' }, Bracket { U'[', U']' }, Bracket { U'{', U'}' } };

    /// The schemes built-in URL detection opens (§7.6): the only ones printed text may choose. `file` goes on
    /// as the local path it names, never as a URL.
    constexpr auto PrintableSchemes =
        std::array<std::string_view, 5> { "http", "https", "ftp", "mailto", "file" };

    /// Whether @p target, whose scheme the printed text chose, opens with one of PrintableSchemes.
    [[nodiscard]] bool hasPrintableScheme(std::string_view target) noexcept
    {
        return std::ranges::any_of(PrintableSchemes,
                                   [&](std::string_view allowed) { return hasUrlScheme(target, allowed); });
    }

    /// Whether a match of @p pattern opening @p target may be opened (§7.6): a path goes the path's way, and
    /// a URL or a custom link whose scheme the printed text chose only to a built-in URL scheme; a template's
    /// own literal scheme, which the user wrote, may be any.
    [[nodiscard]] LinkOpening openingOf(LinkPattern const& pattern, std::string_view target) noexcept
    {
        if (pattern.kind == LinkKind::Path)
            return LinkOpening::Allowed;
        auto const source =
            pattern.urlTemplate ? pattern.urlTemplate->schemeSource() : LinkSchemeSource::Printed;
        return source == LinkSchemeSource::Template || hasPrintableScheme(target) ? LinkOpening::Allowed
                                                                                  : LinkOpening::Refused;
    }

    constexpr auto PunctuationCategories = std::array {
        unicode::General_Category::Connector_Punctuation, unicode::General_Category::Dash_Punctuation,
        unicode::General_Category::Open_Punctuation,      unicode::General_Category::Close_Punctuation,
        unicode::General_Category::Initial_Punctuation,   unicode::General_Category::Final_Punctuation,
        unicode::General_Category::Other_Punctuation,
    };

    /// The last codepoint of the non-empty @p text, and how many bytes it takes. Decodes in place, without
    /// allocating; malformed UTF-8 reads as U+FFFD.
    [[nodiscard]] std::pair<char32_t, std::size_t> lastCodepoint(std::string_view text) noexcept
    {
        auto begin = text.size() - 1;
        while (begin > 0 && isUtf8ContinuationByte(text[begin]))
            --begin;
        auto state = unicode::utf8_decoder_state {};
        auto result = unicode::ConvertResult { unicode::Incomplete {} };
        for (auto const byte: text.substr(begin))
            result = unicode::from_utf8(state, static_cast<uint8_t>(byte));
        auto const* const decoded = std::get_if<unicode::Success>(&result);
        return { decoded != nullptr ? decoded->value : U'�', text.size() - begin };
    }

    [[nodiscard]] bool isPunctuation(char32_t codepoint) noexcept
    {
        auto const category = unicode::codepoint_properties::get(codepoint).general_category;
        return std::ranges::find(PunctuationCategories, category) != PunctuationCategories.end();
    }

    [[nodiscard]] std::optional<int> numberOf(std::csub_match const& group)
    {
        if (!group.matched)
            return std::nullopt;
        auto value = 0;
        auto const [end, error] = std::from_chars(group.first, group.second, value);
        if (error != std::errc {} || end != group.second)
            return std::nullopt;
        return value;
    }

    /// A path's location: `:line[:column]` in groups 2-3, `(line[,column])` in 4-5. A suffix naming a line
    /// only has column 1; a line or column that was printed but does not fit an int is no location at all.
    [[nodiscard]] std::optional<LinkLocation> locationOf(std::cmatch const& match)
    {
        for (auto const& [lineGroup, columnGroup]: std::array { std::pair { 2, 3 }, std::pair { 4, 5 } })
        {
            if (!match[lineGroup].matched)
                continue;
            auto const line = numberOf(match[lineGroup]);
            if (!line)
                return std::nullopt;
            if (!match[columnGroup].matched)
                return LinkLocation { .line = *line, .column = 1 };
            auto const column = numberOf(match[columnGroup]);
            if (!column)
                return std::nullopt;
            return LinkLocation { .line = *line, .column = *column };
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<LinkCandidate> candidateOf(ScanText const& scan,
                                                           LinkPattern const& pattern,
                                                           std::size_t patternIndex,
                                                           std::cmatch const& match)
    {
        auto const spanGroup = pattern.kind == LinkKind::Path ? 1 : 0;
        if (match.length(0) == 0 || match.size() <= static_cast<std::size_t>(spanGroup)
            || !match[spanGroup].matched)
            return std::nullopt;

        auto const begin = static_cast<std::size_t>(match.position(spanGroup));
        auto targetEnd = begin + static_cast<std::size_t>(match.length(spanGroup));
        auto const location = pattern.kind == LinkKind::Path ? locationOf(match) : std::nullopt;
        // A located path's underline runs through its location suffix, which ends the whole match.
        auto end = location ? static_cast<std::size_t>(match.position(0) + match.length(0)) : targetEnd;
        if (pattern.kind != LinkKind::Custom && !location)
        {
            targetEnd = begin + trimmedLength(scan.text().substr(begin, targetEnd - begin));
            end = targetEnd;
        }
        if (targetEnd <= begin)
            return std::nullopt;
        // A link touching a clipped end may go on beyond it: dropped, not reported truncated (§4.1).
        if ((scan.clip().front == ScanEdge::Clipped && begin == 0)
            || (scan.clip().back == ScanEdge::Clipped && end == scan.text().size()))
            return std::nullopt;

        auto target = std::string { scan.original(begin, targetEnd) };
        // A user pattern's url: opens its expansion, with the groups read from the text as printed: the
        // blanking keeps every byte offset, so one offset addresses both.
        if (pattern.kind == LinkKind::Custom && pattern.urlTemplate)
            target = pattern.urlTemplate->expand([&](int group) -> std::string_view {
                auto const index = static_cast<std::size_t>(group);
                if (index >= match.size() || !match[index].matched)
                    return {};
                auto const at = static_cast<std::size_t>(match.position(index));
                return scan.original(at, at + static_cast<std::size_t>(match.length(index)));
            });

        auto const opening = openingOf(pattern, target);
        return LinkCandidate {
            .kind = pattern.kind,
            .patternIndex = patternIndex,
            .byteBegin = begin,
            .byteEnd = end,
            .first = scan.positionOf(scan.firstCellAt(begin)),
            .last = scan.positionOf(scan.lastCellBefore(end)),
            .text = std::string { scan.original(begin, end) },
            .target = std::move(target),
            .location = location,
            .opening = opening,
        };
    }

    /// Where a candidate ranks among others of the same span: a path is one only once its file is found, so
    /// every other pattern's match there goes first -- `ABC-123` is a ticket before it is a bare file name.
    [[nodiscard]] constexpr int spanRank(LinkKind kind) noexcept
    {
        return kind == LinkKind::Path ? 1 : 0;
    }

    void mergeOverlapping(std::vector<LinkCandidate>& candidates)
    {
        // Earliest start, then longest, then a match that is no path, then table order.
        std::ranges::sort(candidates, [](LinkCandidate const& a, LinkCandidate const& b) {
            return std::tuple { a.byteBegin, b.byteEnd, spanRank(a.kind), a.patternIndex }
                   < std::tuple { b.byteBegin, a.byteEnd, spanRank(b.kind), b.patternIndex };
        });
        auto kept = std::vector<LinkCandidate> {};
        for (auto& candidate: candidates)
            if (kept.empty() || candidate.byteBegin >= kept.back().byteEnd)
                kept.push_back(std::move(candidate));
        candidates = std::move(kept);
    }
} // namespace

std::size_t trimmedLength(std::string_view span) noexcept
{
    auto length = span.size();
    while (length > 0)
    {
        auto const kept = span.substr(0, length);
        auto const [codepoint, bytes] = lastCodepoint(kept);
        if (auto const bracket = std::ranges::find(Brackets, codepoint, &Bracket::close);
            bracket != Brackets.end())
        {
            // A closing bracket the match also opened belongs to the link: Foo_(bar).
            if (std::ranges::count(kept, static_cast<char>(bracket->close))
                <= std::ranges::count(kept, static_cast<char>(bracket->open)))
                break;
        }
        else if (!isPunctuation(codepoint)
                 || (codepoint < 0x80 && KeptPunctuation.contains(static_cast<char>(codepoint))))
            break;
        length -= bytes;
    }
    return length;
}

std::vector<LinkCandidate> findLinks(ScanText const& text,
                                     LinkPatterns const& patterns,
                                     PatternSelection selection,
                                     LinkOverlap overlap)
{
    auto candidates = std::vector<LinkCandidate> {};
    auto const subject = text.text();
    for (auto const index: std::views::iota(std::size_t { 0 }, patterns.patterns().size()))
    {
        auto const& pattern = patterns.patterns()[index];
        if (selection == PatternSelection::ClickableOnly && pattern.activation != LinkActivation::Clickable)
            continue;
        try
        {
            auto const matches = std::ranges::subrange(
                std::cregex_iterator(subject.data(), subject.data() + subject.size(), pattern.regex),
                std::cregex_iterator());
            for (auto const& match: matches)
                if (auto candidate = candidateOf(text, pattern, index, match);
                    candidate
                    && (selection == PatternSelection::All || candidate->opening == LinkOpening::Allowed))
                    candidates.push_back(std::move(*candidate));
        }
        catch (std::regex_error const& error)
        {
            // MSVC throws error_stack / error_complexity on a pathological line: this pattern stops there,
            // keeping the candidates it found before the throw, and the other patterns still run.
            // One message per pattern per scan, and only when the category is enabled.
            terminalLog()("Link pattern '{}' gave up on a line: {}", pattern.name, error.what());
        }
    }
    if (overlap == LinkOverlap::Merge)
        mergeOverlapping(candidates);
    return candidates;
}

} // namespace vtbackend
