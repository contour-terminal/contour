// SPDX-License-Identifier: Apache-2.0
#include <contour/cli/AgentTranscript.hpp>
#include <contour/cli/AgentTranscriptJson.hpp>
#include <contour/cli/ExitStatus.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <ranges>
#include <system_error>
#include <utility>

#include <glaze/glaze.hpp>
#include <vthost/control/StatusText.hpp>

using namespace std::string_view_literals;

namespace contour::cli
{

namespace
{
    using detail::Binding;
    using detail::LineExtent;
    using detail::LineFields;
    using detail::LogLine;
    using detail::LookupFailure;
    using detail::TypeField;
    using detail::WireEntry;
    using detail::WirePage;

    using Kind = TranscriptEntryKind;
    using Treatment = EntryTreatment;

    /// A line's own treatment, whatever its message holds: metadata and attachments.
    [[nodiscard]] constexpr EntryRow ignoredType(std::string_view type) noexcept
    {
        return EntryRow { .type = type, .block = ""sv, .treatment = Treatment::Ignored, .kind = Kind::Note };
    }

    /// A `system` subtype, known and not shown.
    [[nodiscard]] constexpr EntryRow ignoredSystem(std::string_view subtype) noexcept
    {
        return EntryRow {
            .type = "system"sv, .block = subtype, .treatment = Treatment::Ignored, .kind = Kind::Note
        };
    }

    /// A message block that is an entry of the transcript.
    [[nodiscard]] constexpr EntryRow shown(std::string_view type, std::string_view block, Kind kind) noexcept
    {
        return EntryRow { .type = type, .block = block, .treatment = Treatment::Shown, .kind = kind };
    }

    /// The log lines Claude Code writes, as measured from logs of versions 2.1.322 to 2.1.328. A newer one
    /// adding a metadata type raises the not-understood count until it gets its row here.
    inline constexpr auto EntryRows = std::array {
        // Metadata: never a turn, whatever it holds (api-request-blob carries a message, and is no turn).
        ignoredType("last-prompt"),
        ignoredType("mode"),
        ignoredType("permission-mode"),
        ignoredType("session-owner"),
        ignoredType("bridge-session"),
        ignoredType("custom-title"),
        ignoredType("agent-name"),
        ignoredType("ai-title"),
        ignoredType("queue-operation"),
        ignoredType("file-history-snapshot"),
        ignoredType("file-history-delta"),
        ignoredType("api-request"),
        ignoredType("api-request-shape"),
        ignoredType("api-request-blob"),
        ignoredType("atis-latch"),
        ignoredType("tools-used"),
        ignoredType("pr-link"),
        ignoredType("cost-state"),
        ignoredType("frame-link"),
        ignoredType("artifact-comment-monitor"),
        ignoredType("artifact-autoreact-ledger"),
        ignoredType("history-suppression"),
        ignoredType("attachment"),
        // `system` lines, by subtype: only a compaction is shown.
        ignoredSystem("turn_duration"),
        ignoredSystem("stop_hook_summary"),
        ignoredSystem("local_command"),
        ignoredSystem("away_summary"),
        ignoredSystem("informational"),
        ignoredSystem("scheduled_task_fire"),
        ignoredSystem("bridge_status"),
        shown("system", "compact_boundary", Kind::Note),
        // Turns, by message block. A `user` prompt typed as a plain string reads as a `text` block.
        shown("user", "text", Kind::User),
        shown("user", "tool_result", Kind::ToolResult),
        shown("assistant", "text", Kind::Assistant),
        shown("assistant", "tool_use", Kind::ToolCall),
        EntryRow { .type = "assistant"sv,
                   .block = "thinking"sv,
                   .treatment = Treatment::Ignored,
                   .kind = Kind::Note },
    };

    /// The entry flags that keep a turn out of the transcript: injected context, a subagent's turn, and the
    /// summary a compaction writes in the user's name.
    enum class TurnFlag : uint8_t
    {
        Meta = 0,
        Sidechain,
        CompactSummary,
    };

    /// The text a Note shows, by the block that made it.
    struct NoteText
    {
        std::string_view block;
        std::string_view text;
    };
    inline constexpr auto NoteTexts = std::array {
        NoteText { .block = "compact_boundary"sv, .text = "conversation compacted"sv },
    };

