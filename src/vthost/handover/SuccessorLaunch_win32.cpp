// SPDX-License-Identifier: Apache-2.0
#include <vthost/handover/SuccessorLaunch.hpp>

#ifdef _WIN32

    #include <vtpty/Process.hpp>
    #include <vtpty/PtyAdoption.hpp>
    #include <vtpty/WindowsHandle.hpp>

    #include <core/WideString.hpp>

    #include <algorithm>
    #include <array>
    #include <chrono>
    #include <cstdlib>
    #include <format>
    #include <optional>
    #include <ranges>
    #include <string_view>
    #include <tuple>
    #include <utility>
    #include <vector>

    #include <Windows.h>

    #include <vthost/DaemonSpawn.hpp>
    #include <vthost/Logging.hpp>
    #include <vthost/handover/HandoverMemory.hpp>
    #include <vthost/handover/HandoverRestore.hpp>

namespace vthost::handover
{

namespace
{
    using vtpty::CrossingHandle;
    using vtpty::lastError;
    using vtpty::UniqueHandle;

    /// @return @p timeout as a wait takes it, which never waits INFINITE for a finite one.
    [[nodiscard]] DWORD waitTimeOf(std::chrono::milliseconds timeout) noexcept
    {
        return static_cast<DWORD>(
            std::clamp<std::chrono::milliseconds::rep>(timeout.count(), 0, INFINITE - 1));
    }

    /// The old daemon's half of the commit point: the two events, and a handle to itself for the successor to
    /// wait on. None is inheritable but by the one launch that names them.
    struct Handshake
    {
        UniqueHandle ready { CreateEventW(nullptr, TRUE, FALSE, nullptr) };
        UniqueHandle commit { CreateEventW(nullptr, TRUE, FALSE, nullptr) };
        UniqueHandle self { [] {
            auto handle = HANDLE {};
            DuplicateHandle(GetCurrentProcess(),
                            GetCurrentProcess(),
                            GetCurrentProcess(),
                            &handle,
                            SYNCHRONIZE,
                            FALSE,
                            0);
            return handle;
        }() };

        [[nodiscard]] bool isComplete() const noexcept
        {
            return ready.valid() && commit.valid() && self.valid();
        }
    };

    /// What the successor did while the old daemon waited for it.
    enum class Readiness : uint8_t
    {
        TimedOut = 0, ///< It said nothing within the wait, or the wait itself failed.
        Ended,        ///< It ended first, ready or not.
        Ready,        ///< It set `ready`, and runs.
    };

    /// Waits until @p successor is ready, ends, or @p timeout passes.
    [[nodiscard]] Readiness awaitReady(HANDLE ready,
                                       HANDLE successor,
                                       std::chrono::milliseconds timeout) noexcept
    {
        auto const events = std::array { ready, successor };
        auto const waited = WaitForMultipleObjects(
            static_cast<DWORD>(events.size()), events.data(), FALSE, waitTimeOf(timeout));
        switch (waited)
        {
            case WAIT_OBJECT_0:
                // Ready, and gone already, is no successor to commit to.
                return WaitForSingleObject(successor, 0) == WAIT_OBJECT_0 ? Readiness::Ended
                                                                          : Readiness::Ready;
            case WAIT_OBJECT_0 + 1: return Readiness::Ended;
            default: return Readiness::TimedOut;
        }
    }

    /// Ends whatever ends this handoff without the successor taking over: the mapping is discarded, and every
    /// session and listener is the old daemon's again (LiveUpgrade rolls them back).
    class DiscardedImage
    {
      public:
        DiscardedImage(HandoverMemory& memory, CrossingHandle mapping) noexcept:
            _memory { memory }, _mapping { mapping }
        {
        }
        DiscardedImage(DiscardedImage const&) = delete;
        DiscardedImage& operator=(DiscardedImage const&) = delete;
        DiscardedImage(DiscardedImage&&) = delete;
        DiscardedImage& operator=(DiscardedImage&&) = delete;
        ~DiscardedImage() { _memory.discard(_mapping); }

      private:
        HandoverMemory& _memory;
        CrossingHandle _mapping;
    };

