// SPDX-License-Identifier: Apache-2.0
#include <vtpty/CommandLine.hpp>
#include <vtpty/ConPty.hpp>
#include <vtpty/EnvironmentBlock.hpp>
#include <vtpty/Process.hpp>
#include <vtpty/Pty.hpp>
#include <vtpty/SpawnLadder.hpp>
#include <vtpty/WindowsCommandLine.hpp>

#include <core/Assert.hpp>
#include <core/Environment.hpp>
#include <core/Overloaded.hpp>
#include <core/Utf8Path.hpp>
#include <core/WideString.hpp>
#include <core/platform/NativeFileSystem.hpp>

#include <array>
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <numeric>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

#include <Windows.h>
#include <direct.h>

using namespace std;

namespace fs = std::filesystem;

namespace vtpty
{

namespace
{
    /// @return This process's environment, as "NAME=VALUE" entries in UTF-8, the "=C:" ones that say each
    ///         drive's directory among them.
    [[nodiscard]] std::vector<std::string> inheritedEnvironment()
    {
        auto entries = std::vector<std::string> {};
        auto* const block = GetEnvironmentStringsW();
        if (block == nullptr)
            return entries;
        auto const* entry = block;
        while (*entry != L'\0')
        {
            auto const length = std::wcslen(entry);
            entries.push_back(core::toUtf8(std::wstring_view { entry, length }));
            entry += length + 1;
        }
        FreeEnvironmentStringsW(block);
        return entries;
    }

    HRESULT initializeStartupInfoAttachedToPTY(STARTUPINFOEXW& startupInfoEx, ConPty& pty)
    {
        // Initializes the specified startup info struct with the required properties and
        // updates its thread attribute list with the specified ConPTY handle

        HRESULT hr { E_UNEXPECTED };

        size_t attrListSize {};

        startupInfoEx.StartupInfo.cb = sizeof(STARTUPINFOEXW);

        // Standard handles left null, and named as the ones to use, so that the child takes the
        // pseudoconsole's. Without the flag a console child is handed ours whenever they are
        // redirected -- Contour run under ctest, from a script, or as a daemon logging to a file --
        // and writes there, past the terminal. Windows Terminal, alacritty and wezterm do the same.
        startupInfoEx.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;

        // Get the size of the thread attribute list.
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attrListSize);

        // Allocate a thread attribute list of the correct size
        startupInfoEx.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(malloc(attrListSize));

        // Initialize thread attribute list
        if (startupInfoEx.lpAttributeList
            && InitializeProcThreadAttributeList(startupInfoEx.lpAttributeList, 1, 0, &attrListSize))
        {
            // Set Pseudo Console attribute
            hr = UpdateProcThreadAttribute(startupInfoEx.lpAttributeList,
                                           0,
                                           PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                           pty.master(),
                                           sizeof(decltype(pty.master())),
                                           nullptr,
                                           nullptr)
                     ? S_OK
                     : HRESULT_FROM_WIN32(GetLastError());
        }
        else
        {
            hr = HRESULT_FROM_WIN32(GetLastError());

            // Not left allocated but uninitialized: ~Process() takes a list it finds to be one that
            // InitializeProcThreadAttributeList() set up, and deletes it as such.
            free(startupInfoEx.lpAttributeList);
            startupInfoEx.lpAttributeList = nullptr;
        }
        return hr;
    }
} // anonymous namespace

struct Process::Private
{
    string path;
    vector<string> args;
    fs::path cwd;
    Environment env;
    vector<string> removedEnvironment;
    std::unique_ptr<Pty> pty {};
    /// Held for the Process's lifetime only: a ConPTY child has no cgroup to leave.
    std::shared_ptr<ProcessPlacement> placement {};

    mutable std::mutex exitStatusMutex {};
    mutable std::optional<Process::ExitStatus> exitStatus {};
    std::optional<std::thread> exitWatcher {};
    /// Set by ~Process to end exitWatcher's wait without the child: manual-reset, or null when it could
    /// not be made, in which case the watcher waits for the child alone.
    HANDLE stopWatching = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    PROCESS_INFORMATION processInfo {};
    STARTUPINFOEXW startupInfo {};

    [[nodiscard]] optional<Process::ExitStatus> checkStatus(bool waitForExit) const;
};

Process::Process(string const& path,
                 vector<string> const& args,
                 fs::path const& cwd,
                 Environment const& env,
                 vector<string> removedEnvironment,
                 bool escapeSandbox,
                 std::unique_ptr<Pty> pty,
                 std::shared_ptr<ProcessPlacement> placement,
                 HangupGrace hangupGrace):
    _d(new Private { .path = path,
                     .args = args,
                     .cwd = cwd,
                     .env = env,
                     .removedEnvironment = std::move(removedEnvironment),
                     .pty = std::move(pty),
                     .placement = std::move(placement) },
       [](Private* p) { delete p; })
{
    // No sandbox to escape, and no grace to give: closing the pseudoconsole ends the child outright.
    core::ignoreUnused(escapeSandbox, hangupGrace);
}

