// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `contour daemon status | logs | stop`: the verbs that talk to a running daemon instead of becoming one.
/// Each takes its collaborators (the control port, the prompt, the file system, the output streams) so the
/// decisions are testable without a daemon, a terminal or a clock.

#include <contour/cli/ExitStatus.hpp>

#include <core/platform/FileSystem.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <memory>
#include <string>
#include <string_view>

#include <vthost/client/DaemonControlClient.hpp>

namespace contour::cli
{

/// What `contour daemon` was asked to do; Run is the daemon itself, the rest talk to one.
enum class DaemonVerb : uint8_t
{
    Run = 0, ///< Become the daemon.
    Status,  ///< Report on the running daemon.
    Logs,    ///< Print the running daemon's log.
    Stop,    ///< Stop the running daemon.
};

/// One row per `contour daemon` sub-verb: the command list, the dispatch and the help read this table.
struct DaemonVerbRow
{
    std::string_view name;     ///< The CLI name.
    std::string_view helpText; ///< Its help text.
    DaemonVerb verb;           ///< What it does.
};

/// The sub-verbs of `contour daemon`.
inline constexpr auto DaemonSubVerbs = std::array {
    DaemonVerbRow { "status",
                    "Reports the running daemon's version, uptime, socket, sessions and clients.",
                    DaemonVerb::Status },
    DaemonVerbRow {
        "logs", "Prints the daemon's log; --follow keeps printing as it grows.", DaemonVerb::Logs },
    DaemonVerbRow { "stop",
                    "Stops the daemon, ending every session it hosts; asks first when sessions exist.",
                    DaemonVerb::Stop },
};

/// Whether `daemon stop` may end sessions without asking (`--yes`).
enum class StopConsent : uint8_t
{
    Ask = 0, ///< Ask first when sessions exist.
    Given,   ///< Already given on the command line.
};

/// Whether `daemon logs` ends after printing (`--follow`).
enum class FollowMode : uint8_t
{
    Once = 0, ///< Print the tail and end.
    Follow,   ///< Keep printing as the log grows.
};

/// What a follow loop does after waiting one tick.
enum class FollowStep : uint8_t
{
    Stop = 0, ///< End the loop.
    Continue, ///< Poll again.
};

/// The answer to a yes/no question put to the user.
enum class Confirmation : uint8_t
{
    Declined = 0, ///< The user said no, or anything but yes.
    Confirmed,    ///< The user said yes.
    Unavailable,  ///< There is nobody to ask.
};

/// Asks the user a yes/no question.
class ConfirmPrompt
{
  public:
    virtual ~ConfirmPrompt() = default;

    /// @param question The full question, including its `[y/N]` suffix.
    /// @return The answer.
    [[nodiscard]] virtual Confirmation ask(std::string_view question) = 0;
};

/// @param in Where answers are read from; must be standard input for the terminal check to hold.
/// @param out Where the question is written.
/// @return A prompt answering Unavailable when standard input is not a terminal; `y` and `yes` confirm,
///         anything else declines.
[[nodiscard]] std::unique_ptr<ConfirmPrompt> makeTerminalPrompt(std::istream& in, std::ostream& out);

/// @param uptime A duration.
/// @return It in at most three units of d, h, m, s: "45s", "2m 05s", "2h 03m 05s", "3d 04h 00m".
[[nodiscard]] std::string formatUptime(std::chrono::seconds uptime);

/// @param status A daemon's report on itself.
/// @return One `label: value` line per fact, the labels padded to one column.
[[nodiscard]] std::string formatDaemonStatus(vthost::client::DaemonStatus const& status);

/// Reads a log file's tail, then what is appended to it.
class LogFollower
{
  public:
    /// @param fs The file system the log is read through (not owned).
    /// @param file The log file.
    LogFollower(core::platform::FileSystem& fs, std::filesystem::path file);

    /// Reads the end of the file and remembers where it ended.
    /// @param lines How many trailing lines to return; 0 for all of the last 256 KiB.
    /// @return The lines, each newline-terminated; empty when the file cannot be read.
    [[nodiscard]] std::string tail(std::size_t lines);

    /// @return The bytes appended since the previous call; a file that shrank is read from its start.
    [[nodiscard]] std::string poll();

  private:
    core::platform::FileSystem& _fs;
    std::filesystem::path _file;
    std::uintmax_t _offset = 0;
};

/// `contour daemon status`.
/// @param control The daemon's control port.
/// @param socket The socket asked, named when no daemon answers.
/// @param out Where the report goes.
/// @param err Where a failure goes.
/// @return Ok; Unreachable when no daemon runs; Failed otherwise.
[[nodiscard]] ExitStatus daemonStatusVerb(vthost::client::DaemonControlPort& control,
                                          std::filesystem::path const& socket,
                                          std::ostream& out,
                                          std::ostream& err);

/// Merges an option a sub-verb shares with `contour daemon` itself, given before and/or after the verb.
/// @param name The option's name, for the message.
/// @param parent The value given before the sub-verb (or its default).
/// @param child The value given after the sub-verb (or its default).
/// @param unset The option's default, which reads as "not given".
/// @return The value that applies, or the refusal text when both were given and differ.
[[nodiscard]] std::expected<std::string, std::string> mergeSharedOption(std::string_view name,
                                                                        std::string const& parent,
                                                                        std::string const& child,
                                                                        std::string const& unset);

/// `contour daemon stop`.
/// @param control The daemon's control port.
/// @param socket The socket asked, named when no daemon answers.
/// @param consent Whether sessions may be ended without asking.
/// @param prompt Asked when sessions exist and consent is not given.
/// @param out Where the confirmation goes.
/// @param err Where a failure or refusal goes.
/// @return Ok once stopped; Unreachable when no daemon runs; Failed otherwise.
[[nodiscard]] ExitStatus daemonStopVerb(vthost::client::DaemonControlPort& control,
                                        std::filesystem::path const& socket,
                                        StopConsent consent,
                                        ConfirmPrompt& prompt,
                                        std::ostream& out,
                                        std::ostream& err);

/// What `contour daemon logs` prints.
struct LogsOptions
{
    std::size_t lines = 100;              ///< How many trailing lines to print first.
    FollowMode follow = FollowMode::Once; ///< Whether to keep printing.
};

/// `contour daemon logs`.
/// @param control The daemon's control port; names the log file.
/// @param fallbackLog The log read when no daemon runs (the default beside the socket).
/// @param options What to print.
/// @param fs The file system the log is read through.
/// @param waitTick Waits one polling interval between follow polls; Stop ends the follow.
/// @param out Where the log goes.
/// @param err Where notes and failures go.
/// @return Ok; Failed when the log cannot be named or read.
[[nodiscard]] ExitStatus daemonLogsVerb(vthost::client::DaemonControlPort& control,
                                        std::filesystem::path const& fallbackLog,
                                        LogsOptions options,
                                        core::platform::FileSystem& fs,
                                        std::function<FollowStep()> const& waitTick,
                                        std::ostream& out,
                                        std::ostream& err);

} // namespace contour::cli
