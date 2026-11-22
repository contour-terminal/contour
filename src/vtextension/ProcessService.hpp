// SPDX-License-Identifier: Apache-2.0
//
// The processes a daemon-world extension starts, as its host-function table reaches them (`proc-start`,
// `procs`, `proc-take`, `proc-release`): an interface, so that the table is tested with a fake and the
// policy behind it without a guest.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vtextension/DaemonWorld.hpp>
#include <vtextension/IHostServices.hpp>

namespace vtextension
{

/// Names one process an extension started, to that extension.
enum class ProcessHandle : uint32_t
{
};

/// Why a process was not started, or a call about one was refused.
enum class ProcessError : uint8_t
{
    NotDeclared,      ///< The manifest declares no command of that name.
    Pending,          ///< The user has not answered whether the extension may run its commands.
    Denied,           ///< The user refused.
    ArgumentRejected, ///< A placeholder has no value, a name is not the command's, or a value is not its
                      ///< kind.
    CwdOutsideRoots,  ///< The working directory does not belong to the command's root.
    CwdMissing,       ///< The working directory does not exist (or its root could not be created).
    CwdNotADirectory, ///< The working directory is no directory, or may not be opened.
    SpawnFailed,      ///< The program could not be started: not found, or the system refused.
    TooMany,          ///< Too many processes run, or too many results are kept.
    Unsupported,      ///< This platform cannot run a declared command.
    UnknownHandle,    ///< No process of the extension has that handle (any more).
    StillRunning,     ///< The process has not finished, so it has no output to take yet.
};

/// What a finished process left behind.
struct ProcessResult
{
    ProcCompletion completion = ProcCompletion::Exited; ///< How it ended.
    std::optional<int32_t> exitCode;                    ///< Its exit code, when it exited.
    std::size_t stdoutBytes = 0;                        ///< How many bytes of its standard output are kept.
    OutputExtent stdoutExtent = OutputExtent::Whole;    ///< Whether its standard output is kept whole.
    std::size_t stderrBytes = 0;                        ///< How many bytes of its standard error are kept.
    OutputExtent stderrExtent = OutputExtent::Whole;    ///< Whether its standard error is kept whole.

    [[nodiscard]] bool operator==(ProcessResult const&) const = default;
};

/// A process the extension started and has not released.
struct ProcessInfo
{
    ProcessHandle handle {};             ///< What start() returned for it.
    std::string tag;                     ///< The extension's own label for it.
    std::string command;                 ///< The declared command it runs.
    std::optional<ProcessResult> result; ///< How it ended; std::nullopt while it runs.

    [[nodiscard]] bool operator==(ProcessInfo const&) const = default;
};

/// A page of a finished process's output.
struct OutputPage
{
    std::string bytes;         ///< The bytes from the requested offset, at most one page.
    std::size_t remaining = 0; ///< How many bytes of the stream follow them.

    [[nodiscard]] bool operator==(OutputPage const&) const = default;
};

/// The declared processes of one extension.
class IProcessService
{
  public:
    IProcessService() = default;
    IProcessService(IProcessService const&) = delete;
    IProcessService(IProcessService&&) = delete;
    IProcessService& operator=(IProcessService const&) = delete;
    IProcessService& operator=(IProcessService&&) = delete;
    virtual ~IProcessService() = default;

    /// Starts a declared command.
    /// @param command The declared command's name.
    /// @param arguments A value for each of its placeholders, by name.
    /// @param cwd The directory it runs in: absolute, inside the command's root.
    /// @param tag The extension's own label for the process.
    /// @return The process's handle; or why it was not started.
    [[nodiscard]] virtual std::expected<ProcessHandle, ProcessError> start(std::string_view command,
                                                                           ConfigMap const& arguments,
                                                                           std::string_view cwd,
                                                                           std::string_view tag) = 0;

    /// @return The processes started and not released, by handle.
    [[nodiscard]] virtual std::vector<ProcessInfo> list() const = 0;

    /// Reads a page of a finished process's output.
    /// @param handle The process.
    /// @param stream Which of its streams.
    /// @param offset Where in the stream the page starts; past its end, the page is empty.
    /// @return The page; or `UnknownHandle`, or `StillRunning`.
    [[nodiscard]] virtual std::expected<OutputPage, ProcessError> take(ProcessHandle handle,
                                                                       ProcStream stream,
                                                                       std::size_t offset) const = 0;

    /// Forgets a process and its output; one still running is stopped.
    /// @param handle The process.
    /// @return Nothing; or `UnknownHandle`.
    [[nodiscard]] virtual std::expected<void, ProcessError> release(ProcessHandle handle) = 0;
};

/// One row of the table that turns a ProcessError into what the guest sees.
struct ProcessErrorRow
{
    ProcessError error;  ///< The service's reason.
    HostError hostError; ///< The WIT host-error it reaches the guest as.
};

/// One row per error, indexed by value: the WIT host-error it reaches the guest as. Only a missing working
/// directory is `gone` (the guest's engine ends a run on it); every other failure to start is a refused
/// start, never a vanished worktree.
inline constexpr auto ProcessErrorToHostError = std::array {
    ProcessErrorRow { .error = ProcessError::NotDeclared, .hostError = HostError::BadArgument },
    ProcessErrorRow { .error = ProcessError::Pending, .hostError = HostError::NotGranted },
    ProcessErrorRow { .error = ProcessError::Denied, .hostError = HostError::NotGranted },
    ProcessErrorRow { .error = ProcessError::ArgumentRejected, .hostError = HostError::BadArgument },
    ProcessErrorRow { .error = ProcessError::CwdOutsideRoots, .hostError = HostError::BadArgument },
    ProcessErrorRow { .error = ProcessError::CwdMissing, .hostError = HostError::Gone },
    ProcessErrorRow { .error = ProcessError::CwdNotADirectory, .hostError = HostError::NotADirectory },
    ProcessErrorRow { .error = ProcessError::SpawnFailed, .hostError = HostError::BadArgument },
    ProcessErrorRow { .error = ProcessError::TooMany, .hostError = HostError::Limit },
    ProcessErrorRow { .error = ProcessError::Unsupported, .hostError = HostError::NotSupportedYet },
    ProcessErrorRow { .error = ProcessError::UnknownHandle, .hostError = HostError::Gone },
    ProcessErrorRow { .error = ProcessError::StillRunning, .hostError = HostError::Limit },
};

static_assert(ProcessErrorToHostError.size() == std::to_underlying(ProcessError::StillRunning) + 1U,
              "every ProcessError has a row");
static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, ProcessErrorToHostError.size()),
                                  [](std::size_t index) {
                                      return std::to_underlying(ProcessErrorToHostError.at(index).error)
                                             == index;
                                  }),
              "ProcessErrorToHostError is indexed by the enumerator's value");

/// @param error Why the service refused.
/// @return The host-error the guest is answered with, by ProcessErrorToHostError.
[[nodiscard]] constexpr HostError hostErrorOf(ProcessError error) noexcept
{
    return ProcessErrorToHostError.at(std::to_underlying(error)).hostError;
}

} // namespace vtextension