    /// @return Whether @p handle is a standard stream a detached process can go on writing to: a file or a
    ///         pipe, not a console, which the successor has none of.
    [[nodiscard]] bool isPassableStream(HANDLE handle) noexcept
    {
        if (handle == nullptr || handle == INVALID_HANDLE_VALUE)
            return false;
        auto const type = GetFileType(handle);
        return type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE;
    }

    /// Makes @p handles inheritable for as long as it lives, and restores how each was then: what limits them
    /// to the one launch that names them.
    class InheritableForLaunch
    {
      public:
        explicit InheritableForLaunch(std::span<HANDLE const> handles)
        {
            for (auto* const handle: handles)
            {
                auto flags = DWORD {};
                if (GetHandleInformation(handle, &flags) == FALSE)
                    continue;
                _restored.emplace_back(handle,
                                       (flags & HANDLE_FLAG_INHERIT) != 0
                                           ? vtpty::HandleInheritance::Inherited
                                           : vtpty::HandleInheritance::NotInherited);
                if (auto const set = vtpty::setInheritance(handle, vtpty::HandleInheritance::Inherited); !set)
                    errorLog()("live upgrade: a handle the successor needs is not inheritable: {}",
                               set.error().message());
            }
        }
        InheritableForLaunch(InheritableForLaunch const&) = delete;
        InheritableForLaunch& operator=(InheritableForLaunch const&) = delete;
        InheritableForLaunch(InheritableForLaunch&&) = delete;
        InheritableForLaunch& operator=(InheritableForLaunch&&) = delete;
        ~InheritableForLaunch()
        {
            for (auto const& [handle, inheritance]: _restored)
                if (auto const restored = vtpty::setInheritance(handle, inheritance); !restored)
                    errorLog()("live upgrade: a handle stays inheritable after the successor's launch: {}",
                               restored.error().message());
        }

      private:
        std::vector<std::pair<HANDLE, vtpty::HandleInheritance>> _restored;
    };

    /// The operating system's launcher.
    class NativeSuccessorLauncher final: public SuccessorLauncher
    {
      public:
        [[nodiscard]] std::expected<SuccessorProcess, std::error_code> launch(
            std::filesystem::path const& executable,
            std::span<std::string const> argv,
            std::span<CrossingHandle const> inherited) override
        {
            auto commandLine = core::toUtf16(joinCommandLine(argv));
            if (!commandLine)
                return std::unexpected(std::make_error_code(std::errc::invalid_argument));

            // The standard streams cross where they lead somewhere a detached process can write to: a daemon
            // run with its output redirected logs on into the same place.
            auto handles =
                std::ranges::to<std::vector>(std::views::transform(inherited, &CrossingHandle::native));
            auto* const output = GetStdHandle(STD_OUTPUT_HANDLE);
            auto* const error = GetStdHandle(STD_ERROR_HANDLE);
            for (auto* const stream: { output, error })
                if (isPassableStream(stream))
                    handles.push_back(stream);
            std::ranges::sort(handles);
            auto const repeated = std::ranges::unique(handles);
            handles.erase(repeated.begin(), repeated.end());

            auto attributes = vtpty::AttributeList::create(1);
            if (!attributes)
                return std::unexpected(attributes.error());
            if (!handles.empty())
                if (auto const listed = attributes->inheritOnly(handles); !listed)
                    return std::unexpected(listed.error());

            auto startup = STARTUPINFOEXW {};
            startup.StartupInfo.cb = sizeof(startup);
            startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
            startup.StartupInfo.hStdOutput = isPassableStream(output) ? output : nullptr;
            startup.StartupInfo.hStdError = isPassableStream(error) ? error : nullptr;
            startup.lpAttributeList = attributes->get();
            auto info = PROCESS_INFORMATION {};
            auto const started = [&] {
                auto const inheritable = InheritableForLaunch { handles };
                // Detached, as a daemon started in the background is: it has no console to keep open.
                return CreateProcessW(executable.c_str(),
                                      commandLine->data(),
                                      nullptr,
                                      nullptr,
                                      TRUE,
                                      EXTENDED_STARTUPINFO_PRESENT | DETACHED_PROCESS | CREATE_NO_WINDOW,
                                      nullptr,
                                      nullptr,
                                      &startup.StartupInfo,
                                      &info);
            }();
            if (started == FALSE)
                return std::unexpected(lastError());
            CloseHandle(info.hThread);
            return SuccessorProcess { .process = CrossingHandle::of(info.hProcess),
                                      .pid = static_cast<std::int64_t>(info.dwProcessId) };
        }

