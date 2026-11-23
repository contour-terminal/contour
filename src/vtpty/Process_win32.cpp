// SPDX-License-Identifier: Apache-2.0
#include <vtpty/CommandLine.hpp>
#include <vtpty/ConPty.hpp>
#include <vtpty/EnvironmentBlock.hpp>
#include <vtpty/Process.hpp>
#include <vtpty/Pty.hpp>
#include <vtpty/SpawnLadder.hpp>
#include <vtpty/UniqueHandle.hpp>
#include <vtpty/Win32Handle.hpp>
#include <vtpty/Win32HeldDirectory.hpp>
#include <vtpty/Win32HeldSpawn.hpp>
#include <vtpty/WindowsCommandLine.hpp>
#include <vtpty/WindowsProcessRules.hpp>

#include <core/Assert.hpp>
#include <core/Environment.hpp>
#include <core/Overloaded.hpp>
#include <core/Utf8Path.hpp>
#include <core/WideString.hpp>
#include <core/platform/NativeFileSystem.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <ranges>
#include <ratio>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
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

    /// The exit code a terminated held-directory child's tree is given.
    constexpr auto TerminatedExitCode = UINT { 1 };

    /// How a held-directory child is created: suspended, so that it is in its job before it runs a single
    /// instruction; on its pseudoconsole; with a UTF-16 environment block.
    constexpr auto HeldCreationFlags =
        DWORD { EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED };

    /// Why a held directory did not become a child's current directory, as the detail StartFailure carries,
    /// the directory's resolved path in place of the `{}`: one row per HeldDirectoryEntryError, in its order.
    constexpr auto HeldEntryDetails = std::to_array<std::pair<HeldDirectoryEntryError, std::string_view>>({
        { HeldDirectoryEntryError::Unbacked, R"(The directory "{}" cannot be entered by its handle here.)" },
        { HeldDirectoryEntryError::Removed, R"(The directory "{}" was removed.)" },
        { HeldDirectoryEntryError::Replaced, R"(The directory "{}" is no longer the one that was opened.)" },
        { HeldDirectoryEntryError::TooLong, R"(The directory "{}" is too long to start a program in.)" },
        { HeldDirectoryEntryError::NotUtf8, R"(The directory "{}" is not UTF-8.)" },
        { HeldDirectoryEntryError::Unguardable,
          R"(The directory "{}" is open elsewhere in a way that lets it be renamed or removed.)" },
    });
    static_assert(HeldEntryDetails.size() == std::to_underlying(HeldDirectoryEntryError::Last) + 1U
                      && std::ranges::all_of(std::views::iota(std::size_t { 0 }, HeldEntryDetails.size()),
                                             [](std::size_t index) {
                                                 return std::to_underlying(HeldEntryDetails.at(index).first)
                                                        == index;
                                             }),
                  "HeldEntryDetails has one row per HeldDirectoryEntryError, in its order");

    /// @return What a start refused by @p error says, of the held directory at @p path.
    [[nodiscard]] std::string heldEntryDetail(HeldDirectoryEntryError error, std::string const& path)
    {
        return std::vformat(HeldEntryDetails.at(std::to_underlying(error)).second,
                            std::make_format_args(path));
    }

    /// @return A start that failed for @p detail.
    [[nodiscard]] std::unexpected<StartFailure> spawnFailed(std::string detail)
    {
        return std::unexpected(
            StartFailure { .error = StartError::SpawnFailed, .detail = std::move(detail) });
    }

    /// How long a held-directory child's tree outlives its closed console before its job is closed: the time
    /// Windows gives a console process to handle CTRL_CLOSE_EVENT.
    constexpr auto ConsoleCloseGrace = std::chrono::seconds { 5 };

    /// What a deferred close of a job owns until it runs: the job, and the process whose exit ends the wait.
    struct DeferredJobClose
    {
        UniqueHandle job;     ///< Closed when the wait ends, ending whatever still runs in it.
        UniqueHandle process; ///< The job's leader, waited for.
    };

    /// The thread pool's callback of closeJobAfter(): the leader exited or the grace passed, either way the
    /// job is closed now, and then the wait object that called it.
    /// @param context The DeferredJobClose, owned from here on.
    /// @param wait The wait object, closed last.
    void CALLBACK closeDeferredJob(PTP_CALLBACK_INSTANCE /*instance*/,
                                   PVOID context,
                                   PTP_WAIT wait,
                                   TP_WAIT_RESULT /*result*/)
    {
        auto pending = std::unique_ptr<DeferredJobClose> { static_cast<DeferredJobClose*>(context) };
        pending.reset(); // the job first, ending what still runs in it
        // Freed once this callback has returned: closing a wait object from its own callback is allowed.
        CloseThreadpoolWait(wait);
    }

    /// Closes @p job once @p process has exited or @p grace has passed, whichever is first, on the thread
    /// pool (CreateThreadpoolWait; the callback closes the job and then its own wait object). Owns both
    /// handles.
    ///
    /// It outlives the Process that called it, on purpose: a Process is freed without waiting for its child
    /// (~Process), and the child's console having been closed, its leader is owed the grace to handle that
    /// before what it started is ended. Nothing is lost should this process exit first: its exit closes the
    /// job's handle all the same, which ends the tree at once.
    /// @param job The job to close: a kill-on-close one, so that closing its last handle ends its tree.
    /// @param process Its leader, open for SYNCHRONIZE.
    /// @param grace How long the leader is given to exit by itself.
    void closeJobAfter(UniqueHandle job, UniqueHandle process, std::chrono::milliseconds grace)
    {
        auto pending = std::make_unique<DeferredJobClose>(std::move(job), std::move(process));
        auto* const wait = CreateThreadpoolWait(&closeDeferredJob, pending.get(), nullptr);
        if (wait == nullptr)
        {
            ptyLog()("Could not defer closing a job; it is closed now: {}",
                     core::systemErrorMessage(GetLastError()));
            return; // pending goes, and the job with it
        }
        // Relative, in 100 ns units: a negative due time.
        using FileTimeTicks = std::chrono::duration<std::int64_t, std::ratio<1, 10'000'000>>;
        auto const due =
            static_cast<std::uint64_t>(-std::chrono::duration_cast<FileTimeTicks>(grace).count());
        auto dueTime = FILETIME { .dwLowDateTime = static_cast<DWORD>(due & 0xFFFF'FFFFU),
                                  .dwHighDateTime = static_cast<DWORD>(due >> 32U) };
        SetThreadpoolWait(wait, pending->process.get(), &dueTime);
        std::ignore = pending.release(); // the callback's from here on
    }
} // anonymous namespace

