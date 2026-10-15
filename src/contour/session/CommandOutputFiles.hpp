// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/SandboxInfo.hpp>

#include <core/Environment.hpp>
#include <core/platform/FileSystem.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <set>
#include <string_view>

namespace contour::session
{

/// Why a command's output could not be put into a file for a pager.
enum class OutputFileError : uint8_t
{
    DirectoryUnavailable = 0, ///< The private directory could not be created, or a link is in its place.
    WriteFailed,              ///< The file could not be written.
    PermissionsFailed,        ///< The directory or the file could not be made owner-only.
};

/// A short reason for @p error, for logs and user-facing notices.
/// @param error The failure.
/// @return A lower-case phrase without a trailing period.
[[nodiscard]] constexpr std::string_view describe(OutputFileError error) noexcept
{
    switch (error)
    {
        case OutputFileError::DirectoryUnavailable: return "the private output directory is unavailable";
        case OutputFileError::WriteFailed: return "the output could not be written";
        case OutputFileError::PermissionsFailed: return "the output could not be made private";
    }
    return "unknown error";
}

/// Where this process keeps the files it hands to a pager: `contour-output-<pid>` in the runtime
/// directory it shares with the rest of the user's session (vtpty::sharedRuntimeDirectory(): per user,
/// owner-only, cleared at logout) when there is one, else under @p temporaryDirectory.
///
/// Shared, because a pager pane in a Flatpak runs on the host (flatpak-spawn --host), and the
/// sandbox's own `$XDG_RUNTIME_DIR` is a directory the host cannot see.
///
/// On Windows, where a file's permission bits are only its read-only flag, what keeps the files
/// private is that the temporary directory is the user's own.
///
/// @param environment Where XDG_RUNTIME_DIR is read; a relative value is ignored, as the XDG base
///                    directory specification asks.
/// @param sandbox The sandbox this process runs in.
/// @param temporaryDirectory The system's temporary directory; empty when unknown.
/// @param processId This process's id, which makes the name unique among running instances.
/// @return The directory; empty when there is nowhere to put it.
[[nodiscard]] std::filesystem::path commandOutputDirectory(core::Environment const& environment,
                                                           vtpty::SandboxInfo const& sandbox,
                                                           std::filesystem::path const& temporaryDirectory,
                                                           int64_t processId);

/// Whether a process is still running, as far as this one can tell.
enum class ProcessState : uint8_t
{
    Unknown = 0, ///< It could not be told; treated as running.
    Running,     ///< A process has the id. It may be a later one that reused it.
    Gone,        ///< No process has the id.
};

/// Asks the platform whether a process is running: injected, so that what is decided about another
/// process's files is tested without that process.
class ProcessLiveness
{
  public:
    ProcessLiveness() = default;
    ProcessLiveness(ProcessLiveness const&) = default;
    ProcessLiveness& operator=(ProcessLiveness const&) = default;
    ProcessLiveness(ProcessLiveness&&) = default;
    ProcessLiveness& operator=(ProcessLiveness&&) = default;
    virtual ~ProcessLiveness() = default;

    /// @param processId The process to ask about.
    /// @return Whether it runs.
    [[nodiscard]] virtual ProcessState stateOf(int64_t processId) const = 0;
};

/// The platform's answer: kill(pid, 0) on POSIX, OpenProcess() on Windows.
class NativeProcessLiveness final: public ProcessLiveness
{
  public:
    [[nodiscard]] ProcessState stateOf(int64_t processId) const override;
};

/// The private files a command's output is written to for a pager pane (OpenCommandOutput).
///
/// All of them live in one directory this object creates on first use, readable by the owner only;
/// each file is deleted when the pane showing it closes (release()), and the directory, with
/// anything left in it, when this object goes -- at exit. A crash skips that, so on first use the
/// directories of processes that are no longer running are removed. The filesystem and the process
/// liveness are injected, so tests run over an in-memory filesystem and made-up processes.
class CommandOutputFiles
{
  public:
    /// @param fileSystem Where the files are written. Borrowed: it must outlive this object.
    /// @param liveness Whether the process that left a private directory still runs. Borrowed: it must
    ///                 outlive this object.
    /// @param directory The private directory (@see commandOutputDirectory); empty disables the files.
    CommandOutputFiles(core::platform::FileSystem const& fileSystem,
                       ProcessLiveness const& liveness,
                       std::filesystem::path directory);
    CommandOutputFiles(CommandOutputFiles const&) = delete;
    CommandOutputFiles& operator=(CommandOutputFiles const&) = delete;
    CommandOutputFiles(CommandOutputFiles&&) = delete;
    CommandOutputFiles& operator=(CommandOutputFiles&&) = delete;
    ~CommandOutputFiles();

    /// Writes @p content to a new file in the private directory, creating the directory first if
    /// needed -- again, if it was removed since. A directory left by an earlier process with the same
    /// id (a crash skips the cleanup) is removed and made afresh; a link in its place is never
    /// followed.
    /// @param content The bytes to write.
    /// @return The file's path, or why there is none.
    [[nodiscard]] std::expected<std::filesystem::path, OutputFileError> create(std::string_view content);

    /// Deletes @p file, if create() handed it out; any other path is left alone.
    ///
    /// A file another process still holds open is kept where the platform refuses to remove it -- on
    /// Windows, while a detached program reads it as its standard input. It is removed by a later
    /// create() once that process has let go, or with the directory at exit.
    /// @param file A path create() returned.
    void release(std::filesystem::path const& file);

    /// The private directory.
    [[nodiscard]] std::filesystem::path const& directory() const noexcept { return _directory; }

  private:
    /// Whether the private directory has been made by this object.
    enum class DirectoryState : uint8_t
    {
        Absent = 0,
        Created,
    };

    /// Creates the private directory, owner-only, unless this object already has and it is still there.
    [[nodiscard]] std::expected<void, OutputFileError> ensureDirectory();

    /// Removes the private directories beside this one whose processes are gone.
    void sweepAbandonedDirectories() const;

    /// Removes @p path and whatever it holds, logging a failure.
    /// @param path The file or directory.
    /// @param what What it is, for the log.
    void removeLogged(std::filesystem::path const& path, std::string_view what) const;

    core::platform::FileSystem const& _fileSystem;
    ProcessLiveness const& _liveness;
    std::filesystem::path _directory;
    DirectoryState _directoryState = DirectoryState::Absent;
    uint64_t _filesCreated = 0;
    std::set<std::filesystem::path> _files;
    /// Released files the platform would not remove yet, because a process still held them open.
    std::set<std::filesystem::path> _lingering;
};

} // namespace contour::session