        void terminate(SuccessorProcess const& successor) override
        {
            auto* const process = successor.process.native();
            TerminateProcess(process, ERROR_PROCESS_ABORTED);
            // One that does not end still cannot take over: it gives up waiting for the commit
            // (SuccessorTakeoverConfig::commitWait).
            if (WaitForSingleObject(process, waitTimeOf(SuccessorTerminateWait)) != WAIT_OBJECT_0)
                errorLog()("live upgrade: the successor {} did not end in time after it was told to",
                           successor.pid);
        }

        [[noreturn]] void endThisProcess() override
        {
            // Not ExitProcess(): that runs every library's detach handler after it ended every other thread,
            // and one waiting for a lock such a thread held would keep this process -- and every handle it
            // still holds to a pseudoconsole the successor serves -- alive for good.
            TerminateProcess(GetCurrentProcess(), EXIT_SUCCESS);
            std::_Exit(EXIT_SUCCESS);
        }
    };

    /// How a successor came to take over, or why it may not.
    enum class Takeover : uint8_t
    {
        /// The handles name no handshake this process can hold: nothing may be taken over. The predecessor,
        /// which never sees it ready, ends it and resumes.
        Impossible = 0,
        /// The predecessor neither committed nor ended in time: it gave up on this process, and serves on.
        Abandoned,
        /// The predecessor ended without committing, right as it saw this one ready: it can resume nothing,
        /// and owns nothing.
        PredecessorEnded,
        Committed, ///< The predecessor saw it ready, committed, and has ended.
    };

    /// The successor's side of the commit point: says it is ready, and waits until it may take over.
    /// @param handles The handles it was handed, the image's mapping read and closed already. Closed by this.
    /// @param config How long it waits.
    /// @return How it came to take over, or why it may not.
    [[nodiscard]] Takeover takeOverFromPredecessor(SuccessorHandles const& handles,
                                                   SuccessorTakeoverConfig const& config)
    {
        auto const ready = UniqueHandle { handles.ready.native() };
        auto const commit = UniqueHandle { handles.commit.native() };
        auto const predecessor = UniqueHandle { handles.predecessor.native() };
        if (SetEvent(ready.get()) == FALSE)
            return Takeover::Impossible;

        auto const events = std::array { commit.get(), predecessor.get() };
        switch (WaitForMultipleObjects(
            static_cast<DWORD>(events.size()), events.data(), FALSE, waitTimeOf(config.commitWait)))
        {
            case WAIT_OBJECT_0:
                // It ends itself as it commits: once it has, nothing it held is held twice.
                if (WaitForSingleObject(predecessor.get(), waitTimeOf(config.predecessorEndWait))
                    != WAIT_OBJECT_0)
                    errorLog()(
                        "handover: the previous daemon committed but did not end in time; taking over");
                return Takeover::Committed;
            case WAIT_OBJECT_0 + 1: return Takeover::PredecessorEnded;
            case WAIT_TIMEOUT: return Takeover::Abandoned;
            default: return Takeover::Impossible;
        }
    }