struct Process::Private
{
    // Declared first, so destroyed last: the guard outlives the job, whose closing ends the tree it guards.

    /// A held-directory child's directory, guarded so that no other directory takes the name it was started
    /// at (see HeldDirectoryEntry): released once the child has ended. None for a child started otherwise.
    std::optional<HeldDirectoryEntry> heldDirectory {};
    /// A held-directory child's kill-on-close job, its whole tree; not inheritable. None for a user's shell,
    /// whose `code .` or `explorer .` must outlive its tab.
    UniqueHandle job {};

    ExecInfo exec;
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

    /// Watches the child for its exit, on a thread of its own, and closes its pseudoconsole once it has:
    /// for a spawned child and an adopted one alike.
    /// @param self The Process this is of.
    void watchExit(Process& self);

    /// Starts exec in its held directory, exec.directory, or nothing at all: no ladder to fall down.
    ///
    /// The program is the .exe findWindowsHeldDirectoryProgram() finds on the absolute entries of the PATH
    /// the child is given -- never a batch file, and never one in the directory -- started by that path, with
    /// exec.program as its argv[0]. The directory is entered by its resolved path, guarded and checked by
    /// prepareHeldDirectoryEntry(), and checked again while the child is still suspended. The child inherits
    /// no handle but its pseudoconsole, and runs in a kill-on-close job of its own.
    /// @param self The Process this is of.
    /// @return An outcome with nothing given up; or why nothing was started.
    [[nodiscard]] StartResult startInHeldDirectory(Process& self);
};

void Process::Private::watchExit(Process& self)
{
    exitWatcher = std::thread([this, &self]() {
        auto const handles = std::array { processInfo.hProcess, stopWatching };
        auto const count = stopWatching != nullptr ? DWORD { 2 } : DWORD { 1 };
        if (WaitForMultipleObjects(count, handles.data(), FALSE, INFINITE) != WAIT_OBJECT_0)
            return; // told to stop, by a Process going away whose child is still ending, or giving it up
        if (auto const status = self.wait(); status.has_value())
            ptyLog()("Process terminated with exit code {}.", *status);
        else
            ptyLog()("Process terminated without an exit status.");
        heldDirectory.reset(); // the child is gone: its directory's name need be kept no longer
        pty->close();
    });
}