    /// The kinds as format 1 spells them.
    inline constexpr auto KindNames = std::array {
        std::pair { Kind::User, "user"sv },
        std::pair { Kind::Assistant, "assistant"sv },
        std::pair { Kind::ToolCall, "tool-call"sv },
        std::pair { Kind::ToolResult, "tool-result"sv },
        std::pair { Kind::ToolError, "tool-error"sv },
        std::pair { Kind::Note, "note"sv },
    };

    [[nodiscard]] std::string_view nameOf(Kind kind) noexcept
    {
        auto const row = std::ranges::find(KindNames, kind, &std::pair<Kind, std::string_view>::first);
        return row != KindNames.end() ? row->second : "note"sv;
    }

    [[nodiscard]] EntryRow const* rowFor(std::string_view type, std::string_view block) noexcept
    {
        auto const row = std::ranges::find_if(EntryRows, [&](EntryRow const& candidate) {
            return candidate.type == type && candidate.block == block;
        });
        return row != EntryRows.end() ? &*row : nullptr;
    }

    [[nodiscard]] bool isKnownType(std::string_view type) noexcept
    {
        return std::ranges::contains(EntryRows, type, &EntryRow::type);
    }

    // {{{ the shapes read from a line

    [[nodiscard]] bool has(LineFields const& fields, TurnFlag flag) noexcept
    {
        switch (flag)
        {
            case TurnFlag::Meta: return fields.isMeta.value_or(false);
            case TurnFlag::Sidechain: return fields.isSidechain.value_or(false);
            case TurnFlag::CompactSummary: return fields.isCompactSummary.value_or(false);
        }
        return false;
    }

    inline constexpr auto HiddenTurnFlags =
        std::array { TurnFlag::Meta, TurnFlag::Sidechain, TurnFlag::CompactSummary };

    /// Unknown keys skipped, nothing past the line read.
    inline constexpr auto LineReadOptions =
        glz::opts { .null_terminated = false, .error_on_unknown_keys = false };

    /// As LineReadOptions, and stopping once `type` is read, so that a cut line still names its type.
    inline constexpr auto TypeReadOptions =
        glz::opts { .null_terminated = false, .error_on_unknown_keys = false, .partial_read = true };
    // }}}

    // {{{ the text of an entry
    using Object = glz::generic::object_t;
    using Array = glz::generic::array_t;

    [[nodiscard]] glz::generic const* memberOf(glz::generic const& node, std::string_view key) noexcept
    {
        auto const* object = node.get_if<Object>();
        if (object == nullptr)
            return nullptr;
        auto const member = object->find(key);
        return member != object->end() ? &member->second : nullptr;
    }

    [[nodiscard]] std::string_view textOf(glz::generic const* node) noexcept
    {
        auto const* text = node != nullptr ? node->get_if<std::string>() : nullptr;
        return text != nullptr ? std::string_view { *text } : std::string_view {};
    }

    /// Cuts valid UTF-8 to at most @p maxBytes, on a code-point boundary.
    void cutAtCodePoint(std::string& text, std::size_t maxBytes)
    {
        if (text.size() <= maxBytes)
            return;
        auto end = maxBytes;
        while (end > 0 && (static_cast<uint8_t>(text[end]) & 0xC0U) == 0x80U)
            --end;
        text.resize(end);
    }

    /// Sanitises @p untrusted line by line, keeping its line breaks, to at most MaxTranscriptEntryBytes.
    [[nodiscard]] std::string sanitizedText(std::string_view untrusted)
    {
        auto result = std::string {};
        auto first = true;
        for (auto const line: std::views::split(untrusted, '\n'))
        {
            if (!first)
                result += '\n';
            first = false;
            result += vthost::control::sanitizeStatusText(std::string_view { line }, MaxTranscriptEntryBytes);
            if (result.size() >= MaxTranscriptEntryBytes)
                break;
        }
        cutAtCodePoint(result, MaxTranscriptEntryBytes);
        return result;
    }

    /// @return A tool result's text: its string content, or its text blocks one per line.
    [[nodiscard]] std::string toolResultText(glz::generic const& block)
    {
        auto const* content = memberOf(block, "content");
        if (content == nullptr)
            return {};
        if (auto const* text = content->get_if<std::string>())
            return *text;
        auto result = std::string {};
        if (auto const* parts = content->get_if<Array>())
        {
            for (auto const& part: *parts)
            {
                if (textOf(memberOf(part, "type")) != "text"sv)
                    continue;
                if (!result.empty())
                    result += '\n';
                result += textOf(memberOf(part, "text"));
            }
        }
        return result;
    }

