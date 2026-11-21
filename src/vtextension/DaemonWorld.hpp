// SPDX-License-Identifier: Apache-2.0
//
// The daemon world's types (`terminal-extensions:daemon@0.1.0`, wit/daemon/world.wit) as the host
// sees them, and the rows that describe them to the Canonical ABI codec. Each enum's enumerators are
// in the WIT's order: their values are the wire's discriminants.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

#include <vtextension/CanonicalAbi.hpp>
#include <vtextension/Event.hpp>
#include <vtextension/IHostServices.hpp>
#include <vtextension/StatuslineWorld.hpp> // LogLevel's codec row: `log-level` is the status-line world's

namespace vtextension
{

/// Why the host calls `update`: the world's `hint`. A hint carries no payload.
enum class DaemonHint : uint8_t
{
    Wake,    ///< The time asked for with `wake` has come.
    Procs,   ///< A process the extension started has finished.
    Panes,   ///< A pane the extension owns changed.
    Tabs,    ///< A tab the extension serves changed.
    Actions, ///< The user invoked one of the extension's actions.
};

/// Why the host refused a call: the world's `host-error`.
enum class HostError : uint8_t
{
    NotGranted,      ///< The user has not granted what the call needs.
    NotOwned,        ///< The pane, tab or process belongs to another extension.
    BadArgument,     ///< An argument is not what the manifest declares or the call accepts.
    Limit,           ///< A limit of the host is reached.
    Gone,            ///< The pane, tab or process no longer exists.
    NotSupportedYet, ///< The host does not serve this import yet: a capability, never a fault.
    NotADirectory,   ///< The working directory is no directory, or may not be opened.
    TooMany,         ///< The extension has as many panes as it may.
    ProgramMissing,  ///< A pane template's program is not found on the PATH the pane would start it with.
};

/// Why the host did not read a file: the world's `file-error`.
enum class FileError : uint8_t
{
    NotGranted,      ///< The root is not declared readable, or not granted.
    BadArgument,     ///< The path is not relative, normalised and free of `..`.
    DirectoryGone,   ///< A directory on the path does not exist: the root's own, or one beneath it.
    NotRegular,      ///< The path names no regular file.
    Unreadable,      ///< The file exists but cannot be read.
    TooLarge,        ///< The file is larger than one call may return.
    Escapes,         ///< The path leaves the root.
    NotSupportedYet, ///< The host does not serve this import yet.
};

/// The host's clocks, read at once: the world's `clock-reading`.
struct ClockReading
{
    uint64_t monotonicMs = 0; ///< Milliseconds on a clock that never goes back.
    uint64_t wallMs = 0;      ///< Milliseconds since the Unix epoch.

    [[nodiscard]] bool operator==(ClockReading const&) const = default;
};

/// A process the extension started: the world's `proc-handle`.
using ProcHandle = uint32_t;

/// The connection an action came from: the world's `origin`.
using Origin = uint32_t;

/// One of a process's two output streams: the world's `proc-stream`.
enum class ProcStream : uint8_t
{
    Stdout, ///< Its standard output.
    Stderr, ///< Its standard error.
};

/// How a process ended: the world's `completion`.
enum class ProcCompletion : uint8_t
{
    Exited,    ///< It exited.
    Signalled, ///< A signal ended it.
    TimedOut,  ///< The host ended it when its declared timeout passed.
};

/// Whether the host kept a stream whole: the world's `extent`.
enum class OutputExtent : uint8_t
{
    Whole,     ///< Everything the process wrote is kept.
    Truncated, ///< The process wrote more than the host keeps.
};

/// What a finished process left behind: the world's `proc-result`.
struct ProcResult
{
    ProcCompletion completion = ProcCompletion::Exited; ///< How it ended.
    std::optional<int32_t> exitCode;                    ///< Its exit code, when it exited.
    uint32_t stdoutBytes = 0;                           ///< How many bytes of its standard output are kept.
    OutputExtent stdoutExtent = OutputExtent::Whole;    ///< Whether its standard output is kept whole.
    uint32_t stderrBytes = 0;                           ///< How many bytes of its standard error are kept.
    OutputExtent stderrExtent = OutputExtent::Whole;    ///< Whether its standard error is kept whole.