StartResult Process::Private::startInHeldDirectory(Process& self)
{
    auto const& held = *exec.directory;
    auto entry = prepareHeldDirectoryEntry(held);
    if (!entry)
        return spawnFailed(heldEntryDetail(entry.error(), held.resolvedPath()));

    // The environment the child is given, and the program found on its PATH, names read as Windows reads
    // them.
    auto const inherited = inheritedEnvironment();
    auto const environment =
        buildWindowsEnvironment(std::vector<std::string_view> { inherited.begin(), inherited.end() },
                                exec.env,
                                exec.removedEnvironment);
    auto environmentBlock = windowsEnvironmentBlock(environment);
    if (!environmentBlock)
        return spawnFailed("The environment configured for it is not UTF-8.");
    auto const searchPath = environmentValue(environment, "PATH", EnvironmentNames::CaseInsensitive)
                                .value_or(std::string_view {});
    auto const program = findWindowsHeldDirectoryProgram(exec.program, searchPath, isExecutableProgram);
    if (!program)
        return spawnFailed(std::format(R"(The program "{}" was not found.)", exec.program));

    // argv[0] as declared, never read as a command line of its own: it is one token of a manifest's argv.
    auto const programPath = core::toUtf16(*program);
    auto commandLine = core::toUtf16(buildWindowsCommandLine(exec.program, exec.arguments));
    if (!programPath || !commandLine)
        return spawnFailed("The program or its arguments are not UTF-8.");
    if (commandLine->size() > MaxWindowsCommandLineUnits)
        return spawnFailed(
            std::format("The command line is {} UTF-16 units long, longer than the {} Windows takes.",
                        commandLine->size(),
                        MaxWindowsCommandLineUnits));

    auto newJob = createKillOnCloseJob();
    if (!newJob)
        return spawnFailed(std::format("Could not create the job it runs in: {}", newJob.error().message()));

    if (auto started = pty->start(); !started)
        return started;
    if (auto const attached = initializeStartupInfoAttachedToPTY(startupInfo, static_cast<ConPty&>(*pty));
        FAILED(attached))
        return spawnFailed(std::format("Could not attach it to its pseudoconsole: {}",
                                       std::system_category().message(attached)));

    // Nothing inherited: with bInheritHandles FALSE no handle of this process reaches the child, whatever is
    // inheritable here, and its console comes from the pseudoconsole attribute.
    auto created = PROCESS_INFORMATION {};
    if (CreateProcessW(programPath->c_str(),
                       commandLine->data(),
                       nullptr,
                       nullptr,
                       FALSE,
                       HeldCreationFlags,
                       environmentBlock->data(),
                       entry->path.c_str(),
                       &startupInfo.StartupInfo,
                       &created)
        == FALSE)
        return spawnFailed("Could not create process. " + core::systemErrorMessage(GetLastError()));
    auto process = UniqueHandle { created.hProcess };
    auto thread = UniqueHandle { created.hThread };

    // The child is suspended: what fails from here on ends it before it ran.
    auto const kill = [&process](std::string detail) {
        TerminateProcess(process.get(), TerminatedExitCode);
        return spawnFailed(std::move(detail));
    };
    // Looked at again with the child suspended, redundantly while the guard holds the name: cheap.
    if (auto const confirmed = confirmHeldDirectoryEntry(*entry); !confirmed)
        return kill(heldEntryDetail(confirmed.error(), held.resolvedPath()));
    if (AssignProcessToJobObject(newJob->get(), process.get()) == FALSE)
        return kill("Could not put it in its job. " + core::systemErrorMessage(GetLastError()));
    if (ResumeThread(thread.get()) == static_cast<DWORD>(-1))
        return kill("Could not resume it. " + core::systemErrorMessage(GetLastError()));

    processInfo = created;
    processInfo.hProcess = process.release();
    processInfo.hThread = thread.release();
    job = std::move(*newJob);
    heldDirectory = std::move(*entry);
    watchExit(self);
    return StartOutcome {};
}

Process::Process(ExecInfo exe,
                 std::unique_ptr<Pty> pty,
                 SandboxEscape sandbox,
                 std::shared_ptr<ProcessPlacement> placement,
                 HangupGrace hangupGrace):
    _d(new Private { .exec = std::move(exe), .pty = std::move(pty), .placement = std::move(placement) },
       [](Private* p) { delete p; })
{
    // No sandbox to escape, and no grace to give: closing the pseudoconsole ends the child outright.
    core::ignoreUnused(sandbox, hangupGrace);
}