    /// @return A tool call's text: its name and its input as compact JSON.
    [[nodiscard]] std::string toolCallText(glz::generic const& block)
    {
        auto result = std::string { textOf(memberOf(block, "name")) };
        if (auto const* input = memberOf(block, "input"))
        {
            auto json = std::string {};
            if (!glz::write_json(*input, json))
                result += ' ' + json;
        }
        return result;
    }

    [[nodiscard]] std::string_view noteText(std::string_view block) noexcept
    {
        auto const row = std::ranges::find(NoteTexts, block, &NoteText::block);
        return row != NoteTexts.end() ? row->text : block;
    }
    // }}}

    /// One page being read: counts every shown entry, keeps only the requested page's.
    class PageReader
    {
      public:
        explicit PageReader(uint32_t page):
            _first { page > 0 ? (std::size_t { page } - 1) * TranscriptEntriesPerPage : 0 },
            _wanted { page > 0 }
        {
        }

        void read(LogLine const& line)
        {
            if (line.text.find_first_not_of(" \t\r") == std::string_view::npos)
                return;
            auto typed = TypeField {};
            if (glz::read<TypeReadOptions>(typed, line.text))
            {
                ++_notUnderstood;
                return;
            }
            if (auto const* row = rowFor(typed.type, ""sv))
            {
                countUnlessIgnored(row->treatment);
                return;
            }
            if (!isKnownType(typed.type) || line.extent == LineExtent::Cut)
            {
                ++_notUnderstood;
                return;
            }
            auto fields = LineFields {};
            if (glz::read<LineReadOptions>(fields, line.text))
            {
                ++_notUnderstood;
                return;
            }
            if (std::ranges::any_of(HiddenTurnFlags, [&](TurnFlag flag) { return has(fields, flag); }))
                return;
            if (fields.message && fields.message->content)
                readBlocks(fields, *fields.message->content);
            else
                readSubtype(fields);
        }

        [[nodiscard]] TranscriptPage finish(uint32_t page) &&
        {
            auto const shownCount = _shown;
            auto const pages = std::max<std::size_t>(
                1, (shownCount + TranscriptEntriesPerPage - 1) / TranscriptEntriesPerPage);
            return TranscriptPage {
                .entries = std::move(_entries),
                .page = page,
                .pages =
                    static_cast<uint32_t>(std::min<std::size_t>(pages, std::numeric_limits<uint32_t>::max())),
                .notUnderstood = _notUnderstood,
                .writtenBy = vthost::control::sanitizeStatusText(_writtenBy, MaxTranscriptFieldBytes),
            };
        }

      private:
        void countUnlessIgnored(Treatment treatment)
        {
            if (treatment != Treatment::Ignored)
                ++_notUnderstood;
        }

        void readBlocks(LineFields const& fields, glz::generic const& content)
        {
            if (auto const* text = content.get_if<std::string>())
            {
                readBlock(fields, "text"sv, [&] { return std::string { *text }; }, Kind::User);
                return;
            }
            auto const* blocks = content.get_if<Array>();
            if (blocks == nullptr)
            {
                ++_notUnderstood;
                return;
            }
            for (auto const& block: *blocks)
            {
                auto const blockType = textOf(memberOf(block, "type"));
                auto const* isError = memberOf(block, "is_error");
                auto const failed =
                    isError != nullptr && isError->get_if<bool>() != nullptr && *isError->get_if<bool>();
                readBlock(
                    fields,
                    blockType,
                    [&] {
                        if (blockType == "tool_use"sv)
                            return toolCallText(block);
                        if (blockType == "tool_result"sv)
                            return toolResultText(block);
                        return std::string { textOf(memberOf(block, "text")) };
                    },
                    failed ? Kind::ToolError : Kind::ToolResult);
            }
        }

        void readSubtype(LineFields const& fields)
        {
            auto const subtype = fields.subtype ? std::string_view { *fields.subtype } : std::string_view {};
            readBlock(fields, subtype, [&] { return std::string { noteText(subtype) }; }, Kind::ToolResult);
        }

