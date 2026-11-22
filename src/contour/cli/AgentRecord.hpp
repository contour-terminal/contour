// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `contour agent record`: appends one finished agent run, as the agent orchestrator describes it, to the
/// file of finished runs a user analyses afterwards (`agent-runs.jsonl` in the daemon's state directory). The
/// orchestrator cannot write a file itself; the line is its own (format 1,
/// `extensions/orchestrator/contract/agent-runs/`), and the verb only checks that it is one JSON object of
/// that format and writes it compactly, so that it is one line whatever it held.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <ostream>
#include <string>
#include <string_view>

namespace contour::cli
{

/// The format of the line this contour writes; another one is refused.
inline constexpr uint32_t RecordFormat = 1;

/// The file, in the daemon's state directory, finished runs are appended to.
inline constexpr std::string_view AgentRunsFileName = "agent-runs.jsonl";

/// Why a line could not be appended.
enum class RecordError : uint8_t
{
    NotARecord,        ///< The line is no JSON object.
    UnsupportedFormat, ///< Its `format` is not RecordFormat.
    Unwritable,        ///< The file could not be created or written.
};

/// Appends bytes to a file, creating it (and its directory) private to the user, never through a link.
class IRecordAppender
{
  public:
    IRecordAppender() = default;
    virtual ~IRecordAppender() = default;
    IRecordAppender(IRecordAppender const&) = delete;
    IRecordAppender& operator=(IRecordAppender const&) = delete;
    IRecordAppender(IRecordAppender&&) = delete;
    IRecordAppender& operator=(IRecordAppender&&) = delete;

    /// Appends @p bytes to @p file in one write.
    /// @return Nothing; else why, in words.
    [[nodiscard]] virtual std::expected<void, std::string> append(std::filesystem::path const& file,
                                                                  std::string_view bytes) = 0;
};

/// @return The appender of the real file system (POSIX `O_APPEND|O_NOFOLLOW`, mode 0600; Windows
///         `FILE_APPEND_DATA` on the file itself, never a reparse point's target).
[[nodiscard]] std::unique_ptr<IRecordAppender> makeNativeRecordAppender();

/// What `contour agent record` was asked.
struct RecordRequest
{
    std::string line;               ///< The run, as one JSON object.
    uint32_t format = RecordFormat; ///< The format the caller wrote.
};

/// Runs `contour agent record`: checks @p request's line and appends it to @p file.
/// @param request What was asked.
/// @param file Where finished runs are kept: AgentRunsFileName in the daemon's state directory.
/// @param appender What appends to it.
/// @param err Where the one line of a refusal is printed.
/// @return The ExitStatus as an exit code.
[[nodiscard]] int runAgentRecord(RecordRequest const& request,
                                 std::filesystem::path const& file,
                                 IRecordAppender& appender,
                                 std::ostream& err);

} // namespace contour::cli
