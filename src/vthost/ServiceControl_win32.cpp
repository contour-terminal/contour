// SPDX-License-Identifier: Apache-2.0

// clang-format off
#ifdef _WIN32
    #include <winsock2.h>
    #include <windows.h>
#endif
// clang-format on

#include <vthost/Daemon.h>
#include <vthost/ServiceControl.h>

#ifdef _WIN32

    #include <array>
    #include <cstddef>
    #include <format>
    #include <span>
    #include <string>
    #include <tuple>
    #include <utility>
    #include <vector>

    #include <taskschd.h>

namespace vthost
{

namespace
{
    /// Owns one COM interface pointer. The SDK's own ComPtr lives in <wrl/client.h>, which
    /// drags in the whole WRL surface for the two operations wanted here.
    template <typename T>
    class ComPtr
    {
      public:
        ComPtr() = default;
        ~ComPtr() { reset(); }

        ComPtr(ComPtr const&) = delete;
        ComPtr& operator=(ComPtr const&) = delete;
        ComPtr(ComPtr&& other) noexcept: _ptr(std::exchange(other._ptr, nullptr)) {}
        ComPtr& operator=(ComPtr&& other) noexcept
        {
            if (this != &other)
            {
                reset();
                _ptr = std::exchange(other._ptr, nullptr);
            }
            return *this;
        }

        [[nodiscard]] T** put() noexcept { return &_ptr; }
        [[nodiscard]] T* get() const noexcept { return _ptr; }
        T* operator->() const noexcept { return _ptr; }
        explicit operator bool() const noexcept { return _ptr != nullptr; }

        void reset() noexcept
        {
            if (_ptr != nullptr)
            {
                _ptr->Release();
                _ptr = nullptr;
            }
        }

      private:
        T* _ptr = nullptr;
    };

    /// Owns one BSTR, the string type every Task Scheduler entry point takes.
    class Bstr
    {
      public:
        explicit Bstr(std::wstring const& text): _value(::SysAllocString(text.c_str())) {}
        ~Bstr()
        {
            if (_value != nullptr)
                ::SysFreeString(_value);
        }

        Bstr(Bstr const&) = delete;
        Bstr& operator=(Bstr const&) = delete;
        Bstr(Bstr&&) = delete;
        Bstr& operator=(Bstr&&) = delete;

        [[nodiscard]] BSTR get() const noexcept { return _value; }

      private:
        BSTR _value = nullptr;
    };

    /// Initializes COM for the calling thread and uninitializes it again.
    ///
    /// Scoped to one operation rather than the process: these verbs run in a short-lived CLI
    /// invocation, and a process-wide apartment would be a second global to reason about in a
    /// binary that also hosts Qt.
    class ComScope
    {
      public:
        ComScope() noexcept: _hr(::CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
        ~ComScope()
        {
            // RPC_E_CHANGED_MODE means someone else already initialized this thread with a
            // different apartment; we did not initialize it, so we must not uninitialize it.
            if (SUCCEEDED(_hr))
                ::CoUninitialize();
        }

        ComScope(ComScope const&) = delete;
        ComScope& operator=(ComScope const&) = delete;
        ComScope(ComScope&&) = delete;
        ComScope& operator=(ComScope&&) = delete;

        [[nodiscard]] bool ok() const noexcept { return SUCCEEDED(_hr) || _hr == RPC_E_CHANGED_MODE; }
        [[nodiscard]] HRESULT hr() const noexcept { return _hr; }

      private:
        HRESULT _hr;
    };

    [[nodiscard]] std::wstring widen(std::string_view text)
    {
        if (text.empty())
            return {};
        auto const needed =
            ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        if (needed <= 0)
            return {};
        auto wide = std::wstring(static_cast<std::size_t>(needed), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), needed);
        return wide;
    }