        /// Counts, skips or shows one block. The text is made only for an entry on the requested page.
        /// @param resultKind The kind a tool result shows as: ToolError when it is marked as an error.
        void readBlock(LineFields const& fields,
                       std::string_view block,
                       auto const& makeText,
                       Kind resultKind)
        {
            auto const* row = rowFor(fields.type, block);
            if (row == nullptr || row->treatment != Treatment::Shown)
            {
                countUnlessIgnored(row != nullptr ? row->treatment : Treatment::NotUnderstood);
                return;
            }
            auto const index = _shown++;
            _writtenBy = fields.version.value_or(std::string {});
            if (!_wanted || index < _first || index >= _first + TranscriptEntriesPerPage)
                return;
            _entries.push_back(TranscriptEntry {
                .kind = row->kind == Kind::ToolResult ? resultKind : row->kind,
                .time = vthost::control::sanitizeStatusText(fields.timestamp.value_or(std::string {}),
                                                            MaxTranscriptFieldBytes),
                .text = sanitizedText(makeText()),
            });
        }

        std::size_t _first;
        bool _wanted;
        std::size_t _shown = 0;
        std::size_t _notUnderstood = 0;
        std::string _writtenBy;
        std::vector<TranscriptEntry> _entries;
    };

    [[nodiscard]] bool isInRange(TranscriptPage const& page) noexcept
    {
        return page.page >= 1 && page.page <= page.pages;
    }

    // {{{ printing

