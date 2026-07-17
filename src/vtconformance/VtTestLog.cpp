// SPDX-License-Identifier: Apache-2.0
#include <crispy/utils.h>

#include <array>
#include <charconv>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vtconformance/VtTestLog.h>

using namespace std::string_view_literals;

namespace vtconformance
{

namespace
{
    /// vttest's record tags, verbatim from its `vttest.h`.
    constexpr auto RecordTags = std::array {
        std::pair { "Note: "sv, VtTestRecordKind::Note }, std::pair { "Send: "sv, VtTestRecordKind::Send },
        std::pair { "Data: "sv, VtTestRecordKind::Data }, std::pair { "Text: "sv, VtTestRecordKind::Text },
        std::pair { "Read: "sv, VtTestRecordKind::Read }, std::pair { "Wait: "sv, VtTestRecordKind::Wait },
        std::pair { "Done: "sv, VtTestRecordKind::Done }, std::pair { "Skip: "sv, VtTestRecordKind::Skip },
    };

    /// Whether a record's payload is byte-encoded (`<27> [ 0 c`) rather than plain text.
    [[nodiscard]] constexpr bool isByteEncoded(VtTestRecordKind kind) noexcept
    {
        return kind == VtTestRecordKind::Send || kind == VtTestRecordKind::Data
               || kind == VtTestRecordKind::Read;
    }

    /// vttest's self-check verdict phrasings, in **first-match-wins order**.
    ///
    /// Order is load-bearing: "No communication errors" contains "ommunication errors", and several
    /// failure lines contain "ok" as an incidental substring. Passing phrasings that could be
    /// swallowed by a broader failure pattern must therefore come first.
    ///
    /// This table is the authority on what counts as a vttest verdict. Adding a phrasing is adding
    /// a row.
    struct VerdictPattern
    {
        std::string_view needle;
        bool passed;
    };

    constexpr auto VerdictPatterns = std::array {
        VerdictPattern { "No communication errors"sv, true },
        VerdictPattern { "Not expected"sv, false },
        VerdictPattern { "Unknown response"sv, false },
        // vttest also reports a reply it could not identify as the bare, lower-case word `unknown` --
        // a *different* string from "Unknown response" above, and its own kind of failure: the
        // terminal answered, but with something the test could not name. Two tests end that way, both
        // by falling through a lookup: tst_DECRQUPSS when parse_upss_name() matches no charset
        // (charsets.c:913), and the VT220 keyboard-language report on an unrecognised code
        // (vt220.c:104).
        //
        // Matched with its `result ` prefix attached rather than on its own: `unknown` is an ordinary
        // English word that appears in vttest's prose, and a bare needle would let a chapter's own
        // narration cast a verdict. @see ResultNotePrefix.
        VerdictPattern { "result unknown"sv, false },
        VerdictPattern { "failed"sv, false },
        VerdictPattern { "Communication errors"sv, false },
        VerdictPattern { "-- OK"sv, true },
        VerdictPattern { "TERMINAL OK"sv, true },
        VerdictPattern { "Autowrap-pending: OK"sv, true },
        VerdictPattern { "ok (expect"sv, true },
    };

    /// The prefix vttest's `show_result()` gives every verdict it logs.
    ///
    /// It writes `Note: result <verdict>` (main.c:2104-2109), not `Text:` -- so a reader that only
    /// scans `Text:` sees whichever verdicts happen to reach the log through `printxx` instead, and
    /// silently ignores the rest. In chapter 06 that was 2 of 22.
    constexpr auto ResultNotePrefix = "result "sv;

    /// @return Whether @p record can carry a verdict at all.
    ///
    /// `Text:` is vttest's test prose, which is where a verdict printed inline ends up. `Note:` is its
    /// commentary, and only the `result ` ones are verdicts -- the others name menu choices and
    /// bookkeeping ("Note: choice 9: Test of known bugs"), which must not be pattern-matched or a
    /// chapter's own title could read as a failure.
    [[nodiscard]] bool carriesVerdict(VtTestRecord const& record) noexcept
    {
        return record.kind == VtTestRecordKind::Text
               || (record.kind == VtTestRecordKind::Note && record.payload.starts_with(ResultNotePrefix));
    }

