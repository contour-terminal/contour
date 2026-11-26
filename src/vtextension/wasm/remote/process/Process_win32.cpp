// SPDX-License-Identifier: Apache-2.0
//
// The launcher on Windows. CreateProcessW() with two attributes: PROC_THREAD_ATTRIBUTE_HANDLE_LIST, so that
// the child inherits exactly the duplicates made for it (closed again once it started) and no other
// inheritable handle of this process; and PROC_THREAD_ATTRIBUTE_JOB_LIST, which puts it in its job before its
// first instruction -- no suspended start, no window outside the job. The job kills the child when its last
// handle closes (the process handle's), allows it no second process, caps its memory and lets an unhandled
// exception end it rather than open a dialog.
//
// Isolated (ProcessIsolation::AppContainer), two attributes more: PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES
// starts it in the extension host's AppContainer, with no capabilities, and
// PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY forbids it any child process through its token, not only through
// the job's process count. The profile and the helper directory's grant are ensured on the first such launch
// (AppContainer_win32.hpp). Its working directory is then its executable's own, which the grant lets it open,
// rather than the volume's root, which an AppContainer may not.
//
// The child gets the null device as stdin and stdout, the stderr it is given or else the null device -- never
// this process's console unasked -- the environment it is given and nothing else (contained, with
// LOCALAPPDATA, TEMP and TMP set to the container's own folders, whatever it was given or this process has:
// the system refuses a contained launch without them; LaunchSpelling_win32.hpp), the root of its
// executable's volume as its working directory (contained, its executable's own, as above), and no console at
// all (DETACHED_PROCESS): it is attached to no console of the parent's and receives none of its Ctrl+C
// events, and the system starts no console host for it -- a console of its own (CREATE_NO_WINDOW) would cost
// one console host process per extension host, which the extension host, reading and writing only the handles
// it is given, has no use for.
//
// The inheritance window. The handle list binds only this CreateProcessW(): while the duplicates are
// inheritable, any other CreateProcess() of this process that inherits handles without a list of its own
// (QProcess, a library's launcher) inherits them too -- a channel end, the interrupt page, the wake event --
// and that stranger's copy of a lane end keeps a dead helper's lane from ending (its parent end would never
// read Closed). So the duplicates are made uninheritable, the command line is built from their values, and
// only right before CreateProcessW() are they made inheritable; right after it they are closed, whatever it
// returned. The window is that one call. Helper launches never race one another in it: every one runs on the
// supervisor's thread. macOS has the like window, on its channel ends (Process_macos.cpp).
#include <vtextension/wasm/remote/process/Process_win32.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <vtextension/wasm/BuildInstrumentation.hpp>
#include <vtextension/wasm/remote/HostRefusals.hpp>
#include <vtextension/wasm/remote/process/AppContainer_win32.hpp>
#include <vtextension/wasm/remote/process/LaunchRefusal_win32.hpp>
#include <vtextension/wasm/remote/process/LaunchSpelling_win32.hpp>
#include <vtextension/wasm/remote/process/ProcessLauncher.hpp>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace vtextension::wasm::remote::process
{

namespace
{
    /// The most handles one WaitForMultipleObjects() takes.
    constexpr auto MaxWaitObjects = std::size_t { MAXIMUM_WAIT_OBJECTS };

    /// How often processes beyond what one wait takes are looked at.
    constexpr auto OverflowPollInterval = std::chrono::milliseconds { 100 };

    /// Closes a handle when destroyed.
    struct HandleCloser
    {
        void operator()(HANDLE handle) const noexcept { ::CloseHandle(handle); }
    };
    using OwnedHandle = std::unique_ptr<void, HandleCloser>;

    /// @return This process's value of the variable @p name, in UTF-8; nothing if it has none, none UTF-16
    ///         spells, or an empty one (GetEnvironmentVariableW() reads 0 for it, as for none). Read through
    ///         the Win32 block, which a process's own changes to its environment reach.
    [[nodiscard]] std::optional<std::string> processVariable(std::string_view name)
    {
        auto const wideName = wideOf(name);
        if (!wideName)
            return std::nullopt;
        // Asked once for the size, with its terminator; again, should it have grown in between.
        auto value = std::wstring {};
        auto length = ::GetEnvironmentVariableW(wideName->c_str(), nullptr, 0);
        while (length > value.size())
        {
            value.resize(length);
            length =
                ::GetEnvironmentVariableW(wideName->c_str(), value.data(), static_cast<DWORD>(value.size()));
        }
        if (length == 0)
            return std::nullopt;
        value.resize(length);
        // UTF-8, as every string of the launcher's is; path::string() would spell it in the ANSI code page.
        auto const utf8 = std::filesystem::path { value }.u8string();
        return std::string { utf8.begin(), utf8.end() };
    }

    /// @return Why a call that set @p error failed to start a process.
    [[nodiscard]] LaunchError launchErrorOf(DWORD error) noexcept
    {
        switch (error)
        {
            case ERROR_FILE_NOT_FOUND:
            case ERROR_PATH_NOT_FOUND:
            case ERROR_ACCESS_DENIED:
            case ERROR_BAD_EXE_FORMAT: return LaunchError::NotFound;
            case ERROR_NOT_ENOUGH_MEMORY:
            case ERROR_OUTOFMEMORY:
            case ERROR_NO_SYSTEM_RESOURCES:
            case ERROR_COMMITMENT_LIMIT:
            case ERROR_TOO_MANY_OPEN_FILES: return LaunchError::Resources;
            case ERROR_NOT_SUPPORTED:
            case ERROR_CALL_NOT_IMPLEMENTED: return LaunchError::Unsupported;
            default: return LaunchError::Io;
        }
    }

    /// @return Why a call at @p step that set @p error failed to start a process, and where.
    [[nodiscard]] LaunchRefusal refusalOf(LaunchStep step, DWORD error) noexcept
    {
        return LaunchRefusal { .error = launchErrorOf(error), .step = step, .code = error };
    }

    /// Owns an initialized PROC_THREAD_ATTRIBUTE_LIST.
    class AttributeList
    {
      public:
        /// @return A list of @p count attributes; nothing if it could not be made.
        [[nodiscard]] static std::optional<AttributeList> create(DWORD count)
        {
            auto size = SIZE_T { 0 };
            ::InitializeProcThreadAttributeList(nullptr, count, 0, &size);
            auto storage = std::make_unique<std::byte[]>(size);
            auto* const list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.get());
            if (!::InitializeProcThreadAttributeList(list, count, 0, &size))
                return std::nullopt;
            return AttributeList { std::move(storage), list };
        }

        AttributeList(AttributeList&& other) noexcept:
            _storage { std::move(other._storage) }, _list { std::exchange(other._list, nullptr) }
        {
        }
        AttributeList& operator=(AttributeList&&) = delete;
        AttributeList(AttributeList const&) = delete;
        AttributeList& operator=(AttributeList const&) = delete;
        ~AttributeList()
        {
            if (_list != nullptr)
                ::DeleteProcThreadAttributeList(_list);
        }

        /// Sets @p attribute, the launch's @p step, to @p values, which must outlive the list's use.
        /// @return Nothing; or why the list refused it.
        template <typename T, std::size_t Extent>
        [[nodiscard]] std::expected<void, LaunchRefusal> set(LaunchStep step,
                                                             DWORD_PTR attribute,
                                                             std::span<T, Extent> values) noexcept
        {
            if (!::UpdateProcThreadAttribute(_list,
                                             0,
                                             attribute,
                                             static_cast<void*>(values.data()),
                                             values.size_bytes(),
                                             nullptr,
                                             nullptr))
                return std::unexpected(refusalOf(step, ::GetLastError()));
            return {};
        }

        [[nodiscard]] LPPROC_THREAD_ATTRIBUTE_LIST get() const noexcept { return _list; }

      private:
        AttributeList(std::unique_ptr<std::byte[]> storage, LPPROC_THREAD_ATTRIBUTE_LIST list) noexcept:
            _storage { std::move(storage) }, _list { list }
        {
        }

        std::unique_ptr<std::byte[]> _storage;
        LPPROC_THREAD_ATTRIBUTE_LIST _list;
    };

    /// @return A duplicate of @p handle, not inheritable (yet); nothing if it could not be made.
    [[nodiscard]] OwnedHandle copyOf(HANDLE handle) noexcept
    {
        auto copy = HANDLE { nullptr };
        if (!::DuplicateHandle(
                ::GetCurrentProcess(), handle, ::GetCurrentProcess(), &copy, 0, FALSE, DUPLICATE_SAME_ACCESS))
            return OwnedHandle {};
        return OwnedHandle { copy };
    }

    /// Makes each of @p handles inheritable.
    /// @return Nothing; or why one could not be.
    [[nodiscard]] std::expected<void, LaunchRefusal> allowInheritance(
        std::span<HANDLE const> handles) noexcept
    {
        for (auto* const handle: handles)
            if (!::SetHandleInformation(handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT))
                return std::unexpected(refusalOf(LaunchStep::Inheritance, ::GetLastError()));
        return {};
    }

    /// @return A job that kills its process when closed, allows it no second one, caps its memory at
    ///         @p dataBytes (0: no cap) and ends it on an unhandled exception; or why it could not be made.
    [[nodiscard]] std::expected<OwnedHandle, LaunchRefusal> makeJob(uint64_t dataBytes) noexcept
    {
        auto job = OwnedHandle { ::CreateJobObjectW(nullptr, nullptr) };
        if (!job)
            return std::unexpected(refusalOf(LaunchStep::Job, ::GetLastError()));
        auto limits = JOBOBJECT_EXTENDED_LIMIT_INFORMATION {};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
                                                  | JOB_OBJECT_LIMIT_ACTIVE_PROCESS
                                                  | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
        limits.BasicLimitInformation.ActiveProcessLimit = 1;
        if (dataBytes != 0)
        {
            limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_MEMORY;
            limits.ProcessMemoryLimit = static_cast<SIZE_T>(dataBytes);
        }
        // The code is read before the job closes: its CloseHandle() may set another.
        if (!::SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
            return std::unexpected(refusalOf(LaunchStep::Job, ::GetLastError()));
        return job;
    }

    class Win32ProcessHandle final: public IProcessHandle
    {
      public:
        Win32ProcessHandle(OwnedHandle process, OwnedHandle job, ProcessId id) noexcept:
            _process { std::move(process) }, _job { std::move(job) }, _id { id }
        {
        }

        [[nodiscard]] ProcessId id() const noexcept override { return _id; }

        void kill() noexcept override
        {
            if (!_status)
                ::TerminateProcess(_process.get(), KilledExitCode);
        }

        [[nodiscard]] std::expected<std::optional<ExitStatus>, LaunchError> tryReap() noexcept override
        {
            if (_status)
                return _status;
            switch (::WaitForSingleObject(_process.get(), 0))
            {
                case WAIT_OBJECT_0: break;
                case WAIT_TIMEOUT: return std::nullopt;
                default: return std::unexpected(launchErrorOf(::GetLastError()));
            }
            // Ended, but its code could not be read: lost, rather than a code it may really have ended with.
            auto code = DWORD { 0 };
            _status = ::GetExitCodeProcess(_process.get(), &code)
                          ? ExitStatus { .kind = ExitKind::Exited, .value = static_cast<int>(code) }
                          : LostExitStatus;
            return _status;
        }

        [[nodiscard]] Waitable waitable() const noexcept override { return _process.get(); }

        [[nodiscard]] std::expected<transport::HandleTarget, LaunchError> handleTarget() override
        {
            // A handle of its own, with no more access than duplicating into the process takes.
            auto target = HANDLE { nullptr };
            if (!::DuplicateHandle(::GetCurrentProcess(),
                                   _process.get(),
                                   ::GetCurrentProcess(),
                                   &target,
                                   PROCESS_DUP_HANDLE,
                                   FALSE,
                                   0))
                return std::unexpected(launchErrorOf(::GetLastError()));
            return transport::HandleTarget::ofProcess(transport::OwnedHandle { target });
        }

      private:
        OwnedHandle _process;
        OwnedHandle _job; ///< Its close ends the process, if it still runs.
        ProcessId _id;
        std::optional<ExitStatus> _status; ///< Set once it is seen to have ended.
    };

    /// Waits on an auto-reset wake event and the first MaxWaitObjects - 1 processes; those beyond are looked
    /// at every OverflowPollInterval.
    class Win32ExitWaitSet final: public IExitWaitSet
    {
      public:
        Win32ExitWaitSet(core::IClock& clock, OwnedHandle wake) noexcept:
            _clock { &clock }, _wake { std::move(wake) }
        {
        }

        [[nodiscard]] std::expected<void, LaunchError> watch(uint64_t key,
                                                             IProcessHandle const& process) override
        {
            unwatch(key);
            _watched.emplace_back(key, process.waitable());
            return {};
        }

        void unwatch(uint64_t key) noexcept override
        {
            std::erase_if(_watched, [&](auto const& watched) { return watched.first == key; });
        }

        [[nodiscard]] std::expected<std::vector<uint64_t>, LaunchError> wait(
            std::optional<core::SteadyTimePoint> deadline) override
        {
            while (true)
            {
                auto handles = std::vector<HANDLE> { _wake.get() };
                for (auto const& [key, process]: _watched | std::views::take(MaxWaitObjects - 1))
                    handles.push_back(process);
                auto const result = ::WaitForMultipleObjects(
                    static_cast<DWORD>(handles.size()), handles.data(), FALSE, timeoutOf(deadline));
                // A watched handle closed while watched: waiting again would fail again, at once.
                if (result == WAIT_FAILED)
                    return std::unexpected(launchErrorOf(::GetLastError()));
                auto ended = std::vector<uint64_t> {};
                for (auto const& [key, process]: _watched)
                    if (::WaitForSingleObject(process, 0) == WAIT_OBJECT_0)
                        ended.push_back(key);
                if (!ended.empty() || result == WAIT_OBJECT_0 || (deadline && _clock->now() >= *deadline))
                    return ended;
            }
        }

        void wake() noexcept override { ::SetEvent(_wake.get()); }

      private:
        /// @return The wait's timeout in milliseconds until @p deadline, rounded up; at most the overflow
        /// poll
        ///         interval while processes overflow the wait.
        [[nodiscard]] DWORD timeoutOf(std::optional<core::SteadyTimePoint> deadline) const
        {
            auto timeout = std::optional<std::chrono::milliseconds> {};
            if (deadline)
                timeout = std::max(std::chrono::milliseconds { 0 },
                                   std::chrono::ceil<std::chrono::milliseconds>(*deadline - _clock->now()));
            if (_watched.size() > MaxWaitObjects - 1)
                timeout = std::min(timeout.value_or(OverflowPollInterval), OverflowPollInterval);
            if (!timeout)
                return INFINITE;
            constexpr auto Longest = std::chrono::milliseconds { INFINITE - 1 };
            return static_cast<DWORD>(std::min(*timeout, Longest).count());
        }

        core::IClock* _clock;
        OwnedHandle _wake;
        std::vector<std::pair<uint64_t, HANDLE>> _watched; ///< Each key's process, owned by its handle.
    };

    /// What an isolated launch adds: to the attribute list the container and the child process policy, to
    /// the environment the container's folders.
    struct ContainedAttributes
    {
        SECURITY_CAPABILITIES capabilities; ///< The container's SID, and no capability.
        DWORD childProcessPolicy;           ///< PROCESS_CREATION_CHILD_PROCESS_RESTRICTED.
        ContainerFolders folders; ///< The container's own folder, and the profile's (ContainerVariables).
    };

    class Win32ProcessLauncher final: public IProcessLauncher
    {
      public:
        /// @param api What isolated launches' containers are ensured through.
        /// @param diagnostics Where a refusal its launch error cannot tell apart is said, once per step and
        ///        code, and a grant this user may not make: Contour's error log.
        Win32ProcessLauncher(AppContainerApi api, std::function<void(std::string_view line)> diagnostics):
            _containers { std::move(api), diagnostics }, _diagnostics { std::move(diagnostics) }
        {
        }

        [[nodiscard]] std::expected<std::unique_ptr<IProcessHandle>, LaunchError> launch(
            LaunchSpec const& spec) override
        {
            return start(spec).transform_error([this](LaunchRefusal const& refusal) {
                if (std::ranges::contains(DetailedLaunchErrors, refusal.error)
                    && !std::ranges::contains(_saidRefusals, std::pair { refusal.step, refusal.code }))
                {
                    _saidRefusals.emplace_back(refusal.step, refusal.code);
                    _diagnostics(launchRefusalLine(refusal));
                }
                return refusal.error;
            });
        }

        [[nodiscard]] std::expected<std::unique_ptr<IExitWaitSet>, LaunchError> makeWaitSet(
            core::IClock& clock) override
        {
            auto wake = OwnedHandle { ::CreateEventW(nullptr, FALSE, FALSE, nullptr) };
            if (!wake)
                return std::unexpected(launchErrorOf(::GetLastError()));
            return std::make_unique<Win32ExitWaitSet>(clock, std::move(wake));
        }

      private:
        /// Starts @p spec's process.
        /// @return Its handle; or why and where the system refused.
        [[nodiscard]] std::expected<std::unique_ptr<IProcessHandle>, LaunchRefusal> start(
            LaunchSpec const& spec)
        {
            auto const executable = std::filesystem::absolute(spec.executable);
            auto const attributes = ::GetFileAttributesW(executable.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES)
                return std::unexpected(LaunchRefusal { .error = LaunchError::NotFound,
                                                       .step = LaunchStep::Executable,
                                                       .code = ::GetLastError() });
            if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
                return std::unexpected(LaunchRefusal { .error = LaunchError::NotFound,
                                                       .step = LaunchStep::Executable,
                                                       .code = ERROR_DIRECTORY });

            // The child's copies, uninheritable until right before CreateProcessW() (the file head's window).
            auto owned = std::vector<OwnedHandle> {};
            auto childHandles = std::vector<transport::NativeHandle> {};
            for (auto const handle: spec.inherited)
            {
                auto copy = copyOf(handle);
                if (!copy)
                    return std::unexpected(refusalOf(LaunchStep::CopyHandle, ::GetLastError()));
                childHandles.push_back(copy.get());
                owned.push_back(std::move(copy));
            }
            auto nullDevice = OwnedHandle { ::CreateFileW(L"NUL",
                                                          GENERIC_READ | GENERIC_WRITE,
                                                          FILE_SHARE_READ | FILE_SHARE_WRITE,
                                                          nullptr,
                                                          OPEN_EXISTING,
                                                          0,
                                                          nullptr) };
            if (nullDevice.get() == INVALID_HANDLE_VALUE)
            {
                std::ignore = nullDevice.release();
                return std::unexpected(refusalOf(LaunchStep::NullDevice, ::GetLastError()));
            }
            // The stderr the spec names, if it is a handle; the null device if not.
            auto errorOutput = OwnedHandle {};
            if (spec.standardError && *spec.standardError != nullptr
                && *spec.standardError != INVALID_HANDLE_VALUE)
                errorOutput = copyOf(*spec.standardError);
            auto* const childError = errorOutput ? errorOutput.get() : nullDevice.get();

            auto handleList = childHandles;
            handleList.push_back(nullDevice.get());
            if (errorOutput)
                handleList.push_back(errorOutput.get());

            auto contained = std::optional<ContainedAttributes> {};
            if (spec.isolation == ProcessIsolation::AppContainer)
            {
                auto const container = _containers.prepare(executable.parent_path());
                if (!container)
                    return std::unexpected(container.error());
                contained = ContainedAttributes {
                    .capabilities = SECURITY_CAPABILITIES { .AppContainerSid = container->sid,
                                                            .Capabilities = nullptr,
                                                            .CapabilityCount = 0,
                                                            .Reserved = 0 },
                    .childProcessPolicy = PROCESS_CREATION_CHILD_PROCESS_RESTRICTED,
                    .folders = container->folders,
                };
            }

            auto job = makeJob(spec.caps.dataBytes);
            if (!job)
                return std::unexpected(job.error());
            auto jobList = std::array<HANDLE, 1> { job->get() };
            auto list = AttributeList::create(contained ? 4 : 2);
            if (!list)
                return std::unexpected(refusalOf(LaunchStep::AttributeList, ::GetLastError()));
            if (auto const set =
                    list->set(LaunchStep::HandleList,
                              PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                              std::span { handleList })
                        .and_then([&] {
                            return list->set(
                                LaunchStep::JobList, PROC_THREAD_ATTRIBUTE_JOB_LIST, std::span { jobList });
                        })
                        .and_then([&]() -> std::expected<void, LaunchRefusal> {
                            if (!contained)
                                return {};
                            return list
                                ->set(LaunchStep::Container,
                                      PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES,
                                      std::span { &contained->capabilities, 1 })
                                .and_then([&] {
                                    return list->set(LaunchStep::ChildProcessPolicy,
                                                     PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY,
                                                     std::span { &contained->childProcessPolicy, 1 });
                                });
                        });
                !set)
                return std::unexpected(set.error());

            auto commandLine = std::wstring {};
            appendArgument(commandLine, executable.wstring());
            for (auto const& argument:
                 spec.arguments ? spec.arguments(childHandles) : std::vector<std::string> {})
            {
                auto const wide = wideOf(argument);
                if (!wide)
                    return std::unexpected(refusalOf(LaunchStep::Spelling, ERROR_NO_UNICODE_TRANSLATION));
                appendArgument(commandLine, *wide);
            }
            // Contained, the container's folders are named by what the parent asked the system for, never by
            // this process's environment (ContainerVariables).
            auto const environment =
                contained ? containedEnvironment(spec.environment, contained->folders) : spec.environment;
            auto block = environmentBlock(environment);
            if (!block)
                return std::unexpected(refusalOf(LaunchStep::Spelling, ERROR_NO_UNICODE_TRANSLATION));
            auto const workingDirectory =
                (contained ? executable.parent_path() : executable.root_path()).wstring();

            auto startup = STARTUPINFOEXW {};
            startup.StartupInfo.cb = sizeof(startup);
            startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
            startup.StartupInfo.hStdInput = nullDevice.get();
            startup.StartupInfo.hStdOutput = nullDevice.get();
            startup.StartupInfo.hStdError = childError;
            startup.lpAttributeList = list->get();
            auto info = PROCESS_INFORMATION {};
            if (auto const allowed = allowInheritance(handleList); !allowed)
                return std::unexpected(allowed.error());
            // No console (DETACHED_PROCESS): a contained child started with one of its own (CREATE_NO_WINDOW)
            // died in its startup with 0xC0000142 on a Windows runner. Unverified why; a likely reason is
            // that its console host is started as its child, which a contained child may not have
            // (PROCESS_CREATION_CHILD_PROCESS_RESTRICTED).
            auto const created =
                ::CreateProcessW(executable.c_str(),
                                 commandLine.data(),
                                 nullptr,
                                 nullptr,
                                 TRUE,
                                 EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | DETACHED_PROCESS,
                                 block->data(),
                                 workingDirectory.c_str(),
                                 &startup.StartupInfo,
                                 &info);
            auto const error = ::GetLastError();
            // Closed at once: the child holds its own copies, and no other process may inherit these.
            owned.clear();
            nullDevice.reset();
            errorOutput.reset();
            if (!created)
                return std::unexpected(refusalOf(LaunchStep::CreateProcess, error));
            ::CloseHandle(info.hThread);
            return std::make_unique<Win32ProcessHandle>(
                OwnedHandle { info.hProcess }, *std::move(job), static_cast<ProcessId>(info.dwProcessId));
        }

        AppContainerProvision _containers; ///< Ensured on the first isolated launch; used on this one thread.
        /// Where a refusal its launch error cannot tell apart is said, and a grant this user may not make:
        /// Contour's error log, or a test's record of it.
        std::function<void(std::string_view line)> _diagnostics;
        std::vector<std::pair<LaunchStep, DWORD>> _saidRefusals; ///< Each step and code said already.
    };
} // namespace

std::shared_ptr<IProcessLauncher> makeWin32ProcessLauncher(
    AppContainerApi api, std::function<void(std::string_view line)> diagnostics)
{
    return std::make_shared<Win32ProcessLauncher>(std::move(api), std::move(diagnostics));
}

std::shared_ptr<IProcessLauncher> makeSystemProcessLauncher()
{
    return makeWin32ProcessLauncher(systemAppContainerApi(), [writer = logWriter()](std::string_view line) {
        writer(LineSeverity::Error, line);
    });
}

ProcessId currentProcessId() noexcept
{
    return static_cast<ProcessId>(::GetCurrentProcessId());
}

std::optional<transport::NativeHandle> defaultHelperStandardError()
{
    if constexpr (ThisBuildInstrumentation != BuildInstrumentation::None)
        if (auto* const own = ::GetStdHandle(STD_ERROR_HANDLE); own != nullptr && own != INVALID_HANDLE_VALUE)
            return own;
    return std::nullopt;
}

std::vector<std::pair<std::string, std::string>> defaultHelperEnvironment()
{
    // Without SystemRoot a process cannot load the system's DLLs.
    auto systemRoot = processVariable("SystemRoot");
    if (!systemRoot)
        return {};
    return { { "SystemRoot", *std::move(systemRoot) } };
}

} // namespace vtextension::wasm::remote::process
