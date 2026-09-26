// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Where a program's log output goes, and how a log line is shaped.
///
/// `<core/log/LogStore.hpp>` is the mechanism — categories, sinks, formatters. This is the POLICY a
/// composition root applies on top of it: open a destination, apply a filter, install the
/// formatters, and put everything back on the way out.

#include <core/log/LogStore.hpp>

#include <expected>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace core::log
{

/// How a log line is laid out.
struct FormatterOptions
{
    /// Wraps the tag and the text in SGR colour. Never enable this for a file sink.
    bool colorize = false;
    /// Prefixes the line with `[YYYY-MM-DD HH:MM:SS.uuuuuu]`.
    bool showTimestamp = true;
    /// Prefixes the line with `[PID]`. Worth enabling whenever several processes share one
    /// terminal or one log file, as a daemon spawned by its client does when it inherits the
    /// client's stderr.
    bool showProcessId = false;
};

/// Where a program sends its log output, and what it logs.
struct OutputConfig
{
    /// The category filter ("all", or a comma-separated list of category names each with an
    /// optional trailing `*`). EMPTY leaves the current enablement alone — which is both how
    /// a filter already applied from $LOG survives, and a hard requirement: configure("")
    /// disables EVERY category, `error` included, because splitting an empty string yields no
    /// patterns at all for its any_of to match.
    std::string filter {};
    /// The file to append log lines to; nullopt (the default) writes to standard error.
    std::optional<std::filesystem::path> file {};
    /// Prefixes every line with the emitting process id.
    bool showProcessId = false;
};

/// Maps a `--log-file` option value onto a destination.
/// @param spec The raw value; both "" and "-" mean standard error.
/// @return The file to append to, or nullopt for standard error.
[[nodiscard]] std::optional<std::filesystem::path> parseLogFileSpec(std::string_view spec);

/// Builds the standard log line formatter:
/// `[timestamp] [pid] [category] text`, with the continuation lines of a multi-line message
/// indented under the text column and carrying no repeated tag.
/// @param options How the line is laid out.
/// @return The formatter, for core::log::setFormatter().
[[nodiscard]] Category::Formatter makeStandardFormatter(FormatterOptions options);

/// As makeStandardFormatter(), but tagged `[error]` and coloured red — for the errorLog
/// category, whose lines a reader must be able to pick out of a busy log at a glance.
/// @param options How the line is laid out.
/// @return The formatter, for Category::setFormatter().
[[nodiscard]] Category::Formatter makeErrorFormatter(FormatterOptions options);

/// Names the filter patterns in @p filterString that match no registered category, so a typo
/// ("--log vthost.session") can be reported instead of silently logging nothing.
/// @param filterString A category filter string, as configure() takes it.
/// @return The unmatched patterns, in the order given; empty for "all" or an empty filter.
[[nodiscard]] std::vector<std::string> unmatchedFilters(std::string_view filterString);

/// @return Whether this process's standard output is a terminal.
///
/// The colourisation gate for anything written there -- core::log::Sink::console(), and a help
/// text. It is asked here, in one place, rather than spelled out at each call site: the Windows
/// half of the branch that used to do the asking answered `true` unconditionally, so a redirected
/// stream received SGR escapes.
[[nodiscard]] bool isStdOutTerminal() noexcept;

/// @return Whether this process's standard error is a terminal.
///
/// The gate for a destination that writes there, which can be redirected independently of
/// standard output.
[[nodiscard]] bool isStdErrTerminal() noexcept;

/// Installs a process-wide log destination for its own lifetime, restoring the console
/// defaults on destruction.
///
/// The restore is load-bearing, not tidiness: core::log::Category holds its sink as a
/// std::reference_wrapper with NO lifetime management, so every category must be pointed back
/// at a static sink before this object's sink dies.
///
/// Thread safety: the writer serialises the whole write-and-flush under a mutex. Sink does
/// NOT do this — Sink::write() calls its writer unguarded, and the `<< text` / flush() pair
/// can interleave — while a server may log from its event-loop thread, a signal thread and
/// worker threads at once. Emitting from any thread is therefore safe here.
///
/// Lifetime precondition: CONSTRUCT AND DESTROY THIS WHILE NO OTHER THREAD LOGS. Installing
/// assigns each category's sink and formatter, which would race with a concurrent emit. A
/// program satisfies this structurally by creating the object before it spawns any thread and
/// destroying it after every thread is joined.
class ScopedOutput
{
  public:
    /// Opens the configured destination (creating parent directories, appending to an existing
    /// file), applies the filter, and points every registered category at the new sink.
    /// @param config Where and what to log.
    /// @return The installed output, or a human-readable reason the file could not be opened.
    [[nodiscard]] static std::expected<std::unique_ptr<ScopedOutput>, std::string> create(
        OutputConfig config);

    /// Prefer create(), which owns the one fallible step.
    /// @param config Where and what to log.
    /// @param file An already-opened destination; a closed stream means standard error.
    ScopedOutput(OutputConfig const& config, std::ofstream file);
    ~ScopedOutput();

    // Categories hold a reference into _sink and the writer captures `this`.
    ScopedOutput(ScopedOutput const&) = delete;
    ScopedOutput& operator=(ScopedOutput const&) = delete;
    ScopedOutput(ScopedOutput&&) = delete;
    ScopedOutput& operator=(ScopedOutput&&) = delete;

  private:
    /// One category's pre-install state, so the destructor restores exactly what was there
    /// rather than guessing at a default.
    struct RestorePoint
    {
        Category* target;
        Sink* previousSink;
        Category::Formatter previousFormatter;
    };

    std::mutex _mutex;
    std::ofstream _file;
    Sink _sink;
    std::vector<RestorePoint> _restorePoints;
};

/// Redirects one category — or every category — into an in-memory buffer for this object's
/// lifetime, restoring the console sink on destruction.
///
/// The restore runs from the destructor rather than the end of a test body because a failing
/// assertion unwinds: leaving a category pointing at a destroyed local sink would corrupt
/// every later test in the binary.
///
/// Thread safety: emitting into a captured category is safe from any thread, and so are
/// @c snapshot, @c contains, @c count and @c lines, which read under the same mutex every append
/// takes. @c text is the one exception: it hands out a reference to the buffer itself, so it is
/// valid only while no thread logs into the capture -- after the writers are joined, say. Read
/// with @c snapshot while they run. Construction and destruction reassign each category's sink,
/// so, like @c ScopedOutput, construct and destroy this while no other thread logs.
class ScopedCapture
{
  public:
    /// @param categoryName The category to capture; empty captures every registered category.
    explicit ScopedCapture(std::string_view categoryName = {});
    ~ScopedCapture();

    ScopedCapture(ScopedCapture const&) = delete;
    ScopedCapture& operator=(ScopedCapture const&) = delete;
    ScopedCapture(ScopedCapture&&) = delete;
    ScopedCapture& operator=(ScopedCapture&&) = delete;

    /// @return Everything written to the captured categories so far, verbatim.
    ///
    /// A reference to the buffer itself, read without the lock: call it only while no thread
    /// logs into the capture. While one may, use @c snapshot.
    [[nodiscard]] std::string const& text() const noexcept { return _text; }

    /// @return A copy of everything written to the captured categories so far, taken under the
    ///         lock, so it is safe while other threads log into the capture.
    [[nodiscard]] std::string snapshot() const;

    /// @param needle The substring to look for.
    /// @return Whether any captured output contains @p needle.
    [[nodiscard]] bool contains(std::string_view needle) const;

    /// @param needle The substring to count.
    /// @return How many captured LINES contain @p needle.
    [[nodiscard]] std::size_t count(std::string_view needle) const;

    /// @return The captured output split into lines, without their terminators.
    [[nodiscard]] std::vector<std::string> lines() const;

  private:
    /// One captured category's pre-capture state, so the destructor can put it back.
    struct RestorePoint
    {
        Category* target;
        Sink* previousSink;
        bool wasEnabled;
    };

    mutable std::mutex _mutex; ///< Guards @c _text against a writer on another thread.
    std::string _text;
    Sink _sink;
    std::vector<RestorePoint> _restorePoints;
};

} // namespace core::log