bool Process::isFlatpak()
{
    return false;
}

StartResult Process::start()
{
    Require(static_cast<ConPty const*>(_d->pty.get()));

    if (auto started = _d->pty->start(); !started)
        return started;

    initializeStartupInfoAttachedToPTY(_d->startupInfo, static_cast<ConPty&>(*_d->pty));

    // The child's environment is handed to it, as on POSIX, rather than set in this process's own for the
    // child to inherit: any other thread reading that meanwhile -- the daemon, a second tab spawning, a log
    // sink -- would read the child's values.
    auto const inherited = inheritedEnvironment();
    auto const environment =
        buildWindowsEnvironment(std::vector<std::string_view> { inherited.begin(), inherited.end() },
                                _d->env,
                                _d->removedEnvironment);
    auto environmentBlock = windowsEnvironmentBlock(environment);
    if (!environmentBlock)
        return std::unexpected(StartFailure { .error = StartError::SpawnFailed,
                                              .detail = "The environment configured for it is not UTF-8." });

    // What runs: the program as configured -- or, where it names no program but holds whitespace
    // ("wsl.exe -d Ubuntu"), the command line it spells -- looked up on the PATH the child is given. A
    // relative path names a file in this process's directory, not the child's: CreateProcess finds the
    // program before the child has a directory of its own.
    auto const searchPath = std::string {
        environmentValue(environment, "PATH", EnvironmentNames::CaseInsensitive).value_or(std::string_view {})
    };
    auto const search = ProgramSearch { .path = searchPath,
                                        .workingDirectory = {},
                                        .fileSystem = core::platform::NativeFileSystem::instance() };
    auto invocation = invocationToRun(_d->path, _d->args, programsIn(search));

    // A bare name runs as found on the child's PATH, as POSIX's execvp() finds it once the child has its
    // environment: CreateProcess would search this process's own PATH, which a configured one is not on.
    if (auto const named = core::pathFromUtf8(invocation.program); named && !named->has_parent_path())
        if (auto found = findProgram(invocation.program, search))
            invocation.program = std::move(*found);

    // Quoted so that a program under "Program Files" is the one that runs, rather than a "C:\Program.exe",
    // and so that the child's C runtime splits an argument holding a space or a double quote back into
    // what it was given.
    auto const cmd = buildWindowsCommandLine(invocation);

    // POSIX walks its fallback ladder in the child, after fork(); with no fork to hide behind, we walk
    // ours here. Each rung gives up strictly more than the one before -- first the working directory,
    // then the command itself -- so a directory the shell cannot be started in (the ERROR_DIRECTORY of
    // issue #1711) costs the user their inherited cwd rather than their new tab.
    auto const loginShellArgs = loginShell(false);
    auto ladder = buildSpawnLadder(
        cmd, core::pathToUtf8(_d->cwd), loginShellArgs.empty() ? std::string {} : loginShellArgs.front());

    // Captured at each failure site: FormatMessageW/LocalFree (inside systemErrorMessage) and the
    // logging below both overwrite the thread's last-error, so reading it once after the loop would
    // report something other than why the spawn failed -- and that reason is what the user reads.
    auto lastError = "No spawn attempt was made."s;

    for (auto& attempt: ladder)
    {
        ptyLog()("Creating process for command line: {}", attempt.commandLine);

        auto commandLine = core::toUtf16(attempt.commandLine);
        auto const workingDirectory = core::toUtf16(attempt.workingDirectory);
        if (!commandLine || !workingDirectory)
        {
            lastError = std::format("The command line or its working directory is not UTF-8: {} in \"{}\".",
                                    attempt.commandLine,
                                    attempt.workingDirectory);
            ptyLog()("Failed to create process: {}", lastError);
            continue;
        }

        // CreateProcessW may write to lpCommandLine in place, so it is handed the string's own
        // writable storage rather than a const_cast of c_str().
        auto const succeeded =
            CreateProcessW(nullptr,             // No module name - use Command Line
                           commandLine->data(), // Command Line
                           nullptr,             // Process handle not inheritable
                           nullptr,             // Thread handle not inheritable
                           FALSE,               // Inherit handles
                           EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT, // Creation flags
                           environmentBlock->data(), // The child's environment
                           workingDirectory->empty() // Empty: use parent's starting directory
                               ? nullptr
                               : workingDirectory->c_str(),
                           &_d->startupInfo.StartupInfo, // Pointer to STARTUPINFO
                           &_d->processInfo);            // Pointer to PROCESS_INFORMATION

        if (!succeeded)
        {
            lastError = core::systemErrorMessage(GetLastError());
            ptyLog()("Failed to create process: {}", lastError);
            continue;
        }

        _d->exitWatcher = std::thread([this]() {
            auto const handles = std::array { _d->processInfo.hProcess, _d->stopWatching };
            auto const count = _d->stopWatching != nullptr ? DWORD { 2 } : DWORD { 1 };
            if (WaitForMultipleObjects(count, handles.data(), FALSE, INFINITE) != WAIT_OBJECT_0)
                return; // told to stop, by a Process going away whose child is still ending
            if (auto const status = wait(); status.has_value())
                ptyLog()("Process terminated with exit code {}.", *status);
            else
                ptyLog()("Process terminated without an exit status.");
            _d->pty->close();
        });

        // The child is running; the only thing left to say is what this rung had to give up to get
        // there, which is empty for the rung that ran exactly what was asked for.
        return StartOutcome { .diagnostic = std::move(attempt.diagnostic) };
    }

    return std::unexpected(StartFailure { .error = StartError::SpawnFailed,
                                          .detail = "Could not create process. "s + std::move(lastError) });
}