    [[nodiscard]] std::string narrow(std::wstring_view text)
    {
        if (text.empty())
            return {};
        auto const needed = ::WideCharToMultiByte(
            CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        if (needed <= 0)
            return {};
        auto narrowed = std::string(static_cast<std::size_t>(needed), '\0');
        ::WideCharToMultiByte(CP_UTF8,
                              0,
                              text.data(),
                              static_cast<int>(text.size()),
                              narrowed.data(),
                              needed,
                              nullptr,
                              nullptr);
        return narrowed;
    }

    [[nodiscard]] ServiceError comError(HRESULT hr, std::string context)
    {
        // Access-denied surfaces through several spellings; all mean the same thing to a user
        // who now has to decide whether to retry elevated.
        auto const code = (hr == E_ACCESSDENIED || hr == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED))
                              ? ServiceErrorCode::AccessDenied
                              : ServiceErrorCode::Backend;
        return ServiceError { .code = code,
                              .systemCode = static_cast<long>(hr),
                              .context = std::move(context) };
    }

    /// The registering user, in the SAM form the Task Scheduler wants ("DOMAIN\\user").
    ///
    /// The trigger is bound to this account deliberately: an unbound logon trigger fires for
    /// EVERY user who logs on, which would start this user's daemon on someone else's login.
    [[nodiscard]] std::wstring currentUserName()
    {
        auto buffer = std::array<wchar_t, 256> {};
        auto size = static_cast<DWORD>(buffer.size());
        if (::GetUserNameW(buffer.data(), &size) == 0)
            return {};
        auto const user = std::wstring { buffer.data() };

        auto domainBuffer = std::array<wchar_t, 256> {};
        auto domainSize = static_cast<DWORD>(domainBuffer.size());
        if (::GetComputerNameW(domainBuffer.data(), &domainSize) == 0)
            return user;
        return std::wstring { domainBuffer.data() } + L"\\" + user;
    }

    /// Splits a full argv into the executable and the quoted argument tail an IExecAction wants
    /// — it takes the two separately, unlike CreateProcess.
    ///
    /// The tail is quoted by vthost::joinCommandLine, the same function the auto-spawn path
    /// hands to CreateProcess: a registration that quoted its arguments differently from the
    /// process the client spawns would break on exactly the paths (`C:\Program Files\...`)
    /// that quoting exists for.
    [[nodiscard]] std::pair<std::string, std::string> splitExecutableAndArguments(
        std::vector<std::string> const& commandLine)
    {
        if (commandLine.empty())
            return {};
        return { commandLine.front(), joinCommandLine(std::span { commandLine }.subspan(1)) };
    }

    /// Hosts the daemon as a Scheduled Task triggered by the installing user's logon.
    ///
    /// This is the backend that answers "start it when I log in": it runs as the invoking user,
    /// in that user's session, and needs neither elevation nor a stored password — none of
    /// which the SCM can offer together (@see ServiceControl.h).
    class TaskSchedulerBackend final: public ServiceBackend
    {
      public:
        explicit TaskSchedulerBackend(std::string_view name): _name(widen(name)) {}

        [[nodiscard]] std::expected<void, ServiceError> install(
            ServiceInstallRequest const& request) override;
        [[nodiscard]] std::expected<void, ServiceError> uninstall() override;
        [[nodiscard]] std::expected<void, ServiceError> start() override;
        [[nodiscard]] std::expected<void, ServiceError> stop() override;
        [[nodiscard]] std::expected<ServiceStatus, ServiceError> status() const override;

      private:
        /// Connects to the local Task Scheduler and opens the root folder.
        [[nodiscard]] static std::expected<std::pair<ComPtr<ITaskService>, ComPtr<ITaskFolder>>, ServiceError>
        connect();

        /// Looks the registration up; a missing one is NotInstalled, not a backend failure.
        [[nodiscard]] std::expected<ComPtr<IRegisteredTask>, ServiceError> lookup(ITaskFolder* folder) const;

        std::wstring _name;
    };

    std::expected<std::pair<ComPtr<ITaskService>, ComPtr<ITaskFolder>>, ServiceError> TaskSchedulerBackend::
        connect()
    {
        auto service = ComPtr<ITaskService> {};
        auto hr = ::CoCreateInstance(CLSID_TaskScheduler,
                                     nullptr,
                                     CLSCTX_INPROC_SERVER,
                                     IID_ITaskService,
                                     reinterpret_cast<void**>(service.put()));
        if (FAILED(hr))
            return std::unexpected(comError(hr, "CoCreateInstance(TaskScheduler)"));

        // Four empty VARIANT arguments mean "the local machine, as the current user".
        auto empty = VARIANT {};
        ::VariantInit(&empty);
        hr = service->Connect(empty, empty, empty, empty);
        if (FAILED(hr))
            return std::unexpected(comError(hr, "ITaskService::Connect"));

        auto folder = ComPtr<ITaskFolder> {};
        auto const root = Bstr { L"\\" };
        hr = service->GetFolder(root.get(), folder.put());
        if (FAILED(hr))
            return std::unexpected(comError(hr, "ITaskService::GetFolder"));

        return std::pair { std::move(service), std::move(folder) };
    }

