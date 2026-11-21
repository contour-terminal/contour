// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `contour agent transcript`: reads one page of a Claude Code session's log -- the agent's turns, its tool
/// calls and their results -- and prints it sanitised, as text or as the versioned JSON the agent
/// orchestrator reads (format 1, `extensions/orchestrator/contract/agent-transcript/`).
///
/// The log is untrusted and may still be growing: it is found by its session UUID, opened without following a
/// link or blocking on a FIFO, bound to the run by the directories its entries were written in, and read one
/// bounded line at a time. A table, transcriptEntryRows(), decides what each line is; a line no row names is
/// counted, never fatal, so that a log from a newer Claude Code degrades to a count.

#include <contour/cli/AgentSessionLog.hpp>

#include <vtbackend/screen/LocalTime.hpp>

#include <core/Environment.hpp>
#include <core/platform/FileSystem.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <istream>
#include <memory>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace contour::cli
{

/// How many shown entries a page holds. Pages count shown entries only, so a page before the last never
/// changes while the log grows.
inline constexpr std::size_t TranscriptEntriesPerPage = 30;

/// The most bytes of text an entry keeps (≈ 124 KiB a page worst case, after JSON escaping).
inline constexpr std::size_t MaxTranscriptEntryBytes = 2048;

/// The most bytes of a time or a version kept.
inline constexpr std::size_t MaxTranscriptFieldBytes = 40;

/// The output format this contour prints; another one asked for is refused.
inline constexpr uint32_t TranscriptFormat = 1;

/// What a shown entry is.
enum class TranscriptEntryKind : uint8_t
{
    User = 0,   ///< The user's prompt.
    Assistant,  ///< The agent's text.
    ToolCall,   ///< A tool the agent called: its name and input.
    ToolResult, ///< What a tool returned.
    ToolError,  ///< A tool result marked as an error.
    Note,       ///< Something that happened to the conversation, such as a compaction.
};

/// What becomes of a log line, or of one block of its message.
enum class EntryTreatment : uint8_t
{
    Ignored = 0,   ///< Known and not shown: metadata, thinking, attachments.
    Shown,         ///< An entry of the transcript.
    NotUnderstood, ///< Counted and skipped; a type no row names is treated so too.
};

/// One row of the table that decides what a log line is.
struct EntryRow
{
    std::string_view type;  ///< The line's `type`.
    std::string_view block; ///< The message block's type, or the `system` subtype; "" = the entry as a whole.
    EntryTreatment treatment; ///< What becomes of it.
    TranscriptEntryKind kind; ///< What it shows as, when Shown.
};

/// @return The table of known log lines and message blocks; a pair no row names is not understood.
[[nodiscard]] std::span<EntryRow const> transcriptEntryRows() noexcept;

/// One shown entry, sanitised.
struct TranscriptEntry
{
    TranscriptEntryKind kind = TranscriptEntryKind::User; ///< What it is.
    std::string time;                                     ///< Its ISO 8601 timestamp.
    std::string text;                                     ///< Its text, at most MaxTranscriptEntryBytes.
};

/// One page of a transcript.
struct TranscriptPage
{
    std::vector<TranscriptEntry> entries; ///< The page's entries, at most TranscriptEntriesPerPage.
    uint32_t page = 1;                    ///< Its 1-based number.
    uint32_t pages = 1;                   ///< How many pages the whole log has; at least 1.
    std::size_t notUnderstood = 0; ///< Lines and blocks of the whole log that were counted and skipped.
    std::string writtenBy;         ///< The Claude Code version of the log's last shown entry.
};

/// How a page is printed.
enum class TranscriptOutput : uint8_t
{
    Text = 0, ///< For a person.
    Json,     ///< Format TranscriptFormat, for the orchestrator.
};

/// What `contour agent transcript` was asked.
struct TranscriptRequest
{
    std::string session;                ///< The session UUID.
    std::string cwd;                    ///< The run's directory; every entry's must lie beneath it.
    uint32_t page = 1;                  ///< The 1-based page.
    uint32_t format = TranscriptFormat; ///< The output format asked for.
    TranscriptOutput output = TranscriptOutput::Text; ///< How to print it.
};

/// Runs `contour agent transcript`: finds the session's log, `<config>/projects/*/<session>.jsonl` whose
/// entries carry a `cwd` and were all written at or beneath the request's directory, reads its page in the
/// same pass, and prints it. Text labels each entry's time UTC; format-1 JSON carries, beside each, the
/// offset from UTC that @p localTime had at that instant.
/// @param request What was asked.
/// @param env Where `$CLAUDE_CONFIG_DIR` and ClaudeHomeVariable are read.
/// @param fs Where the projects directory is listed.
/// @param opener What opens each candidate log.
/// @param localTime The time zone, with its rules, the entries' offsets are read in
/// (vtbackend::systemLocalTime).
/// @param out Where the page is printed.
/// @param err Where the one line of a refusal is printed.
/// @return The ExitStatus as an exit code.
[[nodiscard]] int runAgentTranscript(TranscriptRequest const& request,
                                     core::Environment const& env,
                                     core::platform::FileSystem const& fs,
                                     ISessionLogOpener& opener,
                                     vtbackend::LocalTimeConverter localTime,
                                     std::ostream& out,
                                     std::ostream& err);

} // namespace contour::cli
