// SPDX-License-Identifier: Apache-2.0
#include <contour/session/CommandOutputFiles.hpp>
#include <contour/session/Logging.hpp>

#include <core/Utf8Path.hpp>
#include <core/log/LogStore.hpp>

#include <algorithm>
#include <charconv>
#include <format>
#include <optional>
#include <utility>

#ifdef _WIN32
    #include <Windows.h>
#else
    #include <cerrno>
    #include <csignal>
#endif

namespace contour::session
{

namespace fs = std::filesystem;

namespace
{
    /// What every private directory's name starts with; the owning process's id follows.
    constexpr auto DirectoryPrefix = std::string_view { "contour-output-" };

    /// The id of the process that made the private directory named @p name, if it is one.
    [[nodiscard]] std::optional<int64_t> owningProcessId(std::string_view name)
    {
        if (!name.starts_with(DirectoryPrefix))
            return std::nullopt;
        auto const digits = name.substr(DirectoryPrefix.size());
        auto processId = int64_t {};
        auto const [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), processId);
        if (error != std::errc {} || end != digits.data() + digits.size() || processId <= 0)
            return std::nullopt;
        return processId;
    }
} // namespace

fs::path commandOutputDirectory(core::Environment const& environment,
                                vtpty::SandboxInfo const& sandbox,
                                fs::path const& temporaryDirectory,
                                int64_t processId)
{
    auto const leaf = std::format("{}{}", DirectoryPrefix, processId);
    auto const runtime = environment.get("XDG_RUNTIME_DIR");
    if (auto const shared = vtpty::sharedRuntimeDirectory(
            runtime.transform([](std::string const& value) { return std::string_view { value }; }),
            sandbox.applicationId))
        return *shared / leaf;
    if (temporaryDirectory.empty())
        return {};
    return temporaryDirectory / leaf;
}

ProcessState NativeProcessLiveness::stateOf(int64_t processId) const
{
#ifdef _WIN32
    auto* const process =
        OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(processId));
    if (process == nullptr)
        return GetLastError() == ERROR_INVALID_PARAMETER ? ProcessState::Gone : ProcessState::Unknown;
    auto exitCode = DWORD {};
    auto const queried = GetExitCodeProcess(process, &exitCode);
    CloseHandle(process);
    if (queried == 0)
        return ProcessState::Unknown;
    return exitCode == STILL_ACTIVE ? ProcessState::Running : ProcessState::Gone;
#else
    if (::kill(static_cast<pid_t>(processId), 0) == 0)
        return ProcessState::Running;
    switch (errno)
    {
        case ESRCH: return ProcessState::Gone;
        case EPERM: return ProcessState::Running; // there, and somebody else's
        default: return ProcessState::Unknown;
    }
#endif
}

CommandOutputFiles::CommandOutputFiles(core::platform::FileSystem const& fileSystem,
                                       ProcessLiveness const& liveness,
                                       fs::path directory):
    _fileSystem { fileSystem }, _liveness { liveness }, _directory { std::move(directory) }
{
}

CommandOutputFiles::~CommandOutputFiles()
{
    if (_directoryState == DirectoryState::Created)
        removeLogged(_directory, "the command output directory");
}

void CommandOutputFiles::removeLogged(fs::path const& path, std::string_view what) const
{
    if (auto const removed = _fileSystem.removeAll(path); !removed)
        errorLog()("Could not remove {}: {}", what, removed.error());
}

void CommandOutputFiles::sweepAbandonedDirectories() const
{
    auto const entries = _fileSystem.listDirectory(_directory.parent_path());
    if (!entries)
        return;

    for (auto const& entry: *entries)
    {
        if (entry.path == _directory || entry.isSymlink || !entry.isDirectory)
            continue;
        // Kept whenever in doubt: a directory whose process may still run may still be read.
        if (auto const owner = owningProcessId(core::pathToUtf8(entry.path.filename()));
            owner && _liveness.stateOf(*owner) == ProcessState::Gone)
            removeLogged(entry.path, "an abandoned command output directory");
    }
}

std::expected<void, OutputFileError> CommandOutputFiles::ensureDirectory()
{
    // Removed behind this object's back -- a temporary-file cleaner, tmpfiles ageing, a user's rm -rf --
    // the directory is made again, and what it held is gone with it.
    if (_directoryState == DirectoryState::Created && !_fileSystem.exists(_directory))
    {
        _directoryState = DirectoryState::Absent;
        _files.clear();
        _lingering.clear();
    }

    if (_directoryState == DirectoryState::Created)
        return {};
    if (_directory.empty())
        return std::unexpected(OutputFileError::DirectoryUnavailable);

    // Never follow or reuse a link: whoever placed it would be handed the output.
    if (_fileSystem.isSymlink(_directory))
        return std::unexpected(OutputFileError::DirectoryUnavailable);

    // A crash skips the removal at exit, and output may hold secrets: what such a process left is
    // reclaimed here, by the next one.
    sweepAbandonedDirectories();

    // A directory one cannot remove is somebody else's, and is not used.
    if (_fileSystem.exists(_directory))
        if (auto const removed = _fileSystem.removeAll(_directory); !removed)
            return std::unexpected(OutputFileError::DirectoryUnavailable);

    // Creating refuses whatever appeared in its place since the checks above, link or not.
    if (auto const created = _fileSystem.createDirectory(_directory); !created)
        return std::unexpected(OutputFileError::DirectoryUnavailable);

    if (auto const restricted = _fileSystem.setPermissions(_directory, fs::perms::owner_all); !restricted)
    {
        removeLogged(_directory, "the command output directory");
        return std::unexpected(OutputFileError::PermissionsFailed);
    }

    _directoryState = DirectoryState::Created;
    return {};
}

std::expected<fs::path, OutputFileError> CommandOutputFiles::create(std::string_view content)
{
    if (auto const ready = ensureDirectory(); !ready)
        return std::unexpected(ready.error());

    // Whatever an earlier reader has let go of since its release goes now.
    std::erase_if(_lingering, [this](fs::path const& file) { return _fileSystem.remove(file).has_value(); });

    // The directory admits only its owner, so the moment between writing the file and narrowing its
    // own permissions exposes nothing.
    auto file = _directory / std::format("output-{}.txt", ++_filesCreated);
    if (auto const written = _fileSystem.writeFile(file, content); !written)
        return std::unexpected(OutputFileError::WriteFailed);

    if (auto const restricted =
            _fileSystem.setPermissions(file, fs::perms::owner_read | fs::perms::owner_write);
        !restricted)
    {
        removeLogged(file, "a command output file");
        return std::unexpected(OutputFileError::PermissionsFailed);
    }

    _files.insert(file);
    return file;
}

void CommandOutputFiles::release(fs::path const& file)
{
    if (_files.erase(file) == 0)
        return;
    if (auto const removed = _fileSystem.remove(file); !removed)
    {
        // Expected on Windows for a detached program's input, which it holds open while it runs.
        sessionLog()("Command output file kept until its reader lets go: {}", removed.error());
        _lingering.insert(file);
    }
}

} // namespace contour::session