    std::expected<ComPtr<IRegisteredTask>, ServiceError> TaskSchedulerBackend::lookup(
        ITaskFolder* folder) const
    {
        auto const name = Bstr { _name };
        auto task = ComPtr<IRegisteredTask> {};
        auto const hr = folder->GetTask(name.get(), task.put());
        if (FAILED(hr))
            return std::unexpected(ServiceError { .code = ServiceErrorCode::NotInstalled,
                                                  .systemCode = static_cast<long>(hr),
                                                  .context = "ITaskFolder::GetTask" });
        return task;
    }

    std::expected<void, ServiceError> TaskSchedulerBackend::install(ServiceInstallRequest const& request)
    {
        auto const com = ComScope {};
        if (!com.ok())
            return std::unexpected(comError(com.hr(), "CoInitializeEx"));

        auto connected = connect();
        if (!connected)
            return std::unexpected(connected.error());
        auto& [service, folder] = *connected;

        auto definition = ComPtr<ITaskDefinition> {};
        auto hr = service->NewTask(0, definition.put());
        if (FAILED(hr))
            return std::unexpected(comError(hr, "ITaskService::NewTask"));

        if (auto info = ComPtr<IRegistrationInfo> {}; SUCCEEDED(definition->get_RegistrationInfo(info.put())))
        {
            auto const author = Bstr { L"Contour Terminal" };
            std::ignore = info->put_Author(author.get());
            auto const description = Bstr { widen(request.description) };
            std::ignore = info->put_Description(description.get());
        }

        auto const user = currentUserName();

        // Run as the invoking user with that user's normal rights: a terminal multiplexer has
        // no business holding an elevated token, and TASK_LOGON_INTERACTIVE_TOKEN is what lets
        // the registration skip a stored password entirely.
        if (auto principal = ComPtr<IPrincipal> {}; SUCCEEDED(definition->get_Principal(principal.put())))
        {
            auto const userId = Bstr { user };
            std::ignore = principal->put_UserId(userId.get());
            std::ignore = principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN);
            std::ignore = principal->put_RunLevel(TASK_RUNLEVEL_LUA);
        }

