// SPDX-License-Identifier: Apache-2.0
#include <vtextension/wasm/remote/process/testing/SandboxProbe.hpp>

#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <functional>
#include <latch>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <linux/seccomp.h>
#include <vtextension/wasm/host/ExecutorThread.hpp>
#include <vtextension/wasm/host/HostBootstrap.hpp>
#include <vtextension/wasm/host/HostProcess.hpp>
#include <vtextension/wasm/host/Sandbox.hpp>
#include <vtextension/wasm/host/linux/SeccompFilter.hpp>
#include <vtextension/wasm/host/linux/SystemCallRows.hpp>
#include <vtextension/wasm/remote/proto/Pdu.hpp>
#include <vtextension/wasm/remote/transport/Channel.hpp>
#include <vtextension/wasm/remote/transport/GuestMemoryMapping.hpp>
#include <vtextension/wasm/remote/transport/HandleGrant.hpp>
#include <vtextension/wasm/remote/transport/HelperArguments.hpp>
#include <vtextension/wasm/remote/transport/PduStream.hpp>

#ifdef __aarch64__
    #include <linux/membarrier.h>
#endif

namespace vtextension::wasm::remote::process::testing
{

namespace
{
    namespace host = vtextension::wasm::host;
    using transport::Channel;
    using transport::PduStream;

    constexpr auto SandboxedOption = std::string_view { "--sandboxed" };
    constexpr auto OuterFilterOption = std::string_view { "--outer-filter=" };
    constexpr auto OperationOption = std::string_view { "--sandbox-op=" };
    constexpr auto GuestCodeOption = std::string_view { "--guest-code=" };

    /// A row set the probe can lock down with: the engine's GuestCode, by the name the option gives it.
    struct GuestCodeName
    {
        std::string_view name;
        host::GuestCode code;
    };

    constexpr auto GuestCodeNames = std::array {
        GuestCodeName { .name = "interpreted", .code = host::GuestCode::Interpreted },
        GuestCodeName { .name = "compiled", .code = host::GuestCode::Compiled },
    };

    /// Exit codes of the sandboxed mode, beside the host's for a parent that is gone.
    enum class SandboxProbeExit : uint8_t
    {
        Done = 0,
        Usage = 2,
        NoControl = 3,
        ReportFailed = 4,
        SetupFailed = 5,
    };

    /// The stack of the probe's threads: what the host's executors get is more than enough.
    constexpr auto ThreadStackBytes = std::size_t { 1024 } * 1024;

    /// What the op's thread has to work with.
    struct OperationContext
    {
        PduStream& control;    ///< The control channel, which the permitted op takes a grant off.
        int controlDescriptor; ///< Its descriptor, which send-descriptor sends on raw.
        int parentProcess;     ///< The parent, read before the lockdown.
    };

    /// @return `ok` for a call that returned @p result, `refused:<errno>` for one that failed.
    [[nodiscard]] std::string outcomeOf(long result)
    {
        return result < 0 ? std::format("refused:{}", errno) : std::string { "ok" };
    }

    /// Opens @p path read-only, and closes it if it opened.
    [[nodiscard]] std::string openPath(char const* path, int flags)
    {
        auto const descriptor = ::open(path, flags | O_CLOEXEC);
        auto outcome = outcomeOf(descriptor);
        if (descriptor >= 0)
            ::close(descriptor);
        return outcome;
    }

    [[nodiscard]] std::string openFile(OperationContext&)
    {
        return openPath("/etc/hostname", O_RDONLY);
    }

    [[nodiscard]] std::string openDirectory(OperationContext&)
    {
        return openPath("/", O_RDONLY | O_DIRECTORY);
    }

    /// What a sanitizer's report reads: its own process's maps.
    [[nodiscard]] std::string openOwnProcess(OperationContext&)
    {
        return openPath("/proc/self/maps", O_RDONLY);
    }

    /// Another process's entry, which no build's host may read.
    [[nodiscard]] std::string openOtherProcess(OperationContext& context)
    {
        return openPath(std::format("/proc/{}/maps", context.parentProcess).c_str(), O_RDONLY);
    }

