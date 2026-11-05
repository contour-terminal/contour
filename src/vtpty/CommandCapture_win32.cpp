// SPDX-License-Identifier: Apache-2.0
#include <vtpty/CommandCapture.hpp>
#include <vtpty/CommandCaptureStop.hpp>
#include <vtpty/WindowsHandle.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <Windows.h>

namespace vtpty
{

namespace
{
    /// Size of the stdout pipe's buffer; large so a chatty command rarely waits for us to read.
    constexpr auto PipeBufferSize = DWORD { 1024 * 1024 };

    /// How long a killed command may take to disappear before run() stops waiting for it.
    constexpr auto KillGracePeriod = DWORD { 1000 };

    /// Granularity at which waits re-check the stop token and the deadline.
    constexpr auto PollSlice = DWORD { 10 };

    [[nodiscard]] std::unexpected<CommandCaptureError> spawnFailed(std::error_code cause)
    {
        return std::unexpected(CommandCaptureError { CommandCaptureError::Kind::SpawnFailed, cause });
    }

    /// @param utf8 UTF-8 text.
    /// @return @p utf8 as UTF-16, or std::nullopt when it is not valid UTF-8.
    [[nodiscard]] std::optional<std::wstring> toWide(std::string_view utf8)
    {
        if (utf8.empty())
            return std::wstring {};
        auto const length = MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
        if (length <= 0)
            return std::nullopt;
        auto result = std::wstring(static_cast<size_t>(length), L'\0');
        MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), result.data(), length);
        return result;
    }

    using Clock = std::chrono::steady_clock;

    [[nodiscard]] DWORD millisecondsUntil(Clock::time_point deadline)
    {
        auto const remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        return remaining.count() <= 0 ? DWORD { 0 } : static_cast<DWORD>(remaining.count());
    }

    /// A running shell, its stdout and the job object that owns it and its descendants.
    struct Spawned
    {
        UniqueHandle job;     ///< Kills every process in it when closed.
        UniqueHandle process; ///< The shell.
        UniqueHandle output;  ///< Read end of the shell's stdout.
    };

    /// Starts the shell with @p commandLine inside a kill-on-close job, stdout piped to us.
    [[nodiscard]] std::expected<Spawned, std::error_code> spawnInJob(std::wstring commandLine)
    {
        auto readEnd = UniqueHandle {};
        auto writeEnd = UniqueHandle {};
        {
            auto read = HANDLE { nullptr };
            auto write = HANDLE { nullptr };
            if (!CreatePipe(&read, &write, nullptr, PipeBufferSize))
                return std::unexpected(lastError());
            readEnd = UniqueHandle { read };
            writeEnd = UniqueHandle { write };
        }
        // Only the child's ends are inheritable, narrowing what a concurrent spawn could pick up.
        if (!SetHandleInformation(writeEnd.get(), HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT))
            return std::unexpected(lastError());

        auto inheritable = SECURITY_ATTRIBUTES { sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
        auto nul = UniqueHandle { CreateFileW(L"NUL",
                                              GENERIC_READ | GENERIC_WRITE,
                                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                                              &inheritable,
                                              OPEN_EXISTING,
                                              0,
                                              nullptr) };
        if (!nul.valid())
            return std::unexpected(lastError());

        auto inheritedHandles = std::array<HANDLE, 2> { writeEnd.get(), nul.get() };
        auto attributeList = AttributeList::create(1);
        if (!attributeList)
            return std::unexpected(attributeList.error());
        if (auto const inherited = attributeList->inheritOnly(inheritedHandles); !inherited)
            return std::unexpected(inherited.error());

        auto startupInfo = STARTUPINFOEXW {};
        startupInfo.StartupInfo.cb = sizeof(STARTUPINFOEXW);
        startupInfo.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startupInfo.StartupInfo.hStdInput = nul.get();
        startupInfo.StartupInfo.hStdOutput = writeEnd.get();
        startupInfo.StartupInfo.hStdError = nul.get();
        startupInfo.lpAttributeList = attributeList->get();

        auto job = UniqueHandle { CreateJobObjectW(nullptr, nullptr) };
        if (!job.valid())
            return std::unexpected(lastError());
        auto limits = JOBOBJECT_EXTENDED_LIMIT_INFORMATION {};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
            return std::unexpected(lastError());

        auto processInfo = PROCESS_INFORMATION {};
        if (!CreateProcessW(nullptr,
                            commandLine.data(),
                            nullptr,
                            nullptr,
                            TRUE,
                            CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
                            nullptr,
                            nullptr,
                            &startupInfo.StartupInfo,
                            &processInfo))
            return std::unexpected(lastError());

        auto process = UniqueHandle { processInfo.hProcess };
        auto thread = UniqueHandle { processInfo.hThread };
        if (!AssignProcessToJobObject(job.get(), process.get()) || ResumeThread(thread.get()) == DWORD(-1))
        {
            auto const cause = lastError();
            TerminateProcess(process.get(), 1);
            return std::unexpected(cause);
        }
        // Our copies of the child's ends are closed on return; otherwise the pipe never reports EOF.
        return Spawned { std::move(job), std::move(process), std::move(readEnd) };
    }

    /// @param pipe Read end of a pipe.
    /// @return How many bytes can be read now, or std::nullopt once the pipe is broken (EOF).
    [[nodiscard]] std::optional<DWORD> bytesAvailable(HANDLE pipe) noexcept
    {
        auto available = DWORD { 0 };
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr))
            return std::nullopt; // ERROR_BROKEN_PIPE is EOF; any other failure ends the read the same way.
        return available;
    }

    /// @return Why the run must be abandoned (stop requested or deadline passed), if it must.
    [[nodiscard]] std::optional<CommandCaptureError::Kind> abandonReason(RunStop const& stop,
                                                                         Clock::time_point deadline)
    {
        if (stop.requested())
            return CommandCaptureError::Kind::Cancelled;
        if (Clock::now() >= deadline)
            return CommandCaptureError::Kind::TimedOut;
        return std::nullopt;
    }

    /// Reads the shell's stdout to EOF, keeping at most MaxCapturedOutput bytes, then waits for it to exit.
    [[nodiscard]] std::expected<std::string, CommandCaptureError::Kind> drain(Spawned const& spawned,
                                                                              Clock::time_point deadline,
                                                                              RunStop const& stop)
    {
        auto captured = std::string {};
        auto buffer = std::array<char, CaptureReadChunkSize> {};
        auto available = bytesAvailable(spawned.output.get());
        while (available)
        {
            if (auto const reason = abandonReason(stop, deadline))
                return std::unexpected(*reason);

            if (*available == 0)
            {
                // Wakes early when the shell exits, but a grandchild may still write to the pipe, so
                // keep peeking until it breaks. An exited process makes the wait return at once; yield.
                if (WaitForSingleObject(spawned.process.get(), PollSlice) == WAIT_OBJECT_0)
                    Sleep(1);
            }
            else
            {
                auto read = DWORD { 0 };
                auto const want = std::min(*available, static_cast<DWORD>(buffer.size()));
                if (!ReadFile(spawned.output.get(), buffer.data(), want, &read, nullptr) || read == 0)
                    break;
                if (captured.size() < MaxCapturedOutput) // beyond the cap bytes are drained and dropped
                    captured.append(buffer.data(),
                                    std::min<size_t>(read, MaxCapturedOutput - captured.size()));
            }
            available = bytesAvailable(spawned.output.get());
        }

        while (WaitForSingleObject(spawned.process.get(), std::min(PollSlice, millisecondsUntil(deadline)))
               == WAIT_TIMEOUT)
        {
            if (auto const reason = abandonReason(stop, deadline))
                return std::unexpected(*reason);
        }
        return captured;
    }

    /// Runs @p commandLine inside a kill-on-close job and waits for it, at most @p timeout.
    /// @param commandLine What CreateProcess() runs; nullopt when it could not be spelled in UTF-16.
    /// @param timeout Upper bound on the run.
    /// @param stop What cancels the run.
    /// @return The first line of its standard output, or why there is none.
    [[nodiscard]] std::expected<std::string, CommandCaptureError> captureOutput(
        std::optional<std::wstring> commandLine, std::chrono::milliseconds timeout, RunStop const& stop)
    {
        // A run after shutdown never spawns anything.
        if (stop.requested())
            return std::unexpected(CommandCaptureError { CommandCaptureError::Kind::Cancelled, {} });
        if (!commandLine)
            return spawnFailed(std::make_error_code(std::errc::illegal_byte_sequence));
        auto const deadline = Clock::now() + timeout;

        auto spawned = spawnInJob(*std::move(commandLine));
        if (!spawned)
            return spawnFailed(spawned.error());

        auto const captured = drain(*spawned, deadline, stop);
        if (!captured)
        {
            // TerminateJobObject is asynchronous: wait (bounded) so the command is gone when we return.
            TerminateJobObject(spawned->job.get(), 1);
            WaitForSingleObject(spawned->process.get(), KillGracePeriod);
            return std::unexpected(CommandCaptureError { captured.error(), {} });
        }
        // Closing the job handle on return kills any surviving grandchildren.
        return firstLineOf(*captured);
    }
} // namespace

std::expected<std::string, CommandCaptureError> SystemCommandCapture::run(std::string_view command,
                                                                          std::chrono::milliseconds timeout,
                                                                          core::async::StopToken stop)
{
    auto const wideShell = toWide(_shell);
    auto const wideCommand = toWide(command);
    auto commandLine = wideShell && wideCommand
                           ? std::optional { L"\"" + *wideShell + L"\" /c " + *wideCommand }
                           : std::nullopt;
    return captureOutput(std::move(commandLine), timeout, RunStop { std::move(stop), _shutdown });
}

ProgramCommandCapture::ProgramCommandCapture(core::async::StopToken shutdown):
    _shutdown { std::move(shutdown) }
{
}

std::expected<std::string, CommandCaptureError> ProgramCommandCapture::run(std::string_view command,
                                                                           std::chrono::milliseconds timeout,
                                                                           core::async::StopToken stop)
{
    return captureOutput(toWide(command), timeout, RunStop { std::move(stop), _shutdown });
}

} // namespace vtpty
