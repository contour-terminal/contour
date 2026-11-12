// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `contour daemon status | logs | stop | handover-info`: the verbs that talk to a running daemon instead of
/// becoming one. Each takes its collaborators (the control port, the prompt, the file system, the output
/// streams) so the decisions are testable without a daemon, a terminal or a clock.

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
#include <span>
#include <string>
#include <string_view>

#include <vthost/DaemonBinary.hpp>
#include <vthost/DaemonStarter.hpp>
#include <vthost/KnownDaemons.hpp>
#include <vthost/WorkspacePersistenceConfig.hpp>
#include <vthost/client/DaemonControlClient.hpp>
#include <vthost/client/DaemonRelocation.hpp>
#include <vthost/client/DaemonUpgrade.hpp>

namespace contour::config
{
struct DaemonRestoreConfig;
} // namespace contour::config

namespace vthost
{
class SessionSnapshotStore;
} // namespace vthost

namespace contour::cli
{

/// What `contour daemon` was asked to do; Run is the daemon itself, the rest talk to one.
enum class DaemonVerb : uint8_t
{
    Run = 0, ///< Become the daemon.
    Status,  ///< Report on the running daemon.
    Logs,    ///< Print the running daemon's log.
    Stop,    ///< Stop the running daemon.
    Upgrade, ///< Replace the running daemon with this release's, keeping its sessions running.
    /// Move every daemon of this user that runs from this installation onto its staged copy.
    Relocate,
    /// Print what this binary reads of a live upgrade's handover, for the daemon that would exec it.
    HandoverInfo,
    /// Forget the pinned certificate of a daemon met over TCP, so that its next contact is a first contact.
    ForgetPin,
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
    DaemonVerbRow {
        "stop",
        "Stops the daemon, ending every session it hosts; asks first when sessions exist. The next "
        "daemon starts afresh, unless --keep-snapshot keeps the workspace for it.",
        DaemonVerb::Stop },
    DaemonVerbRow {
        "upgrade",
        "Upgrades the running daemon to this release without ending its sessions: every program keeps "
        "running and the clients reconnect. Exits 5 with the reason when the daemon declines.",
        DaemonVerb::Upgrade },
    DaemonVerbRow {
        "relocate",
        "Moves every daemon of this user that runs from this installation onto its staged copy, keeping its "
        "sessions, so that the installation's files can be replaced. The Windows installer runs it.",
        DaemonVerb::Relocate },
    DaemonVerbRow {
        "handover-info",
        "Internal: prints, as one line of JSON, this binary's release and the handover and terminal "
        "state formats it reads -- what a daemon asks of a new release before it upgrades onto it "
        "live.",
        DaemonVerb::HandoverInfo },
    DaemonVerbRow {
        "forget-pin",
        "Forgets the certificate pinned for the daemon at HOST:PORT, after that daemon was reinstalled; the "
        "next connection to it is a first contact again.",
        DaemonVerb::ForgetPin },
};

/// Whether `daemon stop` may end sessions without asking (`--yes`).
enum class StopConsent : uint8_t
{
    Ask = 0, ///< Ask first when sessions exist.
    Given,   ///< Already given on the command line.
};

/// What `daemon stop` does with the daemon's saved workspace (`--keep-snapshot`).
enum class StopDisposition : uint8_t
{
    Discard = 0,  ///< The next daemon starts afresh.
    KeepSnapshot, ///< The next daemon restores the workspace.
};

/// How `daemon stop` stops.
struct StopOptions
{
    StopConsent consent = StopConsent::Ask;                 ///< Whether to ask before ending sessions.
    StopDisposition disposition = StopDisposition::Discard; ///< What becomes of the saved workspace.
    /// The daemon's snapshot file, emptied when no daemon runs and the workspace is discarded; null when no
    /// file can be named for the daemon's label.
    vthost::SessionSnapshotStore const* savedWorkspace = nullptr;
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

/// The program a daemon service is registered to start.
///
/// A registration outlives the shell that made it, so the program is the executable that is running, not
/// argv[0]: a shell starts a program found on the PATH with the name it was typed under, and absolute() of
/// that is a file in the current directory.
/// @param running The running executable as the operating system reports it
/// (vtpty::currentExecutablePath()),
///        which also resolves an AppImage to the image.
/// @param argv0 The program name the process was started with, the fallback where @p running is not known.
/// @return @p running when known, else @p argv0 made absolute.
[[nodiscard]] std::filesystem::path daemonServiceProgram(
    std::expected<std::filesystem::path, std::error_code> const& running, std::string_view argv0);

/// What repairing a daemon task needs, for the call sites that look for a registration.
///
/// A task whose program an upgrade removed is pointed at the installed executable: the one the installer
/// recorded, never the file this process runs from, which may as well be a portable copy or a build tree.
/// @param fileSystem Where the programs are looked for.
/// @return The inputs, with the installed executable of this machine.
[[nodiscard]] vthost::RegistrationRepairInputs nativeRepairInputs(
    core::platform::FileSystem const& fileSystem);

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

/// `contour daemon forget-pin HOST:PORT`.
/// @param knownDaemons Where the daemons are pinned.
/// @param words What followed the verb: exactly one `HOST:PORT`.
/// @param out Where the confirmation goes.
/// @param err Where a refusal goes.
/// @return Ok when the pin was forgotten; BadUsage for a missing or malformed daemon, or none pinned for it;
///         Failed when the file could not be written.
[[nodiscard]] ExitStatus daemonForgetPinVerb(vthost::KnownDaemons& knownDaemons,
                                             std::span<std::string_view const> words,
                                             std::ostream& out,
                                             std::ostream& err);

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
///
/// A KeepSnapshot stop asks the daemon to keep its workspace only when it offers
/// Capability::WorkspaceRestore (an older daemon refuses the request); otherwise it says the daemon keeps
/// none, and stops it plainly. With no daemon running, a Discard stop empties a saved workspace that would
/// come back, so the next daemon does not bring back what the user meant to end — after asking, unless
/// consent is given, since a crash may have left it as all the user has.
/// @param control The daemon's control port.
/// @param socket The socket asked, named when no daemon answers.
/// @param options Whether to ask first, and what becomes of the saved workspace.
/// @param prompt Asked when sessions exist, or a workspace would be cleared, and consent is not given.
/// @param out Where the confirmation goes.
/// @param err Where a failure or refusal goes.
/// @return Ok once stopped; Unreachable when no daemon runs; Failed otherwise, also when a saved workspace
///         was neither cleared nor kept by the user's answer.
[[nodiscard]] ExitStatus daemonStopVerb(vthost::client::DaemonControlPort& control,
                                        std::filesystem::path const& socket,
                                        StopOptions const& options,
                                        ConfirmPrompt& prompt,
                                        std::ostream& out,
                                        std::ostream& err);

/// `contour daemon upgrade`.
///
/// Stages this release's image, asks the daemon to replace itself with it and reports the outcome. A failure
/// whose outcome is unknown (the daemon did not answer) is settled by asking the daemon's release again until
/// the reconnect hold ran out.
/// @param control The daemon's control port, with a timeout of vthost::client::DefaultUpgradeTimeout.
/// @param binary What this release's image is staged from.
/// @param pacer The clock and waits of the settling of an outcome that is not known.
/// @param targetRelease This release, named in the confirmation.
/// @param socket The socket asked, named when no daemon answers.
/// @param out Where the confirmation goes.
/// @param err Where a failure or refusal goes.
/// @return Ok once the daemon upgrades; Unreachable when no daemon runs; Refused, with the reason, when it
///         declines; Failed when this release's image could not be staged, or the outcome stays unknown (the
///         daemon neither answered nor came back).
[[nodiscard]] ExitStatus daemonUpgradeVerb(vthost::client::DaemonControlPort& control,
                                           vthost::DaemonBinarySource& binary,
                                           vthost::client::SettlePacer& pacer,
                                           std::string_view targetRelease,
                                           std::filesystem::path const& socket,
                                           std::ostream& out,
                                           std::ostream& err);

/// Opens the control port of the daemon at a socket, with the given timeout for a whole call.
using DaemonControlFor = std::function<std::unique_ptr<vthost::client::DaemonControlPort>(
    std::filesystem::path const& socket, std::chrono::milliseconds timeout)>;

/// The control sockets in a socket directory.
/// @param fileSystem Where the directory is listed.
/// @param directory The directory the label sockets lie in.
/// @return The directory's entries that are no sibling endpoint (vthost::client::controlSocketsAmong()),
///         sorted; none where the directory cannot be listed.
[[nodiscard]] std::vector<std::filesystem::path> daemonSocketsIn(core::platform::FileSystem const& fileSystem,
                                                                 std::filesystem::path const& directory);

/// `contour daemon relocate`.
///
/// Moves each daemon that runs from @p installationBin and allows it onto its staged copy
/// (vthost::client::upgradeDaemonTo(), the hand-over onto this release's staged image), so that an installer
/// can replace the installation's files. A socket nobody answers on (a stale file) is skipped silently, and a
/// daemon of another installation, one started with `--in-place` and one too old to say are left, with the
/// reason. Never "unreachable": having no daemon to move is the usual case.
/// @param sockets The control sockets to look at.
/// @param controlFor Opens the control port of the daemon at a socket: one of
/// vthost::client::DefaultControlTimeout
///        to look at it, and one of vthost::client::DefaultUpgradeTimeout to move it.
/// @param binary What the staged image is made from.
/// @param pacer The clock and waits of the settling of an outcome that is not known.
/// @param installationBin The directory of this installation's executable.
/// @param out Where what was moved or left goes.
/// @param err Where a failed move goes.
/// @return Ok unless a move failed, then Failed.
[[nodiscard]] ExitStatus daemonRelocateVerb(std::span<std::filesystem::path const> sockets,
                                            DaemonControlFor const& controlFor,
                                            vthost::DaemonBinarySource& binary,
                                            vthost::client::SettlePacer& pacer,
                                            std::filesystem::path const& installationBin,
                                            std::ostream& out,
                                            std::ostream& err);

/// @param restore The `daemon.restore` section.
/// @param snapshotPath The daemon's snapshot file; empty keeps no workspaces.
/// @return How the daemon keeps its workspace: its scrollback Off when `scrollback` is false or
///         `scrollback_lines` is 0, else the Tail of that many rows.
[[nodiscard]] vthost::WorkspacePersistenceConfig workspacePersistenceOf(
    config::DaemonRestoreConfig const& restore, std::filesystem::path snapshotPath);

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