bool Process::isFlatpak()
{
    return false;
}

ProgramHost Process::programHost(SandboxEscape escape)
{
    return programHostFor(SandboxState::Host, escape); // No sandbox to escape on Windows.
}

HeldDirectorySpawn Process::heldDirectorySpawn(SandboxEscape /*escape*/)
{
    // No sandbox to escape on Windows: a held directory is entered by its path, verified against the handle
    // before and after the child is created (startInHeldDirectory()).
    return HeldDirectorySpawn::Supported;
}

bool Process::findsProgram(ExecInfo const& exec)
{
    // The child's PATH as start() assembles it; a relative path names a file in this process's directory.
    auto const inherited = inheritedEnvironment();
    auto const environment =
        buildWindowsEnvironment(std::vector<std::string_view> { inherited.begin(), inherited.end() },
                                exec.env,
                                exec.removedEnvironment);
    auto const searchPath = std::string {
        environmentValue(environment, "PATH", EnvironmentNames::CaseInsensitive).value_or(std::string_view {})
    };
    // A held directory's program is looked up as startInHeldDirectory() looks it up: an .exe on an absolute
    // entry of that PATH, or nothing -- so a program its spawn would refuse is named missing beforehand.
    if (exec.directory)
        return findWindowsHeldDirectoryProgram(exec.program, searchPath, isExecutableProgram).has_value();
    return namesProgram(exec.program,
                        ProgramSearch { .path = searchPath,
                                        .workingDirectory = {},
                                        .fileSystem = core::platform::NativeFileSystem::instance() });
}

StartResult Process::start()
{
    Require(static_cast<ConPty const*>(_d->pty.get()));
    if (_d->processInfo.hProcess != nullptr)
        return {}; // Adopted: running already, on a pseudoconsole that is open.

    // In its directory or not at all: none of the ladder below, which gives up the directory, then the
    // program.
    if (_d->exec.directory)
        return _d->startInHeldDirectory(*this);

    if (auto started = _d->pty->start(); !started)
        return started;

    initializeStartupInfoAttachedToPTY(_d->startupInfo, static_cast<ConPty&>(*_d->pty));

    // The child's environment is handed to it, as on POSIX, rather than set in this process's own for the
    // child to inherit: any other thread reading that meanwhile -- the daemon, a second tab spawning, a log
    // sink -- would read the child's values.
    auto const inherited = inheritedEnvironment();
    auto const environment =
        buildWindowsEnvironment(std::vector<std::string_view> { inherited.begin(), inherited.end() },
                                _d->exec.env,
                                _d->exec.removedEnvironment);
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
    auto invocation = invocationToRun(_d->exec.program, _d->exec.arguments, programsIn(search));

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
    auto ladder = buildSpawnLadder(cmd,
                                   core::pathToUtf8(_d->exec.workingDirectory),
                                   loginShellArgs.empty() ? std::string {} : loginShellArgs.front());

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

        _d->watchExit(*this);

        // The child is running; the only thing left to say is what this rung had to give up to get
        // there, which is empty for the rung that ran exactly what was asked for.
        return StartOutcome { .diagnostic = std::move(attempt.diagnostic) };
    }

    return std::unexpected(StartFailure { .error = StartError::SpawnFailed,
                                          .detail = "Could not create process. "s + std::move(lastError) });
}

std::expected<void, AdoptError> Process::canAdopt(AdoptedChild const& child)
{
    // Only a handle this process holds can be waited on; and only to the process it was handed over as, which
    // also rejects a handle value that names something else here.
    if (child.processHandle.isNone() || child.pid <= 0
        || std::cmp_not_equal(GetProcessId(child.processHandle.native()), child.pid))
        return std::unexpected(AdoptError::NoSuchChild);
    return {};
}

