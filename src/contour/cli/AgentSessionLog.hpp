// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// A Claude Code session's log, as `contour agent transcript` and `contour agent stats` read it: found by its
/// session UUID under Claude Code's projects directory, opened without following a link or blocking on a
/// FIFO, bound to the run by the directories its entries were written in, and read one bounded line at a
/// time. What a verb does with each line is its own; finding, binding and reading the log are shared here.

#include <core/Environment.hpp>
#include <core/platform/FileSystem.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <istream>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace contour::cli
{

/// The most bytes of one log line held in memory; the rest of a longer line is discarded unread.
inline constexpr std::size_t MaxLogLineBytes = std::size_t { 1024 } * 1024;

/// The most bytes of a path or a directory a refusal names.
inline constexpr std::size_t MaxLogMessageFieldBytes = 2048;

/// The variable naming the home Claude Code keeps `.claude` in, as Node's `os.homedir()` reads it:
/// `%USERPROFILE%` on Windows, where `$HOME` (as Git Bash or MSYS2 set it) is not looked at, `$HOME`
/// elsewhere.
#ifdef _WIN32
inline constexpr std::string_view ClaudeHomeVariable = "USERPROFILE";
#else
inline constexpr std::string_view ClaudeHomeVariable = "HOME";
#endif

/// Why a session log could not be read, or what was asked of it could not be answered: the refusals of both
/// verbs that read one.
enum class TranscriptError : uint8_t
{
    NoConfigDirectory = 0, ///< Neither $CLAUDE_CONFIG_DIR nor ClaudeHomeVariable is set.
    BadSession,            ///< The session is not a canonical (lower-case) UUID.
    NoLog,                 ///< No log of that session was written in the run's directory.
    AmbiguousLog,          ///< Two logs of that session were.
    NotRegular,            ///< The log is a link, a FIFO, a directory or a device.
    Unreadable,            ///< The log could not be opened or read.
    PageOutOfRange,        ///< Page 0, or a page past the last.
    UnsupportedFormat,     ///< An output format this contour does not print.
};

/// Opens a session log without following a link and without blocking; a regular file only. Also tells a
/// projects directory that is a link (a symbolic link, or on Windows a junction) from a real one.
class ISessionLogOpener
{
  public:
    ISessionLogOpener() = default;
    virtual ~ISessionLogOpener() = default;
    ISessionLogOpener(ISessionLogOpener const&) = delete;
    ISessionLogOpener& operator=(ISessionLogOpener const&) = delete;
    ISessionLogOpener(ISessionLogOpener&&) = delete;
    ISessionLogOpener& operator=(ISessionLogOpener&&) = delete;

    /// @param log The log's path.
    /// @return A stream of its bytes; NotRegular for a link or anything but a regular file, Unreadable when
    ///         it cannot be opened.
    [[nodiscard]] virtual std::expected<std::unique_ptr<std::istream>, TranscriptError> open(
        std::filesystem::path const& log) = 0;

    /// @param directory A directory in Claude Code's projects directory.
    /// @return Whether it is a link to elsewhere -- a symbolic link, or a junction or other reparse point on
    ///         Windows -- whose logs are not looked at.
    [[nodiscard]] virtual bool isLinkedDirectory(std::filesystem::path const& directory) = 0;
};

/// @return The opener of the real file system (`O_NOFOLLOW|O_NONBLOCK` + `fstat`; on Windows a reparse point
///         is opened as itself and must be a disk file).
[[nodiscard]] std::unique_ptr<ISessionLogOpener> makeNativeSessionLogOpener();

/// @param session A session id as given.
/// @return Whether it is a canonical UUID: 8-4-4-4-12 lower-case hexadecimal digits.
[[nodiscard]] bool isCanonicalSessionId(std::string_view session) noexcept;

namespace detail
{
    /// Whether a line was held whole or cut at MaxLogLineBytes.
    enum class LineExtent : uint8_t
    {
        Whole = 0,
        Cut,
    };

    /// One line of a log, as held.
    struct LogLine
    {
        std::string_view text;                 ///< What was kept of it, without its newline.
        LineExtent extent = LineExtent::Whole; ///< Whether that is all of it.
    };

    /// Reads a log one newline-terminated line at a time, holding at most MaxLogLineBytes of a line; the rest
    /// of a longer one is read through and discarded. A last line without its newline is never returned: it
    /// is still being written.
    class LineReader
    {
      public:
        /// @param in The log; it must outlive the reader.
        explicit LineReader(std::istream& in);

        /// @return The next whole line; nothing at the end, or before a last line still being written.
        [[nodiscard]] std::optional<LogLine> next();

        /// @return Whether reading stopped on an error rather than at the end.
        [[nodiscard]] bool failed() const { return _in.bad(); }

      private:
        [[nodiscard]] bool fill();

        std::istream& _in;
        std::vector<char> _chunk;
        std::size_t _begin = 0;
        std::size_t _end = 0;
        std::string _line;
    };

    /// Whether a log belongs to the run.
    enum class Binding : uint8_t
    {
        NoDirectory = 0, ///< No entry carries a `cwd`: nothing ties the log to the run.
        Beneath,         ///< Every `cwd` lies at or beneath the run's directory, and one entry carries one.
        Outside,         ///< An entry was written outside it; the read stopped there.
    };

    /// What a verb does with each line of a log it reads.
    using LineSink = std::function<void(LogLine const&)>;

    /// Reads @p log in one pass, checking every entry's `cwd` against @p root as it goes and handing each
    /// line to @p sink, up to the first entry written outside @p root.
    /// @return How the log is bound to the run; Unreadable when reading failed.
    [[nodiscard]] std::expected<Binding, TranscriptError> scanBound(std::istream& log,
                                                                    std::filesystem::path const& root,
                                                                    LineSink const& sink);

    /// Reads one candidate log, with state of its own: scanBound() with a fresh sink.
    using CandidateScan =
        std::function<std::expected<Binding, TranscriptError>(std::istream&, std::filesystem::path const&)>;

    /// What is looked for: a session's log, written in a run's directory.
    struct SessionLogQuery
    {
        std::string_view session; ///< The session UUID.
        std::string_view cwd;     ///< The run's directory; every entry's must lie beneath it.
    };

    /// The session's one log.
    struct LocatedLog
    {
        std::filesystem::path log; ///< Its path.
        std::size_t candidate = 0; ///< Which call of the CandidateScan read it, 0-based.
    };

    /// Why no log was found, with the paths a message names.
    struct LookupFailure
    {
        TranscriptError error = TranscriptError::NoLog;
        std::filesystem::path directory; ///< The projects directory searched.
        std::vector<std::filesystem::path> logs;
    };

    /// Finds the session's log, `<config>/projects/*/<session>.jsonl` whose entries carry a `cwd` and were
    /// all written at or beneath the query's directory: opens each candidate once and has @p scan read it.
    /// @return The one bound log, and which scan read it; else why none, or more than one, was found.
    [[nodiscard]] std::expected<LocatedLog, LookupFailure> locateSessionLog(
        SessionLogQuery query,
        core::Environment const& env,
        core::platform::FileSystem const& fs,
        ISessionLogOpener& opener,
        CandidateScan const& scan);

    /// @return The one line a lookup failure prints.
    [[nodiscard]] std::string lookupMessage(LookupFailure const& failure, SessionLogQuery query);

    /// @return @p path as a message names it: with the platform's own separator throughout, sanitised.
    [[nodiscard]] std::string printable(std::filesystem::path path);

    /// Prints @p message, a refusal, on @p err.
    /// @return The exit code @p error refuses with.
    [[nodiscard]] int refuse(std::ostream& err, TranscriptError error, std::string_view message);
} // namespace detail

} // namespace contour::cli