    [[nodiscard]] bool operator==(ProcResult const&) const = default;
};

/// The `running` case of `proc-state`: a case without a payload.
struct ProcRunning
{
    [[nodiscard]] bool operator==(ProcRunning const&) const = default;
};

/// Whether a process is still running: the world's `proc-state`.
using ProcState = std::variant<ProcRunning, ProcResult>;

/// A process the extension started and has not released: the world's `proc-info`.
struct ProcInfo
{
    ProcHandle handle = 0;            ///< The handle `proc-start` returned.
    std::string tag;                  ///< The extension's own label for it.
    std::string command;              ///< The declared command it runs.
    ProcState state = ProcRunning {}; ///< Whether it is still running.

    [[nodiscard]] bool operator==(ProcInfo const&) const = default;
};

/// A page of a finished process's output: the world's `chunk`.
struct Chunk
{
    ByteList bytes;         ///< The bytes from the requested offset.
    uint32_t remaining = 0; ///< How many bytes of the stream follow this page.

    [[nodiscard]] bool operator==(Chunk const&) const = default;
};

/// What the program in a pane reports about its work: the world's `agent-status`.
enum class AgentStatus : uint8_t
{
    Idle,    ///< It waits for input.
    Working, ///< It is working.
    Done,    ///< It finished its task.
    Blocked, ///< It needs the user.
    Error,   ///< It failed.
};

/// Whether the program the host spawned in a pane is still its program: the world's `program`.
enum class PaneProgram : uint8_t
{
    Alive, ///< It is.
    Gone,  ///< It exited, or another program replaced it.
};

/// A pane the extension opened: the world's `pane-info`.
struct PaneInfo
{
    std::string pane;                         ///< The pane's id.
    std::string tag;                          ///< The extension's own label for it.
    std::optional<AgentStatus> status;        ///< What the pane's program last reported, if anything.
    std::string message;                      ///< The reason line it reported with its status; may be empty.
    PaneProgram program = PaneProgram::Alive; ///< Whether the spawned program is still the pane's program.

    [[nodiscard]] bool operator==(PaneInfo const&) const = default;
};

/// A tab the extension serves: the world's `tab-info`.
struct TabInfo
{
    std::string tab;    ///< The tab's id.
    std::string scheme; ///< The scheme it was opened with.
    uint32_t cols = 0;  ///< Its width, in cells.
    uint32_t rows = 0;  ///< Its height, in cells.

    [[nodiscard]] bool operator==(TabInfo const&) const = default;
};

/// An action the user invoked: the world's `action`.
struct ActionInfo
{
    std::string id;    ///< The action's declared id.
    Origin origin = 0; ///< The connection that invoked it.

    [[nodiscard]] bool operator==(ActionInfo const&) const = default;
};

/// A file's bytes: the world's `file-content`.
struct FileContent
{
    ByteList bytes;              ///< The file's contents.
    uint64_t modifiedUnixMs = 0; ///< When it was last modified, in milliseconds since the Unix epoch.

    [[nodiscard]] bool operator==(FileContent const&) const = default;
};

/// A root the user granted, resolved by the host: the world's `root`.
struct ResolvedRoot
{
    std::string label;              ///< The label the manifest declares the root under.
    std::vector<std::string> paths; ///< Its directories: absolute, normalised, symlinks resolved.

    [[nodiscard]] bool operator==(ResolvedRoot const&) const = default;
};

/// A program a declared command may name that is the daemon's own executable: the world's `self-program`.
struct SelfProgram
{
    std::string name; ///< The program as a command's argv names it, such as `contour`.
    std::string path; ///< The executable the host runs for it: absolute, whatever the daemon's PATH holds.