    [[nodiscard]] std::string socketOf(int family)
    {
        auto const descriptor = ::socket(family, SOCK_STREAM | SOCK_CLOEXEC, 0);
        auto outcome = outcomeOf(descriptor);
        if (descriptor >= 0)
            ::close(descriptor);
        return outcome;
    }

    [[nodiscard]] std::string socketInet(OperationContext&)
    {
        return socketOf(AF_INET);
    }

    [[nodiscard]] std::string socketUnix(OperationContext&)
    {
        return socketOf(AF_UNIX);
    }

    /// Sends a byte to Contour with this process's stderr attached, as a helper handing a descriptor up
    /// would.
    [[nodiscard]] std::string sendDescriptor(OperationContext& context)
    {
        auto byte = std::array<std::byte, 1> {};
        auto vector = iovec { .iov_base = byte.data(), .iov_len = byte.size() };
        alignas(cmsghdr) auto buffer = std::array<std::byte, CMSG_SPACE(sizeof(int))> {};
        auto message = msghdr {};
        message.msg_iov = &vector;
        message.msg_iovlen = 1;
        message.msg_control = buffer.data();
        message.msg_controllen = buffer.size();
        auto* const header = CMSG_FIRSTHDR(&message);
        header->cmsg_level = SOL_SOCKET;
        header->cmsg_type = SCM_RIGHTS;
        header->cmsg_len = CMSG_LEN(sizeof(int));
        auto const attached = STDERR_FILENO;
        std::memcpy(CMSG_DATA(header), &attached, sizeof(attached));
        return outcomeOf(::sendmsg(context.controlDescriptor, &message, MSG_NOSIGNAL));
    }

    [[nodiscard]] std::string forkProcess(OperationContext&)
    {
        auto const child = ::fork();
        if (child == 0)
            ::_exit(0);
        if (child > 0)
            ::waitpid(child, nullptr, 0);
        return outcomeOf(child);
    }

    [[nodiscard]] std::string spawnThread(OperationContext&)
    {
        auto thread = host::ExecutorThread::start(ThreadStackBytes, [] {});
        return thread ? std::string { "ok" } : std::format("refused:{}", thread.error().value());
    }

    [[nodiscard]] std::string executeProgram(OperationContext&)
    {
        auto program = std::string { "/bin/true" };
        auto arguments = std::array<char*, 2> { program.data(), nullptr };
        auto environment = std::array<char*, 1> { nullptr };
        return outcomeOf(::execve(program.c_str(), arguments.data(), environment.data()));
    }

    [[nodiscard]] std::string traceMe(OperationContext&)
    {
        return outcomeOf(::ptrace(PTRACE_TRACEME, 0, nullptr, nullptr));
    }

    [[nodiscard]] std::string signalParent(OperationContext& context)
    {
        return outcomeOf(::kill(context.parentProcess, 0));
    }

    [[nodiscard]] std::string mapAnonymous(int protection, int flags)
    {
        constexpr auto Bytes = std::size_t { 4096 };
        // Through the system call itself: what the filter sees, without the C library's wrapper in between.
        auto const mapped = ::syscall(SYS_mmap, nullptr, Bytes, protection, flags | MAP_ANONYMOUS, -1, 0);
        if (mapped < 0)
            return outcomeOf(mapped);
        ::syscall(SYS_munmap, mapped, Bytes);
        return "ok";
    }

    [[nodiscard]] std::string mapWritableExecutable(OperationContext&)
    {
        return mapAnonymous(PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE);
    }

    [[nodiscard]] std::string mapSharedUnfixed(OperationContext&)
    {
        return mapAnonymous(PROT_READ | PROT_WRITE, MAP_SHARED);
    }

    /// What an engine that compiles does: maps memory writable, then makes it @p protection.
    [[nodiscard]] std::string protectAnonymous(int protection)
    {
        constexpr auto Bytes = std::size_t { 4096 };
        auto* const mapped =
            ::mmap(nullptr, Bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapped == MAP_FAILED)
            return outcomeOf(-1);
        auto outcome = outcomeOf(::syscall(SYS_mprotect, mapped, Bytes, protection));
        ::munmap(mapped, Bytes);
        return outcome;
    }