std::expected<std::unique_ptr<Process>, AdoptError> Process::adopt(AdoptedChild const& child,
                                                                   std::unique_ptr<Pty> pty)
{
    Require(pty != nullptr);
    if (auto const adoptable = canAdopt(child); !adoptable)
        return std::unexpected(adoptable.error());

    auto process = std::make_unique<Process>(child.executable,
                                             child.args,
                                             child.cwd,
                                             Environment {},
                                             std::vector<std::string> {},
                                             SandboxEscape::Contained,
                                             std::move(pty),
                                             std::make_shared<NoPlacement>(),
                                             child.hangupGrace);
    auto* const handle = child.processHandle.native();
    // Crossed for this child alone: no program this process starts inherits it.
    if (auto const set = setInheritance(handle, HandleInheritance::NotInherited); !set)
        ptyLog()("An adopted child's handle stays inheritable: {}", set.error().message());
    process->_d->processInfo.hProcess = handle;
    process->_d->processInfo.dwProcessId = static_cast<DWORD>(child.pid);
    // Its tree's job, where it runs in one: this Process ends it, as the one that started the child did. No
    // guard of its directory comes with it, nor is one needed: by now the child's loader has long made the
    // directory its current one, which Windows holds against a rename as the guard did.
    if (!child.job.isNone())
    {
        auto* const job = child.job.native();
        if (auto const set = setInheritance(job, HandleInheritance::NotInherited); !set)
            ptyLog()("An adopted child's job stays inheritable: {}", set.error().message());
        process->_d->job = UniqueHandle { job };
    }
    process->_d->watchExit(*process);
    return process;
}

std::optional<AdoptedChild> Process::handoverChild() const
{
    // One that exited is handed over all the same, as POSIX hands over one not reaped yet: its handle still
    // names it, and the adopter learns of its exit as it starts watching.
    if (_d->processInfo.hProcess == nullptr)
        return nullopt;
    return AdoptedChild { .pid = static_cast<std::int64_t>(_d->processInfo.dwProcessId),
                          .executable = _d->exec.program,
                          .args = _d->exec.arguments,
                          .cwd = _d->exec.workingDirectory,
                          .hangupGrace = {},
                          .processHandle = CrossingHandle::of(_d->processInfo.hProcess),
                          .job = CrossingHandle::of(_d->job.get()) };
}

std::optional<AdoptedChild> Process::releaseChild()
{
    auto child = handoverChild();
    // Its watcher stops without the child, and nothing here waits for, ends or closes the child from now on:
    // the handles to it and to its job are the caller's, so that neither terminate() nor ~Process ends a tree
    // this Process no longer owns.
    if (_d->exitWatcher)
    {
        if (_d->stopWatching != nullptr)
            SetEvent(_d->stopWatching);
        _d->exitWatcher->join();
        _d->exitWatcher.reset();
    }
    if (child)
    {
        _d->processInfo.hProcess = nullptr;
        std::ignore = _d->job.release();
    }
    // The guard of its directory stays here until this Process goes. It ends nothing -- it only keeps the
    // directory's name -- and cannot cross: by the time a child is given up, its loader has long made the
    // directory its current one, which Windows holds against a rename as the guard does.
    return child;
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

    // A held-directory child's tree: once its leader exited, or while its console is open (the leader was
    // waited for above), the job closes with Private, ending any straggler at once. A leader still running on
    // a closed console is owed the time to handle that close, which no one here waits for: its job is closed
    // after the grace, or as soon as the leader exits.
    if (_d->job.valid() && _d->pty->isClosed() && _d->processInfo.hProcess != nullptr
        && !_d->checkStatus(false).has_value())
    {
        auto leader = HANDLE {};
        if (DuplicateHandle(GetCurrentProcess(),
                            _d->processInfo.hProcess,
                            GetCurrentProcess(),
                            &leader,
                            SYNCHRONIZE,
                            FALSE,
                            0)
            != FALSE)
            closeJobAfter(std::move(_d->job), UniqueHandle { leader }, ConsoleCloseGrace);
        else
            ptyLog()("Could not wait for a closed console's child; its job is closed now: {}",
                     core::systemErrorMessage(GetLastError()));
    }

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

Process::SpawnProgress Process::spawnProgress() const
{
    // CreateProcess() returns with the child attached to its pseudo console, which flushes nothing later.
    return SpawnProgress::Settled;
}

void Process::terminate(TerminationHint terminationHint)
{
    // Windows has no hangup to send: either way the process is ended outright.
    core::ignoreUnused(terminationHint);

    // A held-directory child's whole tree, whether or not its leader still runs: what it started may.
    if (_d->job.valid())
    {
        if (TerminateJobObject(_d->job.get(), TerminatedExitCode) != FALSE)
            return;
        ptyLog()("TerminateJobObject() failed: {}", core::systemErrorMessage(GetLastError()));
    }

    if (!alive())
        return;

    TerminateProcess(_d->processInfo.hProcess, TerminatedExitCode);
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