    /// @return The number the digits of @p text spell; std::nullopt when it holds anything else.
    [[nodiscard]] std::optional<int> numberOf(std::string_view text) noexcept
    {
        auto value = 0;
        auto const [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (error != std::errc {} || end != text.data() + text.size()
            || !std::ranges::all_of(text, [](char c) { return c >= '0' && c <= '9'; }))
            return std::nullopt;
        return value;
    }

    /// @param time An entry's timestamp, as Claude Code writes it: ISO 8601 in UTC, `2026-11-19T10:00:00Z`
    ///        with or without a fraction of a second.
    /// @return The instant it names, to the second; std::nullopt for any other text.
    [[nodiscard]] std::optional<std::chrono::sys_seconds> utcInstantOf(std::string_view time) noexcept
    {
        constexpr auto Shape = "dddd-dd-ddTdd:dd:dd"sv;
        if (time.size() <= Shape.size() || !time.ends_with('Z'))
            return std::nullopt;
        for (auto const index: std::views::iota(std::size_t { 0 }, Shape.size()))
            if (Shape[index] != 'd' && time[index] != Shape[index])
                return std::nullopt;
        // What follows the seconds is `Z`, or a fraction and then `Z`.
        if (auto const rest = time.substr(Shape.size(), time.size() - Shape.size() - 1);
            !rest.empty() && (rest.front() != '.' || !numberOf(rest.substr(1))))
            return std::nullopt;
        auto const year = numberOf(time.substr(0, 4));
        auto const month = numberOf(time.substr(5, 2));
        auto const day = numberOf(time.substr(8, 2));
        auto const hour = numberOf(time.substr(11, 2));
        auto const minute = numberOf(time.substr(14, 2));
        auto const second = numberOf(time.substr(17, 2));
        if (!year || !month || !day || !hour || !minute || !second || *hour > 23 || *minute > 59
            || *second > 60)
            return std::nullopt;
        auto const date = std::chrono::year { *year } / std::chrono::month { static_cast<unsigned>(*month) }
                          / std::chrono::day { static_cast<unsigned>(*day) };
        if (!date.ok())
            return std::nullopt;
        return std::chrono::sys_days { date } + std::chrono::hours { *hour }
               + std::chrono::minutes { *minute } + std::chrono::seconds { *second };
    }

    [[nodiscard]] std::string jsonPage(TranscriptPage const& page,
                                       std::string_view session,
                                       vtbackend::LocalTimeConverter localTime)
    {
        auto wire = WirePage { .format = TranscriptFormat,
                               .session = session,
                               .writtenBy = page.writtenBy,
                               .page = page.page,
                               .pages = page.pages,
                               .notUnderstood = page.notUnderstood,
                               .entries = {} };
        wire.entries.reserve(page.entries.size());
        for (auto const& entry: page.entries)
            // The offset the reader's time zone had at that instant, from this host's rules: an entry
            // written before a daylight-saving change shows the time it was then.
            wire.entries.push_back(WireEntry {
                .kind = nameOf(entry.kind),
                .time = entry.time,
                .offset = utcInstantOf(entry.time).transform([&](std::chrono::sys_seconds instant) {
                    return static_cast<int32_t>(vtbackend::utcOffsetAt(localTime, instant).count());
                }),
                .text = entry.text });
        auto json = std::string {};
        if (glz::write_json(wire, json))
            return {};
        return json + '\n';
    }

    /// @return The clock time of an ISO 8601 timestamp (`HH:MM:SS`), or the timestamp as it is.
    [[nodiscard]] std::string_view clockTime(std::string_view time) noexcept
    {
        constexpr auto ClockOffset = std::size_t { 11 };
        constexpr auto ClockLength = std::size_t { 8 };
        if (time.size() >= ClockOffset + ClockLength && time[ClockOffset - 1] == 'T')
            return time.substr(ClockOffset, ClockLength);
        return time;
    }

    /// What every line of an entry's text starts with in text output.
    constexpr auto TextIndent = "  "sv;

    [[nodiscard]] std::string textPage(TranscriptPage const& page, std::string_view session)
    {
        auto text = std::format("session {} · page {} of {}", session, page.page, page.pages);
        if (!page.writtenBy.empty())
            text += std::format(" · Claude Code {}", page.writtenBy);
        text += '\n';
        for (auto const& entry: page.entries)
        {
            // Labelled: a person reading it elsewhere does not mistake it for their own time.
            text += std::format("── {} {}{}\n",
                                nameOf(entry.kind),
                                clockTime(entry.time),
                                utcInstantOf(entry.time) ? " UTC"sv : ""sv);
            // Every line of the text is indented, so that no text can pass for an entry's header or the
            // closing count.
            for (auto const line: std::views::split(std::string_view { entry.text }, '\n'))
                text += std::format("{}{}\n", line.empty() ? ""sv : TextIndent, std::string_view { line });
        }
        if (page.notUnderstood > 0)
            text += std::format(
                "{} {} not understood\n", page.notUnderstood, page.notUnderstood == 1 ? "entry" : "entries");
        return text;
    }
    // }}}

    /// @return The page as text or as format-1 JSON, ending in a newline.
    [[nodiscard]] std::string formatPage(TranscriptPage const& page,
                                         TranscriptRequest const& request,
                                         vtbackend::LocalTimeConverter localTime)
    {
        switch (request.output)
        {
            case TranscriptOutput::Text: return textPage(page, request.session);
            case TranscriptOutput::Json: return jsonPage(page, request.session, localTime);
        }
        return {};
    }

} // namespace

std::span<EntryRow const> transcriptEntryRows() noexcept
{
    return EntryRows;
}

int runAgentTranscript(TranscriptRequest const& request,
                       core::Environment const& env,
                       core::platform::FileSystem const& fs,
                       ISessionLogOpener& opener,
                       vtbackend::LocalTimeConverter localTime,
                       std::ostream& out,
                       std::ostream& err)
{
    if (request.format != TranscriptFormat)
        return detail::refuse(err,
                              TranscriptError::UnsupportedFormat,
                              std::format("unsupported transcript format {}; this contour prints {}",
                                          request.format,
                                          TranscriptFormat));
    // One open and one pass: the read that binds the log to the run is the one that keeps its page.
    auto pages = std::vector<TranscriptPage> {};
    auto const scan = [&](std::istream& log, std::filesystem::path const& root) {
        auto reader = PageReader { request.page };
        auto binding = detail::scanBound(log, root, [&](LogLine const& line) { reader.read(line); });
        pages.push_back(std::move(reader).finish(request.page));
        return binding;
    };
    auto const query = detail::SessionLogQuery { .session = request.session, .cwd = request.cwd };
    auto const located = detail::locateSessionLog(query, env, fs, opener, scan);
    if (!located)
        return detail::refuse(err, located.error().error, detail::lookupMessage(located.error(), query));
    auto const& page = pages[located->candidate];
    if (!isInRange(page))
        return detail::refuse(err,
                              TranscriptError::PageOutOfRange,
                              std::format("page {} of {} does not exist", request.page, page.pages));
    out << formatPage(page, request, localTime);
    return exitCode(ExitStatus::Ok);
}

} // namespace contour::cli
