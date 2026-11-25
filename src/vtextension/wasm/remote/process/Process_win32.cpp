// SPDX-License-Identifier: Apache-2.0
//
// The launcher on Windows. CreateProcessW() with two attributes: PROC_THREAD_ATTRIBUTE_HANDLE_LIST, so that
// the child inherits exactly the duplicates made for it (closed again once it started) and no other
// inheritable handle of this process; and PROC_THREAD_ATTRIBUTE_JOB_LIST, which puts it in its job before its
// first instruction -- no suspended start, no window outside the job. The job kills the child when its last
// handle closes (the process handle's), allows it no second process, caps its memory and lets an unhandled
// exception end it rather than open a dialog.
//
// The child gets the null device as stdin and stdout, this process's stderr, the environment it is given and
// nothing else, and the root of its executable's volume as its working directory.
//
// The inheritance window. The handle list binds only this CreateProcessW(): while the duplicates are
// inheritable, any other CreateProcess() of this process that inherits handles without a list of its own
// (QProcess, a library's launcher) inherits them too -- a channel end, the interrupt page, the wake event --
// and that stranger's copy of a lane end keeps a dead helper's lane from ending (its parent end would never
// read Closed). So the duplicates are made uninheritable, the command line is built from their values, and
// only right before CreateProcessW() are they made inheritable; right after it they are closed, whatever it
// returned. The window is that one call. Helper launches never race one another in it: every one runs on the
// supervisor's thread. macOS has the like window, on its channel ends (Process_macos.cpp).
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

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

    /// @return @p text, UTF-8, as UTF-16; nothing if it is not UTF-8.
    [[nodiscard]] std::optional<std::wstring> wideOf(std::string_view text)
    {
        if (text.empty())
            return std::wstring {};
        auto const size = ::MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
        if (size == 0)
            return std::nullopt;
        auto wide = std::wstring(static_cast<std::size_t>(size), L'\0');
        ::MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), wide.data(), size);
        return wide;
    }

    /// Appends @p argument to @p commandLine, quoted as the C runtime's argv parser reads it back.
    void appendArgument(std::wstring& commandLine, std::wstring_view argument)
    {
        if (!commandLine.empty())
            commandLine += L' ';
        if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos)
        {
            commandLine += argument;
            return;
        }
        commandLine += L'"';
        auto backslashes = std::size_t { 0 };
        for (auto const c: argument)
        {
            if (c == L'\\')
            {
                ++backslashes;
                continue;
            }
            // Backslashes double only before a quote; the quote itself is escaped.
            commandLine.append(c == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
            backslashes = 0;
            commandLine += c;
        }
        commandLine.append(backslashes * 2, L'\\');
        commandLine += L'"';
    }

    /// @return The environment block of @p environment: `NAME=value` entries sorted by name, as Windows wants
    ///         them, each null-terminated, the block null-terminated; nothing if an entry is not UTF-8.
    [[nodiscard]] std::optional<std::wstring> environmentBlock(
        std::span<std::pair<std::string, std::string> const> environment)
    {
        auto entries = std::vector<std::pair<std::wstring, std::wstring>> {};
        for (auto const& [name, value]: environment)
        {
            auto wideName = wideOf(name);
            auto wideValue = wideOf(value);
            if (!wideName || !wideValue)
                return std::nullopt;
            entries.emplace_back(*std::move(wideName), *std::move(wideValue));
        }
        auto const upper = [](std::wstring const& name) {
            auto folded = name;
            std::ranges::transform(folded, folded.begin(), [](wchar_t c) { return std::towupper(c); });
            return folded;
        };
        std::ranges::sort(entries, {}, [&](auto const& entry) { return upper(entry.first); });
        auto block = std::wstring {};
        for (auto const& [name, value]: entries)
        {
            block += name;
            block += L'=';
            block += value;
            block += L'\0';
        }
        // An empty block still needs two terminators.
        if (entries.empty())
            block += L'\0';
        block += L'\0';
        return block;
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

        /// Sets @p attribute to the handles @p values, which must outlive the list's use.
        /// @return Nothing; or why the list refused it.
        [[nodiscard]] std::expected<void, LaunchError> set(DWORD_PTR attribute,
                                                           std::span<HANDLE> values) noexcept
        {
            if (!::UpdateProcThreadAttribute(
                    _list, 0, attribute, values.data(), values.size_bytes(), nullptr, nullptr))
                return std::unexpected(launchErrorOf(::GetLastError()));
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
    [[nodiscard]] std::expected<void, LaunchError> allowInheritance(std::span<HANDLE const> handles) noexcept
    {
        for (auto* const handle: handles)
            if (!::SetHandleInformation(handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT))
                return std::unexpected(launchErrorOf(::GetLastError()));
        return {};
    }

    /// @return A job that kills its process when closed, allows it no second one, caps its memory at
    ///         @p dataBytes (0: no cap) and ends it on an unhandled exception; nothing if it could not be
    ///         made.
    [[nodiscard]] OwnedHandle makeJob(uint64_t dataBytes) noexcept
    {
        auto job = OwnedHandle { ::CreateJobObjectW(nullptr, nullptr) };
        if (!job)
            return job;
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
        if (!::SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
            return OwnedHandle {};
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

        [[nodiscard]] std::optional<ExitStatus> tryReap() noexcept override
        {
            if (_status)
                return _status;
            if (::WaitForSingleObject(_process.get(), 0) != WAIT_OBJECT_0)
                return std::nullopt;
            auto code = DWORD { 0 };
            if (!::GetExitCodeProcess(_process.get(), &code))
                code = static_cast<DWORD>(-1);
            _status = ExitStatus { .kind = ExitKind::Exited, .value = static_cast<int>(code) };
            return _status;
        }

        [[nodiscard]] Waitable waitable() const noexcept override { return _process.get(); }

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

    class Win32ProcessLauncher final: public IProcessLauncher
    {
      public:
        [[nodiscard]] std::expected<std::unique_ptr<IProcessHandle>, LaunchError> launch(
            LaunchSpec const& spec) override
        {
            auto const executable = std::filesystem::absolute(spec.executable);
            auto const attributes = ::GetFileAttributesW(executable.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
                return std::unexpected(LaunchError::NotFound);

            // The child's copies, uninheritable until right before CreateProcessW() (the file head's window).
            auto owned = std::vector<OwnedHandle> {};
            auto childHandles = std::vector<transport::NativeHandle> {};
            for (auto const handle: spec.inherited)
            {
                auto copy = copyOf(handle);
                if (!copy)
                    return std::unexpected(launchErrorOf(::GetLastError()));
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
                return std::unexpected(launchErrorOf(::GetLastError()));
            }
            // This process's stderr, if it has one; the null device if not.
            auto errorOutput = OwnedHandle {};
            if (auto* const own = ::GetStdHandle(STD_ERROR_HANDLE);
                own != nullptr && own != INVALID_HANDLE_VALUE)
                errorOutput = copyOf(own);
            auto* const childError = errorOutput ? errorOutput.get() : nullDevice.get();

            auto handleList = childHandles;
            handleList.push_back(nullDevice.get());
            if (errorOutput)
                handleList.push_back(errorOutput.get());

            auto job = makeJob(spec.caps.dataBytes);
            if (!job)
                return std::unexpected(launchErrorOf(::GetLastError()));
            auto jobList = std::array<HANDLE, 1> { job.get() };
            auto list = AttributeList::create(2);
            if (!list)
                return std::unexpected(launchErrorOf(::GetLastError()));
            if (auto const set = list->set(PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handleList).and_then([&] {
                    return list->set(PROC_THREAD_ATTRIBUTE_JOB_LIST, jobList);
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
                    return std::unexpected(LaunchError::Io);
                appendArgument(commandLine, *wide);
            }
            auto block = environmentBlock(spec.environment);
            if (!block)
                return std::unexpected(LaunchError::Io);
            auto const workingDirectory = executable.root_path().wstring();

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
            auto const created =
                ::CreateProcessW(executable.c_str(),
                                 commandLine.data(),
                                 nullptr,
                                 nullptr,
                                 TRUE,
                                 EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
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
                return std::unexpected(launchErrorOf(error));
            ::CloseHandle(info.hThread);
            return std::make_unique<Win32ProcessHandle>(
                OwnedHandle { info.hProcess }, std::move(job), static_cast<ProcessId>(info.dwProcessId));
        }

        [[nodiscard]] std::expected<std::unique_ptr<IExitWaitSet>, LaunchError> makeWaitSet(
            core::IClock& clock) override
        {
            auto wake = OwnedHandle { ::CreateEventW(nullptr, FALSE, FALSE, nullptr) };
            if (!wake)
                return std::unexpected(launchErrorOf(::GetLastError()));
            return std::make_unique<Win32ExitWaitSet>(clock, std::move(wake));
        }
    };
} // namespace

std::shared_ptr<IProcessLauncher> makeSystemProcessLauncher()
{
    return std::make_shared<Win32ProcessLauncher>();
}

ProcessId currentProcessId() noexcept
{
    return static_cast<ProcessId>(::GetCurrentProcessId());
}

std::vector<std::pair<std::string, std::string>> defaultHelperEnvironment()
{
    // Without SystemRoot a process cannot load the system's DLLs. Read through the Win32 block, which a
    // process's own changes to its environment reach.
    auto value = std::wstring(MAX_PATH, L'\0');
    auto const length =
        ::GetEnvironmentVariableW(L"SystemRoot", value.data(), static_cast<DWORD>(value.size()));
    if (length == 0 || length >= value.size())
        return {};
    value.resize(length);
    // UTF-8, as every string of the launcher's is; path::string() would spell it in the ANSI code page.
    auto const utf8 = std::filesystem::path { value }.u8string();
    return { { "SystemRoot", std::string { utf8.begin(), utf8.end() } } };
}

} // namespace vtextension::wasm::remote::process