    [[nodiscard]] std::string protectWritableExecutable(OperationContext&)
    {
        return protectAnonymous(PROT_READ | PROT_WRITE | PROT_EXEC);
    }

    /// Compiled code turning executable once written: read and execute, never write.
    [[nodiscard]] std::string protectExecutable(OperationContext&)
    {
        return protectAnonymous(PROT_READ | PROT_EXEC);
    }

    /// What every engine's host does as guest memory opens: no execute bit.
    [[nodiscard]] std::string protectReadable(OperationContext&)
    {
        return protectAnonymous(PROT_READ);
    }

    [[nodiscard]] std::string mapExecutable(OperationContext&)
    {
        return mapAnonymous(PROT_READ | PROT_EXEC, MAP_PRIVATE);
    }

    [[nodiscard]] std::string channelPair(OperationContext&)
    {
        auto const pair = transport::makeChannelPair();
        if (!pair)
            return "failed:channel-pair";
        return pair->upward == transport::UpwardDescriptors::Refused ? "upward:refused" : "upward:discarded";
    }

    [[nodiscard]] std::string nothing(OperationContext&)
    {
        return "ok";
    }

    /// What the locked-down host does: takes a grant off control and maps it over a reservation, opens it,
    /// writes it, unmaps and closes it; draws random bytes, reads the clock, waits on a condition, asks for
    /// its alternate signal stack, and (aarch64) flushes the instruction caches.
    [[nodiscard]] std::string permitted(OperationContext& context)
    {
        auto const request = context.control.receive();
        auto const* const instantiate = request ? std::get_if<proto::Instantiate>(&*request) : nullptr;
        if (!instantiate)
            return "failed:instantiate";
        {
            auto handle = transport::claimGrantedHandle(context.control, instantiate->memory.handle);
            if (!handle)
                return "failed:claim";
            auto mapping = transport::GuestMemoryMapping::map(
                handle->get(), instantiate->memory.objectBytes, instantiate->memory.initialBytes, 0);
            if (!mapping)
                return "failed:map";
            std::ignore = std::move(*handle).adopt();
            constexpr auto Mark = std::string_view { "probe" };
            std::ranges::copy(std::as_bytes(std::span { Mark }), mapping->base());
        }
        auto random = std::array<std::byte, 8> {};
        if (::getrandom(random.data(), random.size(), 0) != static_cast<ssize_t>(random.size()))
            return "failed:getrandom";
        auto now = timespec {};
        if (::syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &now) != 0)
            return "failed:clock";
        auto mutex = std::mutex {};
        auto condition = std::condition_variable {};
        {
            auto lock = std::unique_lock { mutex };
            std::ignore = condition.wait_for(lock, std::chrono::milliseconds { 1 });
        }
        auto stack = stack_t {};
        if (::sigaltstack(nullptr, &stack) != 0)
            return "failed:sigaltstack";
#ifdef __aarch64__
        if (::syscall(SYS_membarrier, MEMBARRIER_CMD_QUERY, 0, 0) < 0)
            return "failed:membarrier";
#endif
        return "ok";
    }

    /// One op the probe can try.
    struct Operation
    {
        std::string_view name;
        std::string (*run)(OperationContext& context);
    };

    constexpr auto Operations = std::array {
        Operation { .name = "none", .run = &nothing },
        Operation { .name = "open-file", .run = &openFile },
        Operation { .name = "open-directory", .run = &openDirectory },
        Operation { .name = "open-own-process", .run = &openOwnProcess },
        Operation { .name = "open-other-process", .run = &openOtherProcess },
        Operation { .name = "socket-inet", .run = &socketInet },
        Operation { .name = "socket-unix", .run = &socketUnix },
        Operation { .name = "send-descriptor", .run = &sendDescriptor },
        Operation { .name = "fork", .run = &forkProcess },
        Operation { .name = "spawn-thread", .run = &spawnThread },
        Operation { .name = "execve", .run = &executeProgram },
        Operation { .name = "ptrace-traceme", .run = &traceMe },
        Operation { .name = "signal-parent", .run = &signalParent },
        Operation { .name = "map-writable-executable", .run = &mapWritableExecutable },
        Operation { .name = "map-shared-unfixed", .run = &mapSharedUnfixed },
        Operation { .name = "protect-writable-executable", .run = &protectWritableExecutable },
        Operation { .name = "map-executable", .run = &mapExecutable },
        Operation { .name = "protect-executable", .run = &protectExecutable },
        Operation { .name = "protect-readable", .run = &protectReadable },
        Operation { .name = "channel-pair", .run = &channelPair },
        Operation { .name = "permitted", .run = &permitted },
    };