        // A daemon is meant to outlive everything; the defaults would stop it after three days
        // and refuse to start it on battery.
        if (auto settings = ComPtr<ITaskSettings> {}; SUCCEEDED(definition->get_Settings(settings.put())))
        {
            std::ignore = settings->put_StartWhenAvailable(VARIANT_TRUE);
            std::ignore = settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE);
            std::ignore = settings->put_StopIfGoingOnBatteries(VARIANT_FALSE);
            std::ignore = settings->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW);
            std::ignore = settings->put_ExecutionTimeLimit(Bstr { L"PT0S" }.get()); // no limit
        }

        auto triggers = ComPtr<ITriggerCollection> {};
        hr = definition->get_Triggers(triggers.put());
        if (FAILED(hr))
            return std::unexpected(comError(hr, "ITaskDefinition::get_Triggers"));
        auto trigger = ComPtr<ITrigger> {};
        hr = triggers->Create(TASK_TRIGGER_LOGON, trigger.put());
        if (FAILED(hr))
            return std::unexpected(comError(hr, "ITriggerCollection::Create(LOGON)"));
        if (auto logon = ComPtr<ILogonTrigger> {};
            SUCCEEDED(trigger->QueryInterface(IID_ILogonTrigger, reinterpret_cast<void**>(logon.put()))))
        {
            auto const id = Bstr { L"contour-daemon-logon" };
            std::ignore = logon->put_Id(id.get());
            auto const userId = Bstr { user };
            std::ignore = logon->put_UserId(userId.get()); // THIS user's logon, not anyone's
        }

        auto actions = ComPtr<IActionCollection> {};
        hr = definition->get_Actions(actions.put());
        if (FAILED(hr))
            return std::unexpected(comError(hr, "ITaskDefinition::get_Actions"));
        auto action = ComPtr<IAction> {};
        hr = actions->Create(TASK_ACTION_EXEC, action.put());
        if (FAILED(hr))
            return std::unexpected(comError(hr, "IActionCollection::Create(EXEC)"));
        auto exec = ComPtr<IExecAction> {};
        hr = action->QueryInterface(IID_IExecAction, reinterpret_cast<void**>(exec.put()));
        if (FAILED(hr))
            return std::unexpected(comError(hr, "IAction::QueryInterface(IExecAction)"));

        auto const [executable, arguments] = splitExecutableAndArguments(request.commandLine);
        auto const path = Bstr { widen(executable) };
        std::ignore = exec->put_Path(path.get());
        auto const argumentText = Bstr { widen(arguments) };
        std::ignore = exec->put_Arguments(argumentText.get());

        auto empty = VARIANT {};
        ::VariantInit(&empty);
        auto registered = ComPtr<IRegisteredTask> {};
        auto const name = Bstr { _name };
        hr = folder->RegisterTaskDefinition(name.get(),
                                            definition.get(),
                                            TASK_CREATE_OR_UPDATE,
                                            empty, // userId: the current user
                                            empty, // password: none, per the logon type below
                                            TASK_LOGON_INTERACTIVE_TOKEN,
                                            empty, // sddl
                                            registered.put());
        if (FAILED(hr))
            return std::unexpected(comError(hr, "ITaskFolder::RegisterTaskDefinition"));
        return {};
    }

    std::expected<void, ServiceError> TaskSchedulerBackend::uninstall()
    {
        auto const com = ComScope {};
        if (!com.ok())
            return std::unexpected(comError(com.hr(), "CoInitializeEx"));

        auto connected = connect();
        if (!connected)
            return std::unexpected(connected.error());
        auto& [service, folder] = *connected;

        // Stopping first is not optional: DeleteTask leaves a running instance running, and the
        // daemon would then survive its own uninstall with nothing left to manage it.
        if (auto task = lookup(folder.get()); task)
            std::ignore = (*task)->Stop(0);

        auto const name = Bstr { _name };
        if (auto const hr = folder->DeleteTask(name.get(), 0); FAILED(hr))
            return std::unexpected(ServiceError { .code = ServiceErrorCode::NotInstalled,
                                                  .systemCode = static_cast<long>(hr),
                                                  .context = "ITaskFolder::DeleteTask" });
        return {};
    }

    std::expected<void, ServiceError> TaskSchedulerBackend::start()
    {
        auto const com = ComScope {};
        if (!com.ok())
            return std::unexpected(comError(com.hr(), "CoInitializeEx"));

        auto connected = connect();
        if (!connected)
            return std::unexpected(connected.error());
        auto& [service, folder] = *connected;

        auto task = lookup(folder.get());
        if (!task)
            return std::unexpected(task.error());

        auto empty = VARIANT {};
        ::VariantInit(&empty);
        auto running = ComPtr<IRunningTask> {};
        if (auto const hr = (*task)->Run(empty, running.put()); FAILED(hr))
            return std::unexpected(comError(hr, "IRegisteredTask::Run"));
        return {};
    }

    std::expected<void, ServiceError> TaskSchedulerBackend::stop()
    {
        auto const com = ComScope {};
        if (!com.ok())
            return std::unexpected(comError(com.hr(), "CoInitializeEx"));

        auto connected = connect();
        if (!connected)
            return std::unexpected(connected.error());
        auto& [service, folder] = *connected;

        auto task = lookup(folder.get());
        if (!task)
            return std::unexpected(task.error());

        if (auto const hr = (*task)->Stop(0); FAILED(hr))
            return std::unexpected(ServiceError { .code = ServiceErrorCode::NotRunning,
                                                  .systemCode = static_cast<long>(hr),
                                                  .context = "IRegisteredTask::Stop" });
        return {};
    }

    std::expected<ServiceStatus, ServiceError> TaskSchedulerBackend::status() const
    {
        auto const com = ComScope {};
        if (!com.ok())
            return std::unexpected(comError(com.hr(), "CoInitializeEx"));

        auto connected = connect();
        if (!connected)
            return std::unexpected(connected.error());
        auto& [service, folder] = *connected;

        auto task = lookup(folder.get());
        if (!task)
            return ServiceStatus { .state = ServiceRunState::NotInstalled,
                                   .mode = ServiceStartMode::Logon,
                                   .commandLine = {} };

        auto state = TASK_STATE_UNKNOWN;
        std::ignore = (*task)->get_State(&state);

        // The registered action is what the daemon will actually run, and the one thing a user
        // cannot otherwise check without opening the Task Scheduler UI.
        auto commandLine = std::string {};
        if (auto definition = ComPtr<ITaskDefinition> {};
            SUCCEEDED((*task)->get_Definition(definition.put())))
            if (auto actions = ComPtr<IActionCollection> {};
                SUCCEEDED(definition->get_Actions(actions.put())))
                if (auto action = ComPtr<IAction> {}; SUCCEEDED(actions->get_Item(1, action.put())))
                    if (auto exec = ComPtr<IExecAction> {}; SUCCEEDED(
                            action->QueryInterface(IID_IExecAction, reinterpret_cast<void**>(exec.put()))))
                    {
                        BSTR path = nullptr;
                        BSTR arguments = nullptr;
                        if (SUCCEEDED(exec->get_Path(&path)) && path != nullptr)
                        {
                            commandLine = narrow(path);
                            ::SysFreeString(path);
                        }
                        if (SUCCEEDED(exec->get_Arguments(&arguments)) && arguments != nullptr)
                        {
                            commandLine += " " + narrow(arguments);
                            ::SysFreeString(arguments);
                        }
                    }

        return ServiceStatus { .state = state == TASK_STATE_RUNNING ? ServiceRunState::Running
                                                                    : ServiceRunState::Stopped,
                               .mode = ServiceStartMode::Logon,
                               .commandLine = std::move(commandLine) };
    }
} // namespace