    /// @return Why @p session cannot be adopted in this process; nothing where it can.
    [[nodiscard]] std::optional<std::string_view> unadoptable(HandoverSession const& session)
    {
        if (auto const adoptable = vtpty::canAdoptPty(session.pty); !adoptable)
            return adoptable.error() == vtpty::AdoptError::ForeignConsoleHost
                       ? "the conpty.dll this release ships is not beside it"
                       : "its pseudoconsole's handles did not cross";
        if (session.device == SessionDevice::ChildOnPty && !vtpty::Process::canAdopt(session.child))
            return "its program's handle did not cross";
        return std::nullopt;
    }
} // namespace

std::unique_ptr<SuccessorLauncher> makeNativeSuccessorLauncher()
{
    return std::make_unique<NativeSuccessorLauncher>();
}

ExecFailure SuccessorHandoff::handOver(std::filesystem::path const& executable,
                                       std::vector<std::string> argv,
                                       HandoverImage const& image,
                                       std::span<uint8_t const> bytes)
{
    auto const mapping = _memory.write(bytes);
    if (!mapping)
        return ExecFailure { .cause = std::make_error_code(std::errc::not_enough_memory) };
    auto const discarded = DiscardedImage { _memory, *mapping };
    auto const handshake = Handshake {};
    if (!handshake.isComplete())
        return ExecFailure { .cause = lastError() };

    auto const handles = SuccessorHandles { .image = *mapping,
                                            .ready = CrossingHandle::of(handshake.ready.get()),
                                            .commit = CrossingHandle::of(handshake.commit.get()),
                                            .predecessor = CrossingHandle::of(handshake.self.get()) };
    auto inherited = handlesNamedBy(image);
    inherited.insert(inherited.end(), { handles.image, handles.ready, handles.commit, handles.predecessor });
    argv.push_back(handoverArgument(handoverToken(handles)));

    auto const successor = _launcher.launch(executable, argv, inherited);
    if (!successor)
        return ExecFailure { .cause = successor.error() };
    auto const process = UniqueHandle { successor->process.native() };
    daemonLog()("live upgrade: started the successor (pid {}); waiting for it to take over", successor->pid);

    switch (awaitReady(handshake.ready.get(), process.get(), _config.readyTimeout))
    {
        case Readiness::Ready:
            // The commit point: from here the successor owns every session and listener, and this process
            // touches none of them again.
            SetEvent(handshake.commit.get());
            daemonLog()("live upgrade: handed over to the successor (pid {})", successor->pid);
            _launcher.endThisProcess();
        case Readiness::Ended:
            errorLog()("live upgrade: the successor (pid {}) ended before it took over", successor->pid);
            return ExecFailure { .cause = std::make_error_code(std::errc::no_such_process) };
        case Readiness::TimedOut: break;
    }
    errorLog()("live upgrade: the successor (pid {}) was not ready within {} ms; ending it",
               successor->pid,
               _config.readyTimeout.count());
    _launcher.terminate(*successor);
    return ExecFailure { .cause = std::make_error_code(std::errc::timed_out) };
}

std::expected<HandoverImage, std::string> receiveHandover(SuccessorHandles const& handles,
                                                          HandoverMemory& memory,
                                                          SuccessorTakeoverConfig config)
{
    // Read, and its mapping closed, before anything is said: an image this process cannot read is one the
    // predecessor keeps serving.
    auto image = readHandover(memory, handles.image);
    auto const refuse = [&](std::string why) {
        // Said nothing: the predecessor ends this process once it gives up waiting, and serves on.
        for (auto const handle: { handles.ready, handles.commit, handles.predecessor })
            CloseHandle(handle.native());
        return std::unexpected(std::move(why));
    };
    if (!image)
        return refuse(std::format("cannot read the image the previous daemon handed over (error {})",
                                  std::to_underlying(image.error())));
    // Every session, or none: one this process cannot adopt is one the predecessor keeps serving.
    for (auto const& session: image->sessions)
        if (auto const why = unadoptable(session))
            return refuse(
                std::format("cannot adopt the session \"{}\" handed over: {}", session.label, *why));

    switch (takeOverFromPredecessor(handles, config))
    {
        case Takeover::Impossible:
            return std::unexpected(std::string { "the handover's handshake names no event of this process" });
        case Takeover::Abandoned:
            return std::unexpected(
                std::format("the previous daemon did not commit within {} ms of this one being ready; it "
                            "serves on",
                            config.commitWait.count()));
        case Takeover::PredecessorEnded:
            errorLog()("handover: the previous daemon ended without committing; taking over what it left");
            break;
        case Takeover::Committed: break;
    }
    return *std::move(image);
}

} // namespace vthost::handover

#endif
