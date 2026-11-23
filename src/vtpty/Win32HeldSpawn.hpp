// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What every Windows spawn of a program into a held directory shares beyond the directory's own check
/// (Win32HeldDirectory.hpp): the test that a candidate program is a file to run, and the kill-on-close job
/// the child's tree runs in -- and how a pseudoconsole child's job is closed once its console was.
/// PipedProcessRunner and Process::start both spawn that way.

#ifdef _WIN32

    #include <vtpty/UniqueHandle.hpp>
    #include <vtpty/Win32Handle.hpp>

    #include <core/Utf8Path.hpp>
    #include <core/platform/NativeFileSystem.hpp>

    #include <chrono>
    #include <expected>
    #include <string>
    #include <system_error>

    #include <Windows.h>

namespace vtpty
{

/// The limits of a job while its tree is held: closing the job ends the tree, and a process in it that raises
/// an exception nothing handles ends at once rather than waiting on Windows Error Reporting.
inline constexpr auto KillOnCloseJobLimits =
    DWORD { JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION };

/// @param candidate An absolute path, UTF-8.
/// @return Whether @p candidate names a file to run: the ExecutableTest a held-directory program lookup
///         (findWindowsHeldDirectoryProgram) is given.
[[nodiscard]] inline bool isExecutableProgram(std::string const& candidate)
{
    auto const path = core::pathFromUtf8(candidate);
    return path && core::platform::NativeFileSystem::instance().isExecutableFile(*path);
}

/// Sets @p job's limits to @p flags alone.
/// @return Nothing; or why they could not be set.
[[nodiscard]] inline std::expected<void, std::error_code> setJobLimits(HANDLE job, DWORD flags) noexcept
{
    auto limits = JOBOBJECT_EXTENDED_LIMIT_INFORMATION {};
    limits.BasicLimitInformation.LimitFlags = flags;
    if (SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) == FALSE)
        return std::unexpected(lastError());
    return {};
}

/// @return A new job, not inheritable, with KillOnCloseJobLimits; or why it could not be made.
[[nodiscard]] inline std::expected<UniqueHandle, std::error_code> createKillOnCloseJob()
{
    auto job = UniqueHandle { CreateJobObjectW(nullptr, nullptr) };
    if (!job.valid())
        return std::unexpected(lastError());
    return setJobLimits(job.get(), KillOnCloseJobLimits).transform([&job] { return std::move(job); });
}

/// How long a held-directory child's tree outlives its closed console before its job is closed: the time
/// Windows gives a console process to handle CTRL_CLOSE_EVENT.
inline constexpr auto ConsoleCloseGrace = std::chrono::seconds { 5 };

/// Closes @p job once @p process has exited or @p grace has passed, whichever is first, on the thread pool
/// (CreateThreadpoolWait; the callback closes the job and then its own wait object). Owns both handles; where
/// the wait cannot be made, the job is closed at once.
///
/// It outlives whoever called it, on purpose: a Process is freed without waiting for its child (~Process),
/// and the child's console having been closed, its leader is owed the grace to handle that before what it
/// started is ended. Nothing is lost should this process exit first: its exit closes the job's handle all the
/// same, which ends the tree at once.
/// @param job The job to close: a kill-on-close one, so that closing its last handle ends its tree.
/// @param process Its leader, open for SYNCHRONIZE.
/// @param grace How long the leader is given to exit by itself: ConsoleCloseGrace, but for a test.
void closeJobAfter(UniqueHandle job, UniqueHandle process, std::chrono::milliseconds grace);

} // namespace vtpty

#endif