    /// An outer sandbox's filter: what it refuses, and with which errno; only when the call's arguments pass
    /// `checks`, if any.
    struct OuterRefusal
    {
        host::SystemCall call;
        int error;
        std::span<host::ArgumentCheck const> checks = {};
    };

    /// The outer filters the probe can install before it sandboxes itself.
    struct OuterFilter
    {
        std::string_view name;
        std::span<OuterRefusal const> refusals;
    };

    constexpr auto NoLandlock = std::array {
        OuterRefusal { .call = host::SystemCall::LandlockCreateRuleset, .error = ENOSYS },
        OuterRefusal { .call = host::SystemCall::LandlockAddRule, .error = ENOSYS },
        OuterRefusal { .call = host::SystemCall::LandlockRestrictSelf, .error = ENOSYS },
    };
    // What Flatpak's bwrap refuses that the host might meet (flatpak-run.c's seccomp list, in part).
    constexpr auto FlatpakLike = std::array {
        OuterRefusal { .call = host::SystemCall::Clone3, .error = ENOSYS },
        OuterRefusal { .call = host::SystemCall::Unshare, .error = EPERM },
        OuterRefusal { .call = host::SystemCall::Setns, .error = EPERM },
        OuterRefusal { .call = host::SystemCall::Mount, .error = EPERM },
        OuterRefusal { .call = host::SystemCall::Personality, .error = EPERM },
        OuterRefusal { .call = host::SystemCall::Ptrace, .error = EPERM },
    };
    /// SO_PASSRIGHTS, which older headers lack.
    constexpr auto PassRightsOption = uint64_t { 83 };
    constexpr auto SettingPassRights = std::array {
        host::ArgumentCheck {
            .index = 2, .test = host::ArgumentTest::Equals, .mask = 0xFFFF'FFFF, .value = PassRightsOption },
    };
    constexpr auto NoPassRights = std::array {
        OuterRefusal { .call = host::SystemCall::Setsockopt, .error = EPERM, .checks = SettingPassRights },
    };

    constexpr auto OuterFilters = std::array {
        OuterFilter { .name = "no-landlock", .refusals = NoLandlock },
        OuterFilter { .name = "flatpak", .refusals = FlatpakLike },
        OuterFilter { .name = "passrights", .refusals = NoPassRights },
        OuterFilter { .name = "thread", .refusals = {} },
    };

    /// @return A filter refusing @p refusals and allowing all else, for this architecture.
    [[nodiscard]] std::expected<std::vector<sock_filter>, host::ConfinementFailure> outerProgram(
        std::span<OuterRefusal const> refusals)
    {
        auto rows = std::vector<host::SystemCallRow> {};
        for (auto const& refusal: refusals)
            if (auto const number = host::systemCallNumber(refusal.call, host::ThisAuditArch))
                rows.push_back(host::SystemCallRow { .number = *number,
                                                     .checks = refusal.checks,
                                                     .action = host::SystemCallAction::Refuse,
                                                     .error = refusal.error,
                                                     .reason = "the outer sandbox's" });
        return host::assembleFilter(rows,
                                    host::SandboxDescriptors { .control = -1 },
                                    ::getpid(),
                                    host::ThisAuditArch,
                                    host::SystemCallAction::Allow);
    }

    /// Installs @p program on the calling thread only, as a thread that filters itself does.
    /// @return Nothing once installed; the errno otherwise.
    [[nodiscard]] std::expected<void, int> installOnThisThread(std::vector<sock_filter>& program)
    {
        auto const filter =
            sock_fprog { .len = static_cast<unsigned short>(program.size()), .filter = program.data() };
        if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0
            || ::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &filter) != 0)
            return std::unexpected(errno);
        return {};
    }

    /// A thread that runs a filter of its own until the probe ends.
    class FilteredThread
    {
      public:
        /// Starts it, and waits until it installed its filter.
        /// @return Nothing once it did; the errno otherwise.
        [[nodiscard]] std::expected<void, int> start()
        {
            auto installed = std::latch { 1 };
            auto outcome = std::expected<void, int> {};
            auto thread = host::ExecutorThread::start(ThreadStackBytes, [this, &installed, &outcome] {
                auto program = outerProgram({});
                outcome = program ? installOnThisThread(*program) : std::unexpected(program.error().code);
                installed.count_down();
                _done.wait();
            });
            if (!thread)
                return std::unexpected(thread.error().value());
            installed.wait();
            _thread = std::move(*thread);
            return outcome;
        }

        void stop() noexcept
        {
            _done.count_down();
            if (_thread)
                _thread->join();
        }

      private:
        std::latch _done { 1 };
        std::optional<host::ExecutorThread> _thread;
    };

    /// Sends @p text on @p channel.
    [[nodiscard]] bool sendText(Channel& channel, std::string_view text)
    {
        return channel.send(std::span { reinterpret_cast<uint8_t const*>(text.data()), text.size() })
            .has_value();
    }

    /// @return The lockdown line's value for @p failure.
    [[nodiscard]] std::string failedLine(host::ConfinementFailure const& failure)
    {
        return std::format("failed:{}:{}", std::to_underlying(failure.step), failure.code);
    }

    /// Whether the probe sandboxes itself.
    enum class Sandboxing : uint8_t
    {
        None,   ///< Not: only an outer filter, if any, confines it.
        System, ///< As the host does: makeSystemSandbox().
    };

    /// What the probe was asked for.
    struct Request
    {
        Sandboxing sandboxing = Sandboxing::None;
        OuterFilter const* outer = nullptr;
        Operation const* operation = &Operations.front();
        host::GuestCode guestCode = host::GuestCode::Compiled;
        std::vector<std::string_view> helperArguments;
    };

    [[nodiscard]] std::optional<Request> parse(std::span<std::string_view const> arguments)
    {
        auto request = Request {};
        for (auto const argument: arguments)
        {
            if (argument == SandboxedOption)
                request.sandboxing = Sandboxing::System;
            else if (argument.starts_with(OuterFilterOption))
            {
                auto const name = argument.substr(OuterFilterOption.size());
                auto const found = std::ranges::find(OuterFilters, name, &OuterFilter::name);
                if (found == OuterFilters.end())
                    return std::nullopt;
                request.outer = &*found;
            }
            else if (argument.starts_with(OperationOption))
            {
                auto const name = argument.substr(OperationOption.size());
                auto const found = std::ranges::find(Operations, name, &Operation::name);
                if (found == Operations.end())
                    return std::nullopt;
                request.operation = &*found;
            }
            else if (argument.starts_with(GuestCodeOption))
            {
                auto const name = argument.substr(GuestCodeOption.size());
                auto const found = std::ranges::find(GuestCodeNames, name, &GuestCodeName::name);
                if (found == GuestCodeNames.end())
                    return std::nullopt;
                request.guestCode = found->code;
            }
            else
                request.helperArguments.push_back(argument);
        }
        return request;
    }

    [[nodiscard]] int exitWith(SandboxProbeExit exit)
    {
        return static_cast<int>(exit);
    }
} // namespace

