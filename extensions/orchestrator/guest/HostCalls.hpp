// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <orchestrator/core/Commands.hpp>
#include <orchestrator/core/Model.hpp>
#include <orchestrator/core/Reconcile.hpp>
#include <orchestrator/core/Run.hpp>

/// The orchestrator as a guest of the daemon world (`terminal-extensions:daemon`): its driver, and the host
/// calls the driver is written against.
namespace orchestrator::guest
{

/// Why the host calls `update`, in the order the host delivers pending hints.
enum class Hint : uint8_t
{
    Wake,    ///< The time asked for with IGuestHost::wake() has come.
    Procs,   ///< A process the guest started has finished.
    Panes,   ///< A pane the guest owns changed.
    Tabs,    ///< A tab the guest serves changed.
    Actions, ///< The user invoked one of the guest's actions.
};

/// How loud a logged message is.
enum class LogLevel : uint8_t
{
    Trace, ///< Detail only a developer wants.
    Debug, ///< Detail that explains a decision.
    Info,  ///< Something the user may want to know.
    Warn,  ///< Something went wrong, and the guest carries on as before.
    Error, ///< Something went wrong that loses work or state.
};

/// Why the host refused a call.
enum class HostError : uint8_t
{
    NotGranted,      ///< The user has not granted what the call needs.
    NotOwned,        ///< The pane, tab or process belongs to another extension.
    BadArgument,     ///< An argument is not what the manifest declares, or a program is missing.
    Limit,           ///< A limit of the host is reached.
    Gone,            ///< The pane, tab, process or working directory no longer exists.
    NotSupportedYet, ///< The host does not serve this call yet.
};

/// Why the host did not read a file.
enum class FileError : uint8_t
{
    NotGranted,      ///< The root is not declared readable, or not granted.
    BadArgument,     ///< The path is not relative, normalised and free of `..`.
    DirectoryGone,   ///< The root's directory no longer exists.
    NotRegular,      ///< The path names no regular file.
    Unreadable,      ///< The file exists but cannot be read.
    TooLarge,        ///< The file is larger than one call may return.
    Escapes,         ///< The path leaves the root.
    NotSupportedYet, ///< The host does not serve this call yet.
};

/// One of a process's two output streams.
enum class ProcStream : uint8_t
{
    Stdout, ///< Its standard output.
    Stderr, ///< Its standard error.
};

/// How a process ended.
enum class ProcCompletion : uint8_t
{
    Exited,    ///< It exited; ProcResult::exitCode says with what.
    Signalled, ///< A signal ended it.
    TimedOut,  ///< The host ended it when its declared timeout passed.
};

/// The host's clocks, read at once: the guest's only time.
struct ClockReading
{
    uint64_t monoMs = 0; ///< Milliseconds on a clock that never goes back.
    uint64_t wallMs = 0; ///< Milliseconds since the Unix epoch.
};

/// What a finished process left behind.
struct ProcResult
{
    ProcCompletion completion = ProcCompletion::Exited; ///< How it ended.
    std::optional<int32_t> exitCode;                    ///< Its exit code, when it exited.
    uint32_t stdoutBytes = 0;                           ///< The bytes of its standard output the host kept.
    OutputExtent stdoutExtent = OutputExtent::Whole;    ///< Whether its standard output is kept whole.
    uint32_t stderrBytes = 0;                           ///< The bytes of its standard error the host kept.
    OutputExtent stderrExtent = OutputExtent::Whole;    ///< Whether its standard error is kept whole.
};

/// A process the guest started and has not released.
struct ProcInfo
{
    uint32_t handle = 0;              ///< The handle IGuestHost::procStart() returned.
    std::string tag;                  ///< The tag it was started with.
    std::string command;              ///< The declared command it runs.
    std::optional<ProcResult> result; ///< What it left behind; std::nullopt while it runs.
};

/// A page of a finished process's output.
struct Chunk
{
    std::string bytes;      ///< The bytes from the requested offset.
    uint32_t remaining = 0; ///< How many bytes of the stream follow this page.
};

/// A pane the guest opened, as the host lists it.
struct PaneInfo
{
    std::string pane;                                ///< The pane's id.
    std::string tag;                                 ///< The tag it was opened with.
    std::optional<AgentStatus> status;               ///< What its program last reported, if anything.
    ProgramLiveness program = ProgramLiveness::Gone; ///< Whether the spawned program is still its program.
};

/// The daemon world's imports as the driver uses them; WitHost.cpp adapts the generated bindings, and the
/// tests a fake.
class IGuestHost
{
  public:
    virtual ~IGuestHost() = default;