namespace
{
    /// The not-yet-built SCM backend.
    ///
    /// `boot` and `manual` mean a real SCM service, and an SCM service running as a NAMED USER
    /// needs that user's password at CreateServiceW time — there is no equivalent of the
    /// logon task's TASK_LOGON_INTERACTIVE_TOKEN. That means an interactive credential prompt
    /// plus a service-control dispatcher (StartServiceCtrlDispatcherW) in the daemon itself,
    /// neither of which exists yet. Reporting exactly that beats registering something under
    /// LocalSystem, which would start in session 0 with a different %TEMP% and %USERNAME% and
    /// therefore bind a socket the user's own `contour client` could never find.
    class UnimplementedScmBackend final: public ServiceBackend
    {
      public:
        [[nodiscard]] std::expected<void, ServiceError> install(ServiceInstallRequest const&) override
        {
            return std::unexpected(error());
        }
        [[nodiscard]] std::expected<void, ServiceError> uninstall() override
        {
            return std::unexpected(error());
        }
        [[nodiscard]] std::expected<void, ServiceError> start() override { return std::unexpected(error()); }
        [[nodiscard]] std::expected<void, ServiceError> stop() override { return std::unexpected(error()); }
        [[nodiscard]] std::expected<ServiceStatus, ServiceError> status() const override
        {
            return std::unexpected(error());
        }

      private:
        [[nodiscard]] static ServiceError error()
        {
            return ServiceError { .code = ServiceErrorCode::Unsupported,
                                  .systemCode = 0,
                                  .context = "--start=boot and --start=manual are not implemented yet; "
                                             "use --start=logon, which starts with your session and "
                                             "needs no password" };
        }
    };
} // namespace

std::unique_ptr<ServiceBackend> makeServiceBackend(ServiceStartMode mode, std::string_view name)
{
    switch (mode)
    {
        case ServiceStartMode::Logon: return std::make_unique<TaskSchedulerBackend>(name);
        case ServiceStartMode::Boot:
        case ServiceStartMode::Manual: return std::make_unique<UnimplementedScmBackend>();
    }
    return std::make_unique<TaskSchedulerBackend>(name);
}

} // namespace vthost

#endif // _WIN32
