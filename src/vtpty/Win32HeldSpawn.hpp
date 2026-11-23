// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What every Windows spawn of a program into a held directory shares beyond the directory's own check
/// (Win32HeldDirectory.hpp): the test that a candidate program is a file to run, and the kill-on-close job
/// the child's tree runs in. PipedProcessRunner and Process::start both spawn that way.

#ifdef _WIN32

    #include <vtpty/UniqueHandle.hpp>
    #include <vtpty/Win32Handle.hpp>

    #include <core/Utf8Path.hpp>
    #include <core/platform/NativeFileSystem.hpp>

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

} // namespace vtpty

#endif