    [[nodiscard]] bool operator==(SelfProgram const&) const = default;
};

/// What the host tells the extension once, when it loads: the world's `host-info`.
struct DaemonHostInfo
{
    std::vector<ResolvedRoot> roots;            ///< Every root the manifest declares, resolved.
    std::string home;                           ///< The user's home directory.
    int32_t utcOffsetMinutes = 0;               ///< How far local time is ahead of UTC at load.
    std::vector<SelfProgram> selfPrograms = {}; ///< The programs the host runs as its own executable.

    [[nodiscard]] bool operator==(DaemonHostInfo const&) const = default;
};

} // namespace vtextension

namespace vtextension::abi
{

// clang-format off
template <> struct RecordFields<ClockReading> { static constexpr auto Members = std::tuple { &ClockReading::monotonicMs, &ClockReading::wallMs }; };
template <> struct RecordFields<ProcResult> { static constexpr auto Members = std::tuple {
    &ProcResult::completion, &ProcResult::exitCode, &ProcResult::stdoutBytes, &ProcResult::stdoutExtent,
    &ProcResult::stderrBytes, &ProcResult::stderrExtent }; };
template <> struct RecordFields<ProcRunning> { static constexpr auto Members = std::tuple {}; };
template <> struct RecordFields<ProcInfo> { static constexpr auto Members = std::tuple { &ProcInfo::handle, &ProcInfo::tag, &ProcInfo::command, &ProcInfo::state }; };
template <> struct RecordFields<Chunk> { static constexpr auto Members = std::tuple { &Chunk::bytes, &Chunk::remaining }; };
template <> struct RecordFields<PaneInfo> { static constexpr auto Members = std::tuple { &PaneInfo::pane, &PaneInfo::tag, &PaneInfo::status, &PaneInfo::message, &PaneInfo::program }; };
template <> struct RecordFields<TabInfo> { static constexpr auto Members = std::tuple { &TabInfo::tab, &TabInfo::scheme, &TabInfo::cols, &TabInfo::rows }; };
template <> struct RecordFields<ActionInfo> { static constexpr auto Members = std::tuple { &ActionInfo::id, &ActionInfo::origin }; };
template <> struct RecordFields<FileContent> { static constexpr auto Members = std::tuple { &FileContent::bytes, &FileContent::modifiedUnixMs }; };
template <> struct RecordFields<ResolvedRoot> { static constexpr auto Members = std::tuple { &ResolvedRoot::label, &ResolvedRoot::paths }; };
template <> struct RecordFields<SelfProgram> { static constexpr auto Members = std::tuple { &SelfProgram::name, &SelfProgram::path }; };
template <> struct RecordFields<DaemonHostInfo> { static constexpr auto Members = std::tuple { &DaemonHostInfo::roots, &DaemonHostInfo::home, &DaemonHostInfo::utcOffsetMinutes, &DaemonHostInfo::selfPrograms }; };

template <> inline constexpr uint32_t EnumCaseCount<DaemonHint> = 5;
template <> inline constexpr uint32_t EnumCaseCount<HostError> = 9;
template <> inline constexpr uint32_t EnumCaseCount<FileError> = 8;
template <> inline constexpr uint32_t EnumCaseCount<ProcStream> = 2;
template <> inline constexpr uint32_t EnumCaseCount<ProcCompletion> = 3;
template <> inline constexpr uint32_t EnumCaseCount<OutputExtent> = 2;
template <> inline constexpr uint32_t EnumCaseCount<AgentStatus> = 5;
template <> inline constexpr uint32_t EnumCaseCount<PaneProgram> = 2;
// clang-format on

static_assert(EnumCaseCount<LogLevel> == 5, "the daemon world's log-level is the status-line world's");

} // namespace vtextension::abi