void Process::close()
{
    // Closing the pseudoconsole is what ends the child here: the console host sends it the close.
    _d->pty->close();
}

void Process::waitForClosed()
{
    Require(static_cast<ConPty const*>(_d->pty.get()));

    // TODO: Should probably wait for process exit instead here
    _d->pty->waitForClosed();
}

Pty& Process::pty() noexcept
{
    return *_d->pty;
}

Pty const& Process::pty() const noexcept
{
    return *_d->pty;
}

Process::~Process()
{
    // Once its pseudoconsole is closed, the console host ends the child by itself, in the child's own time
    // -- seconds, for one that handles the close -- and there is nothing left to watch for. Waiting for it
    // here would hold up whoever frees the Process, the GUI thread among them. Before that close, the
    // child is waited for, as ever.
    if (_d->stopWatching != nullptr && _d->pty->isClosed())
        SetEvent(_d->stopWatching);
    if (_d->exitWatcher)
        _d->exitWatcher.value().join();
    if (_d->stopWatching != nullptr)
        CloseHandle(_d->stopWatching);

    // Only what start() got as far as creating: a Process that never spawned holds no handles.
    if (_d->processInfo.hThread != nullptr)
        CloseHandle(_d->processInfo.hThread);
    if (_d->processInfo.hProcess != nullptr)
        CloseHandle(_d->processInfo.hProcess);

    if (_d->startupInfo.lpAttributeList != nullptr)
        DeleteProcThreadAttributeList(_d->startupInfo.lpAttributeList);
    free(_d->startupInfo.lpAttributeList);
}

optional<Process::ExitStatus> Process::checkStatus() const
{
    return _d->checkStatus(false);
}

optional<Process::ExitStatus> Process::Private::checkStatus(bool waitForExit) const
{
    {
        auto const _ = lock_guard { exitStatusMutex };
        if (exitStatus.has_value())
            return exitStatus;
    }

    // Without a child there is no status to ask for, and GetExitCodeProcess() would fail on the null
    // handle.
    if (processInfo.hProcess == nullptr)
        return nullopt;

    // The process's handle, not its primary thread's: a process can outlive its first thread.
    if (waitForExit && WaitForSingleObject(processInfo.hProcess, INFINITE) != WAIT_OBJECT_0)
        ptyLog()("WaitForSingleObject() failed: {}", core::systemErrorMessage(GetLastError()));

    // Neither a failed query nor STILL_ACTIVE is an exit, so neither records one -- nor throws, which
    // would leave alive() through its noexcept.
    auto exitCode = DWORD {};
    if (!GetExitCodeProcess(processInfo.hProcess, &exitCode))
    {
        ptyLog()("GetExitCodeProcess() failed: {}", core::systemErrorMessage(GetLastError()));
        return nullopt;
    }
    if (exitCode == STILL_ACTIVE)
        return nullopt;

    auto const _ = lock_guard { exitStatusMutex };
    if (!exitStatus.has_value())
        exitStatus = ExitStatus { NormalExit { static_cast<int>(exitCode) } };
    return exitStatus;
}

bool Process::alive() const noexcept
{
    return _d->processInfo.hProcess != nullptr && !_d->checkStatus(false).has_value();
}

void Process::terminate(TerminationHint terminationHint)
{
    // Windows has no hangup to send: either way the process is ended outright.
    core::ignoreUnused(terminationHint);

    if (!alive())
        return;

    TerminateProcess(_d->processInfo.hProcess, 1);
}

optional<Process::ExitStatus> Process::wait()
{
    return _d->checkStatus(true);
}

vector<string> Process::loginShell(bool escapeSandbox)
{
    core::ignoreUnused(escapeSandbox);

    return { "powershell.exe"s }; // TODO: Find out what the user's default shell is.
}

std::string Process::userName()
{
    if (auto name = core::defaultEnvironment().get("USERNAME"); name && !name->empty())
        return std::move(*name);

    return "unknown"s;
}

fs::path Process::homeDirectory()
{
    if (auto const profile = core::defaultEnvironment().get("USERPROFILE"); profile && !profile->empty())
    {
        if (auto path = core::pathFromUtf8(*profile))
            return std::move(*path);
    }

    return { "/" };
}

string Process::workingDirectory() const
{
    // TODO: Windows
    return "."s;
}

std::optional<std::string> Process::foregroundProcessName() const
{
    // A console has no foreground process group to ask.
    return std::nullopt;
}

} // namespace vtpty