bool asksForSandboxProbe(std::span<std::string_view const> arguments) noexcept
{
    return std::ranges::any_of(arguments, [](std::string_view argument) {
        return argument == SandboxedOption || argument.starts_with(OuterFilterOption)
               || argument.starts_with(OperationOption) || argument.starts_with(GuestCodeOption);
    });
}

int runSandboxProbe(std::span<std::string_view const> arguments)
{
    auto const request = parse(arguments);
    if (!request)
        return exitWith(SandboxProbeExit::Usage);
    auto const parsed = transport::parseHelperArguments(request->helperArguments);
    if (!parsed)
        return exitWith(SandboxProbeExit::Usage);
    if (!host::bindToParent(parsed->parentProcessId))
        return host::exitCodeOf(host::HostProcessExit::ParentGone);
    // As the host does: no core dump of a process its filter kills.
    std::ignore = host::hardenProcess();
    auto controlEnd = Channel::adopt(parsed->control);
    // Each report on a lane of its own: a reader of the first must not take in the second.
    auto lane = Channel::adopt(parsed->lanes.at(0));
    auto opLane = Channel::adopt(parsed->lanes.at(1));
    if (!controlEnd || !lane || !opLane)
        return exitWith(SandboxProbeExit::NoControl);
    auto const controlDescriptor = controlEnd->nativeHandle();
    auto control = PduStream {
        *std::move(controlEnd), proto::ChannelKind::Control, proto::Direction::ToHelper, proto::PduLimits {}
    };

    // The outer sandbox, there before the host starts.
    auto filtered = FilteredThread {};
    if (request->outer && request->outer->name == "thread")
    {
        if (!filtered.start())
            return exitWith(SandboxProbeExit::SetupFailed);
    }
    else if (request->outer)
    {
        auto const program = outerProgram(request->outer->refusals);
        if (!program || ::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 || !host::installFilter(*program))
            return exitWith(SandboxProbeExit::SetupFailed);
    }

    // Each thread opens a file once told to, between the restriction and the lockdown, then runs what it is
    // given once told to go.
    auto openNow = std::latch { 1 };
    auto opened = std::latch { 2 };
    auto finish = std::latch { 1 };
    auto beforeRestrict = std::string {};
    auto afterRestrict = std::string {};
    auto early = host::ExecutorThread::start(ThreadStackBytes, [&] {
        openNow.wait();
        beforeRestrict = openPath("/etc/hostname", O_RDONLY);
        opened.count_down();
        finish.wait();
    });
    if (!early)
        return exitWith(SandboxProbeExit::SetupFailed);

    auto const sandbox =
        request->sandboxing == Sandboxing::System
            ? host::makeSystemSandbox(host::SandboxDescriptors { .control = controlDescriptor },
                                      request->guestCode)
            : std::unique_ptr<host::ISandbox> {};
    auto const restricted =
        sandbox ? sandbox->restrictBeforeThreads() : std::expected<void, host::ConfinementFailure> {};

    auto context = OperationContext { .control = control,
                                      .controlDescriptor = controlDescriptor,
                                      .parentProcess = ::getppid() };
    auto go = std::latch { 1 };
    auto reported = std::latch { 1 };
    auto reportSent = false;
    auto executor = host::ExecutorThread::start(ThreadStackBytes, [&] {
        openNow.wait();
        afterRestrict = openPath("/etc/hostname", O_RDONLY);
        opened.count_down();
        go.wait();
        auto const outcome = request->operation->run(context);
        reportSent = sendText(*opLane, std::format("op={}\nend\n", outcome));
        reported.count_down();
        // Until Contour ends the connection.
        while (control.receive())
        {
        }
    });
    if (!executor)
        return exitWith(SandboxProbeExit::SetupFailed);
    openNow.count_down();
    opened.wait();

    auto const lockedDown = sandbox ? sandbox->lockDown() : std::expected<void, host::ConfinementFailure> {};
    auto const lockdown = [&] {
        if (!sandbox)
            return std::string { "none" };
        if (!restricted)
            return failedLine(restricted.error());
        if (!lockedDown)
            return failedLine(lockedDown.error());
        return std::string { "locked" };
    }();
    if (!sendText(*lane,
                  std::format("lockdown={}\nbefore-restrict={}\nafter-restrict={}\nend\n",
                              lockdown,
                              beforeRestrict,
                              afterRestrict)))
        return exitWith(SandboxProbeExit::ReportFailed);

    go.count_down();
    reported.wait();
    executor->join();
    finish.count_down();
    early->join();
    filtered.stop();
    return exitWith(reportSent ? SandboxProbeExit::Done : SandboxProbeExit::ReportFailed);
}

} // namespace vtextension::wasm::remote::process::testing