    /// Logs a message to the host's log.
    /// @param level How loud it is.
    /// @param message The message.
    virtual void log(LogLevel level, std::string_view message) = 0;

    /// Asks for one Hint::Wake after @p afterMs milliseconds, replacing any earlier request.
    /// @param afterMs The delay.
    virtual void wake(uint32_t afterMs) = 0;

    /// @return The host's clocks.
    [[nodiscard]] virtual ClockReading now() = 0;

    /// Starts a declared command.
    /// @param command The declared command's name.
    /// @param arguments Its arguments, by placeholder.
    /// @param cwd Its working directory, inside the command's root.
    /// @param tag The guest's own label, which procs() lists it with.
    /// @return Its handle, or why it did not start: `Gone` for a missing working directory, `BadArgument`
    ///         for a missing program among others.
    [[nodiscard]] virtual std::expected<uint32_t, HostError> procStart(std::string_view command,
                                                                       std::span<Argument const> arguments,
                                                                       std::string_view cwd,
                                                                       std::string_view tag) = 0;

    /// @return The processes the guest started and has not released, running and finished.
    [[nodiscard]] virtual std::expected<std::vector<ProcInfo>, HostError> procs() = 0;

    /// Reads a page of a finished process's output.
    /// @param handle The process.
    /// @param stream Which of its streams.
    /// @param offset The byte the page starts at.
    /// @return The page, or why it could not be read.
    [[nodiscard]] virtual std::expected<Chunk, HostError> procTake(uint32_t handle,
                                                                   ProcStream stream,
                                                                   uint32_t offset) = 0;

    /// Frees a finished process's output; its handle is no longer valid.
    /// @param handle The process.
    /// @return Nothing, or why it was not released.
    [[nodiscard]] virtual std::expected<void, HostError> procRelease(uint32_t handle) = 0;

    /// Reads a stored value.
    /// @param key Its key.
    /// @return The value; std::nullopt if the key holds none.
    [[nodiscard]] virtual std::expected<std::optional<std::string>, HostError> storeGet(
        std::string_view key) = 0;

    /// Stores a value durably.
    /// @param key Its key.
    /// @param value The value.
    /// @return Nothing, or why it was not stored.
    [[nodiscard]] virtual std::expected<void, HostError> storeSet(std::string_view key,
                                                                  std::string_view value) = 0;

    /// Removes a stored value durably.
    /// @param key Its key.
    /// @return Nothing, or why it was not removed.
    [[nodiscard]] virtual std::expected<void, HostError> storeRemove(std::string_view key) = 0;

    /// @param prefix What the keys start with.
    /// @return The stored keys that start with @p prefix, sorted.
    [[nodiscard]] virtual std::expected<std::vector<std::string>, HostError> storeKeys(
        std::string_view prefix) = 0;

    /// Opens a pane from a declared template.
    /// @param paneTemplate The template's name.
    /// @param arguments Its arguments, by placeholder.
    /// @param cwd Its working directory.
    /// @param title Its title.
    /// @param tag The guest's own label, which panes() lists it with.
    /// @return The pane's id, or why it was not opened.
    [[nodiscard]] virtual std::expected<std::string, HostError> paneOpen(std::string_view paneTemplate,
                                                                         std::span<Argument const> arguments,
                                                                         std::string_view cwd,
                                                                         std::string_view title,
                                                                         std::string_view tag) = 0;

    /// Closes a pane the guest owns.
    /// @param pane The pane's id.
    /// @return Nothing, or why it was not closed.
    [[nodiscard]] virtual std::expected<void, HostError> paneClose(std::string_view pane) = 0;

    /// @return The panes the guest owns.
    [[nodiscard]] virtual std::expected<std::vector<PaneInfo>, HostError> panes() = 0;

    /// Reads a regular file inside a readable root.
    /// @param root The root's label.
    /// @param path The file, relative to the root.
    /// @return Its bytes, or why they could not be read.
    [[nodiscard]] virtual std::expected<std::string, FileError> fileRead(std::string_view root,
                                                                         std::string_view path) = 0;

    /// Sets the guest's summary.
    /// @param shortText One line for a status line.
    /// @param detail Its detail.
    /// @return Nothing, or why it was not set.
    [[nodiscard]] virtual std::expected<void, HostError> summarySet(std::string_view shortText,
                                                                    std::string_view detail) = 0;
};

/// @param error A host error.
/// @return Its name as the daemon world spells it, such as `not-supported-yet`.
[[nodiscard]] std::string_view nameOf(HostError error) noexcept;

/// @param error A file error.
/// @return Its name as the daemon world spells it, such as `directory-gone`.
[[nodiscard]] std::string_view nameOf(FileError error) noexcept;

} // namespace orchestrator::guest