    /// Names for the control bytes a VT report is made of, so reports read like sequences.
    constexpr auto ControlNames = std::array {
        std::pair { '\x1b', "ESC"sv }, std::pair { '\x9b', "CSI"sv }, std::pair { '\x90', "DCS"sv },
        std::pair { '\x9c', "ST"sv },  std::pair { '\x07', "BEL"sv },
    };
} // namespace

std::string decodeBytes(std::string_view encoded)
{
    auto result = std::string {};

    while (!encoded.empty())
    {
        if (encoded.front() == ' ')
        {
            encoded.remove_prefix(1);
            continue;
        }

        if (encoded.front() == '<')
        {
            auto const close = encoded.find('>');
            if (close != std::string_view::npos)
            {
                auto const digits = encoded.substr(1, close - 1);
                auto value = 0u;
                auto const [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
                if (ec == std::errc {} && ptr == digits.data() + digits.size() && value <= 0xFF)
                {
                    result += static_cast<char>(static_cast<unsigned char>(value));
                    encoded.remove_prefix(close + 1);
                    continue;
                }
            }
        }

        result += encoded.front();
        encoded.remove_prefix(1);
    }

    return result;
}

std::vector<VtTestRecord> parseVtTestLog(std::string_view transcript)
{
    auto records = std::vector<VtTestRecord> {};

    auto const lines = crispy::split(transcript, '\n');
    for (auto const& [index, line]: crispy::views::enumerate(lines))
    {
        auto const lineNumber = index + 1;

        for (auto const& [tag, kind]: RecordTags)
        {
            // vttest writes the tag with its trailing space, but an empty payload (an unanswered
            // query) leaves the line as a bare "Read:" with the space trimmed by nothing at all —
            // accept both spellings.
            auto const bare = tag.substr(0, tag.size() - 1);
            if (!line.starts_with(bare))
                continue;

            auto payload = line.substr(bare.size());
            if (payload.starts_with(' '))
                payload.remove_prefix(1);

            records.push_back(VtTestRecord {
                .kind = kind,
                .payload = isByteEncoded(kind) ? decodeBytes(payload) : std::string(payload),
                .lineNumber = lineNumber,
            });
            break;
        }
    }

    return records;
}

std::vector<Query> extractQueries(std::vector<VtTestRecord> const& records)
{
    auto queries = std::vector<Query> {};

    // A query is a `Send:` (a control sequence) immediately followed by a `Wait:`; the answer is the
    // first `Read:` before the matching `Done:`. vttest guarantees this bracketing because it
    // suspends command-file replay across a reply-read.
    //
    // The `Send:` prefix is what makes an empty reply *mean* something. vttest brackets its waits for
    // HUMAN input the very same way -- "Push the RETURN key:" is a `Data:` followed by an unanswered
    // `Wait:` -- so keying off `Wait:` alone would report every keyboard prompt in the suite as a
    // terminal that failed to answer. Requiring a preceding `Send:` keeps the oracle honest.
    for (auto index = size_t { 0 }; index < records.size(); ++index)
    {
        if (records[index].kind != VtTestRecordKind::Wait)
            continue;

        if (index == 0 || records[index - 1].kind != VtTestRecordKind::Send)
            continue;

        auto query = Query { .request = records[index - 1].payload,
                             .reply = {},
                             .lineNumber = records[index].lineNumber };

        for (auto scan = index + 1; scan < records.size(); ++scan)
        {
            if (records[scan].kind == VtTestRecordKind::Done)
                break;
            if (records[scan].kind == VtTestRecordKind::Read)
            {
                query.reply = records[scan].payload;
                break;
            }
        }

        queries.push_back(std::move(query));
    }

    return queries;
}

std::vector<Verdict> extractVerdicts(std::vector<VtTestRecord> const& records)
{
    auto verdicts = std::vector<Verdict> {};

    for (auto const& record: records)
    {
        if (!carriesVerdict(record))
            continue;

        for (auto const& pattern: VerdictPatterns)
        {
            if (record.payload.find(pattern.needle) == std::string::npos)
                continue;

            verdicts.push_back(Verdict {
                .text = record.payload,
                .passed = pattern.passed,
                .lineNumber = record.lineNumber,
            });
            break;
        }
    }

    return verdicts;
}

std::string prettyBytes(std::string_view bytes)
{
    auto result = std::string {};

    for (auto const ch: bytes)
    {
        auto named = false;
        for (auto const& [byte, name]: ControlNames)
            if (ch == byte)
            {
                result += name;
                named = true;
                break;
            }

        if (named)
            continue;

        auto const value = static_cast<unsigned char>(ch);
        if (value >= 0x20 && value < 0x7F)
            result += ch;
        else
            result += std::format("<0x{:02X}>", value);
    }

    return result;
}

} // namespace vtconformance
