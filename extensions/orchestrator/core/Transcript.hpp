// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/// One page of an agent's transcript, as `contour agent transcript --json` prints it in format 1
/// (contract/agent-transcript/). The JSON reader stays in Transcript.cpp.
namespace orchestrator
{

/// The format the orchestrator asks for and reads; a page in another one is a FormatMismatch.
inline constexpr uint32_t TranscriptFormat = 1;

/// What a transcript entry is.
enum class TranscriptEntryKind : uint8_t
{
    User = 0,   ///< The user's prompt.
    Assistant,  ///< The agent's text.
    ToolCall,   ///< A tool the agent called.
    ToolResult, ///< What a tool returned.
    ToolError,  ///< A tool result marked as an error.
    Note,       ///< Something that happened to the conversation, such as a compaction.
};

/// One entry of a page, sanitised by the host.
struct TranscriptEntry
{
    TranscriptEntryKind kind = TranscriptEntryKind::User; ///< What it is.
    std::string time;                                     ///< Its ISO 8601 timestamp.
    std::optional<int32_t> utcOffsetMinutes; ///< How far the host's local time was ahead of UTC at @ref time,
                                             ///< by its time zone's rules; std::nullopt: not known.
    std::string text;                        ///< Its text, lines joined by `\n`.
    [[nodiscard]] bool operator==(TranscriptEntry const&) const = default;
};

/// One page of a transcript.
struct TranscriptPage
{
    uint32_t page = 0;          ///< Its 1-based number.
    uint32_t pages = 0;         ///< How many pages the log has.
    std::string writtenBy;      ///< The Claude Code version that wrote the log's last entry.
    uint32_t notUnderstood = 0; ///< The log's lines the host skipped, and the entries of a kind this
                                ///< reader does not know.
    std::vector<TranscriptEntry> entries; ///< The page's entries, in log order.
    [[nodiscard]] bool operator==(TranscriptPage const&) const = default;
};

/// Why a page could not be read.
enum class TranscriptReadError : uint8_t
{
    NotJson = 0,    ///< Not one JSON document, or the verb failed without printing one.
    WrongShape,     ///< JSON, but not a page.
    FormatMismatch, ///< A page in another format, or the verb refused the format asked for.
};

/// The start of the one line `contour agent transcript` prints when it refuses the format asked for:
/// `unsupported transcript format <n>; this contour prints <m>`.
inline constexpr std::string_view UnsupportedFormatPrefix = "unsupported transcript format ";

/// Reads a page the verb printed.
/// @param json Its standard output.
/// @return The page; an entry of an unknown kind is counted in TranscriptPage::notUnderstood and left out.
[[nodiscard]] std::expected<TranscriptPage, TranscriptReadError> parseTranscriptPage(std::string_view json);

/// Tells the verb's refusal of the format from any other failure.
/// @param stderrText What the failed verb printed on standard error.
/// @return FormatMismatch for the format refusal; NotJson for any other failure, which printed no page.
[[nodiscard]] TranscriptReadError classifyTranscriptFailure(std::string_view stderrText) noexcept;

} // namespace orchestrator
