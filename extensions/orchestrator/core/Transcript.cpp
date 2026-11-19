// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/core/Transcript.hpp>

#include <algorithm>
#include <array>
#include <optional>
#include <string_view>
#include <utility>

#include <glaze/glaze.hpp>
#include <orchestrator/core/Json.hpp>
#include <orchestrator/core/Naming.hpp>
#include <orchestrator/core/TranscriptJson.hpp>

using namespace std::string_view_literals;

namespace orchestrator
{

namespace
{
    using transcript::detail::JsonFormat;
    using transcript::detail::JsonPage;

    /// The input is a string_view: nothing terminates it, and a newer `contour` may add keys.
    constexpr auto ReadOptions = glz::opts { .null_terminated = false, .error_on_unknown_keys = false };

    /// The kinds as format 1 spells them.
    constexpr auto KindNames = std::array {
        std::pair { "user"sv, TranscriptEntryKind::User },
        std::pair { "assistant"sv, TranscriptEntryKind::Assistant },
        std::pair { "tool-call"sv, TranscriptEntryKind::ToolCall },
        std::pair { "tool-result"sv, TranscriptEntryKind::ToolResult },
        std::pair { "tool-error"sv, TranscriptEntryKind::ToolError },
        std::pair { "note"sv, TranscriptEntryKind::Note },
    };

    [[nodiscard]] std::optional<TranscriptEntryKind> kindNamed(std::string_view name) noexcept
    {
        auto const row =
            std::ranges::find(KindNames, name, &std::pair<std::string_view, TranscriptEntryKind>::first);
        if (row == KindNames.end())
            return std::nullopt;
        return row->second;
    }

    [[nodiscard]] TranscriptReadError readErrorOf(ParseError error) noexcept
    {
        switch (error)
        {
            case ParseError::NotJson: return TranscriptReadError::NotJson;
            case ParseError::WrongShape: return TranscriptReadError::WrongShape;
        }
        return TranscriptReadError::WrongShape;
    }
} // namespace

std::expected<TranscriptPage, TranscriptReadError> parseTranscriptPage(std::string_view json)
{
    // The format first: a page of another format may have another shape, which is a mismatch, not a fault.
    auto const format = json::readDocument<ReadOptions, JsonFormat>(json);
    if (!format)
        return std::unexpected(readErrorOf(format.error()));
    if (format->format != TranscriptFormat)
        return std::unexpected(TranscriptReadError::FormatMismatch);
    auto read = json::readDocument<ReadOptions, JsonPage>(json);
    if (!read)
        return std::unexpected(readErrorOf(read.error()));
    auto page = TranscriptPage { .page = read->page,
                                 .pages = read->pages,
                                 .writtenBy = std::move(read->writtenBy),
                                 .notUnderstood = read->notUnderstood,
                                 .entries = {} };
    page.entries.reserve(read->entries.size());
    for (auto& entry: read->entries)
    {
        auto const kind = kindNamed(entry.kind);
        if (!kind)
        {
            ++page.notUnderstood;
            continue;
        }
        // An offset no time zone has is no offset: the entry's time is then shown in UTC.
        auto const offset = entry.offset.and_then([](int32_t minutes) -> std::optional<int32_t> {
            if (minutes < -MaxUtcOffsetMinutes || minutes > MaxUtcOffsetMinutes)
                return std::nullopt;
            return minutes;
        });
        page.entries.push_back(TranscriptEntry { .kind = *kind,
                                                 .time = std::move(entry.time),
                                                 .utcOffsetMinutes = offset,
                                                 .text = std::move(entry.text) });
    }
    return page;
}

TranscriptReadError classifyTranscriptFailure(std::string_view stderrText) noexcept
{
    return stderrText.starts_with(UnsupportedFormatPrefix) ? TranscriptReadError::FormatMismatch
                                                           : TranscriptReadError::NotJson;
}

} // namespace orchestrator
