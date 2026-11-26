// SPDX-License-Identifier: Apache-2.0
#include <vtextension/wasm/remote/process/testing/SandboxProbe.hpp>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>

#include <net/if.h>
#ifdef __linux__
    #include <sys/prctl.h>
#elifdef __FreeBSD__
    #include <sys/procctl.h>
    #include <sys/procdesc.h>
    #include <sys/sockio.h>
#elifdef __APPLE__
    #include <sys/sockio.h>
    #include <sys/sys_domain.h>
    #include <sys/sysctl.h>

    #include <mach/mach.h>
    #include <servers/bootstrap.h>
#endif

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
#include <iterator>
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

#include <arpa/inet.h>
#include <netinet/in.h>
#include <vtextension/wasm/host/ExecutorThread.hpp>
#include <vtextension/wasm/host/HostBootstrap.hpp>
#include <vtextension/wasm/host/HostProcess.hpp>
#include <vtextension/wasm/host/Sandbox.hpp>
#include <vtextension/wasm/remote/proto/Pdu.hpp>
#include <vtextension/wasm/remote/transport/Channel.hpp>
#include <vtextension/wasm/remote/transport/GuestMemoryMapping.hpp>
#include <vtextension/wasm/remote/transport/HandleGrant.hpp>
#include <vtextension/wasm/remote/transport/HelperArguments.hpp>
#include <vtextension/wasm/remote/transport/PduStream.hpp>

#ifdef __linux__
    #include <linux/seccomp.h>
    #include <vtextension/wasm/host/linux/SeccompFilter.hpp>
    #include <vtextension/wasm/host/linux/SystemCallRows.hpp>
    #ifdef __aarch64__
        #include <linux/membarrier.h>
    #endif
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

    /// A file the system has, which no locked-down host may read.
#ifdef __APPLE__
    constexpr auto ReadableFile = "/etc/hosts";
#else
    constexpr auto ReadableFile = "/etc/hostname";
#endif

    /// A program every system has, which no locked-down host may run: macOS has no /bin/true.
#ifdef __APPLE__
    constexpr auto RunnableProgram = "/usr/bin/true";
#else
    constexpr auto RunnableProgram = "/bin/true";
#endif

    /// A Unix-domain socket the system listens on, which no locked-down host may reach. Linux and FreeBSD
    /// refuse before the path is looked at, so it need exist only on macOS, whose profile refuses the
    /// connect.
    constexpr auto ListeningSocketPath = std::string_view { "/var/run/syslog" };

    /// The type of the sockets the ops make: close-on-exec where the system takes it at creation.
#ifdef SOCK_CLOEXEC
    constexpr auto StreamSocket = SOCK_STREAM | SOCK_CLOEXEC;
#else
    constexpr auto StreamSocket = SOCK_STREAM;
#endif

    /// The flags of a send that must not raise SIGPIPE: none where the system has no MSG_NOSIGNAL (macOS),
    /// whose channel sockets carry SO_NOSIGPIPE instead (Channel_posix.cpp).
#ifdef MSG_NOSIGNAL
    constexpr auto SendFlags = MSG_NOSIGNAL;
#else
    constexpr auto SendFlags = 0;
#endif

    /// The stack of the probe's threads: what the host's executors get is more than enough.
    constexpr auto ThreadStackBytes = std::size_t { 1024 } * 1024;

    /// What the op's thread has to work with.
    struct OperationContext
    {
        PduStream& control;    ///< The control channel, which the permitted op takes a grant off.
        int controlDescriptor; ///< Its descriptor, which send-descriptor sends on raw.
        int parentProcess;     ///< The parent, read before the lockdown.
        /// The descriptors the sandbox was handed beside control, each with its role.
        std::span<host::HeldDescriptor const> held;
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
        return openPath(ReadableFile, O_RDONLY);
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

    [[nodiscard]] std::string socketOf(int family, int type = StreamSocket, int protocol = 0)
    {
        auto const descriptor = ::socket(family, type, protocol);
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

    /// Reaches for an address: the loopback's discard port. Where a sandbox lets a socket be made at all, it
    /// is the address that it refuses.
    [[nodiscard]] std::string connectInet(OperationContext&)
    {
        auto const descriptor = ::socket(AF_INET, StreamSocket, 0);
        if (descriptor < 0)
            return outcomeOf(descriptor);
        constexpr auto DiscardPort = uint16_t { 9 };
        auto address = sockaddr_in {};
        address.sin_family = AF_INET;
        address.sin_port = htons(DiscardPort);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        auto const outcome =
            outcomeOf(::connect(descriptor, reinterpret_cast<sockaddr const*>(&address), sizeof(address)));
        ::close(descriptor);
        return outcome;
    }

    /// Reaches for a Unix-domain socket by its path: where a sandbox lets a socket be made at all (macOS), it
    /// is the connect that it refuses.
    [[nodiscard]] std::string connectUnix(OperationContext&)
    {
        auto const descriptor = ::socket(AF_UNIX, StreamSocket, 0);
        if (descriptor < 0)
            return outcomeOf(descriptor);
        auto address = sockaddr_un {};
        address.sun_family = AF_UNIX;
        std::ranges::copy(ListeningSocketPath, std::begin(address.sun_path));
        auto const outcome =
            outcomeOf(::connect(descriptor, reinterpret_cast<sockaddr const*>(&address), sizeof(address)));
        ::close(descriptor);
        return outcome;
    }

    /// Sends a datagram to an Internet address, on a socket made for it and never connected: what a sandbox
    /// that lets a socket be made must refuse by the address.
    [[nodiscard]] std::string sendToInet(OperationContext&)
    {
        auto const descriptor = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (descriptor < 0)
            return outcomeOf(descriptor);
        constexpr auto DiscardPort = uint16_t { 9 };
        auto address = sockaddr_in {};
        address.sin_family = AF_INET;
        address.sin_port = htons(DiscardPort);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        auto const byte = std::array<std::byte, 1> {};
        auto const outcome = outcomeOf(::sendto(descriptor,
                                                byte.data(),
                                                byte.size(),
                                                SendFlags,
                                                reinterpret_cast<sockaddr const*>(&address),
                                                sizeof(address)));
        ::close(descriptor);
        return outcome;
    }

    /// Binds an Internet stream socket to the loopback and listens on it: a server of its own.
    [[nodiscard]] std::string listenInet(OperationContext&)
    {
        auto const descriptor = ::socket(AF_INET, StreamSocket, 0);
        if (descriptor < 0)
            return outcomeOf(descriptor);
        auto address = sockaddr_in {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        auto outcome =
            outcomeOf(::bind(descriptor, reinterpret_cast<sockaddr const*>(&address), sizeof(address)));
        if (outcome == "ok")
            outcome = outcomeOf(::listen(descriptor, 1));
        ::close(descriptor);
        return outcome;
    }

    /// Binds a Unix-domain socket to a path of its own: a file made, and a server reachable by it.
    [[nodiscard]] std::string bindUnix(OperationContext&)
    {
        auto const descriptor = ::socket(AF_UNIX, StreamSocket, 0);
        if (descriptor < 0)
            return outcomeOf(descriptor);
        auto const path = std::format("/tmp/contour-sandbox-probe-{}.socket", ::getpid());
        auto address = sockaddr_un {};
        address.sun_family = AF_UNIX;
        std::ranges::copy(path, std::begin(address.sun_path));
        auto const outcome =
            outcomeOf(::bind(descriptor, reinterpret_cast<sockaddr const*>(&address), sizeof(address)));
        if (outcome == "ok")
            ::unlink(path.c_str());
        ::close(descriptor);
        return outcome;
    }

    /// Makes a routing socket, which reads the system's routes and interfaces as they change.
    [[nodiscard]] std::string socketRoute(OperationContext&)
    {
        return socketOf(AF_ROUTE, SOCK_RAW, 0);
    }

#ifdef __APPLE__
    /// Makes a kernel control socket (AF_SYSTEM), through which a process talks to kernel extensions.
    [[nodiscard]] std::string socketSystem(OperationContext&)
    {
        return socketOf(AF_SYSTEM, SOCK_DGRAM, SYSPROTO_CONTROL);
    }
#endif

    /// Lists the system's network interfaces (SIOCGIFCONF) through an Internet socket made for it.
    [[nodiscard]] std::string listInterfaces(OperationContext&)
    {
        auto const descriptor = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (descriptor < 0)
            return outcomeOf(descriptor);
        auto requests = std::array<ifreq, 16> {};
        auto configuration = ifconf {};
        configuration.ifc_len = static_cast<int>(sizeof(requests));
        configuration.ifc_req = requests.data();
        auto const outcome = outcomeOf(::ioctl(descriptor, SIOCGIFCONF, &configuration));
        ::close(descriptor);
        return outcome;
    }

    /// Asks the control channel how much it holds, by an ioctl no descriptor of the host needs.
    [[nodiscard]] std::string ioctlControl(OperationContext& context)
    {
        auto pending = 0;
        return outcomeOf(::ioctl(context.controlDescriptor, FIONREAD, &pending));
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
        return outcomeOf(::sendmsg(context.controlDescriptor, &message, SendFlags));
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

#ifdef __FreeBSD__
    /// Forks as Capsicum lets a process in capability mode fork: to a process descriptor.
    [[nodiscard]] std::string forkToDescriptor(OperationContext&)
    {
        auto descriptor = -1;
        auto const child = ::pdfork(&descriptor, 0);
        if (child == 0)
            ::_exit(0);
        if (child > 0)
            ::close(descriptor);
        return outcomeOf(child);
    }
#endif

    [[nodiscard]] std::string spawnThread(OperationContext&)
    {
        auto thread = host::ExecutorThread::start(ThreadStackBytes, [] {});
        return thread ? std::string { "ok" } : std::format("refused:{}", thread.error().value());
    }

    [[nodiscard]] std::string executeProgram(OperationContext&)
    {
        auto program = std::string { RunnableProgram };
        auto arguments = std::array<char*, 2> { program.data(), nullptr };
        auto environment = std::array<char*, 1> { nullptr };
        return outcomeOf(::execve(program.c_str(), arguments.data(), environment.data()));
    }

    [[nodiscard]] std::string traceMe(OperationContext&)
    {
#if defined(__FreeBSD__) || defined(__APPLE__)
        return outcomeOf(::ptrace(PT_TRACE_ME, 0, nullptr, 0));
#else
        return outcomeOf(::ptrace(PTRACE_TRACEME, 0, nullptr, nullptr));
#endif
    }

    [[nodiscard]] std::string signalParent(OperationContext& context)
    {
        return outcomeOf(::kill(context.parentProcess, 0));
    }

    [[nodiscard]] std::string mapAnonymous(int protection, int flags)
    {
        constexpr auto Bytes = std::size_t { 4096 };
#ifdef __linux__
        // Through the system call itself: what the filter sees, without the C library's wrapper in between.
        auto const mapped = ::syscall(SYS_mmap, nullptr, Bytes, protection, flags | MAP_ANONYMOUS, -1, 0);
        if (mapped < 0)
            return outcomeOf(mapped);
        ::syscall(SYS_munmap, mapped, Bytes);
#else
        // FreeBSD's syscall() returns an int, too narrow for an address; no filter sits between it and mmap.
        auto* const mapped = ::mmap(nullptr, Bytes, protection, flags | MAP_ANONYMOUS, -1, 0);
        if (mapped == MAP_FAILED)
            return outcomeOf(-1);
        ::munmap(mapped, Bytes);
#endif
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
#ifdef __linux__
        // Through the system call itself, as mapAnonymous() maps.
        auto outcome = outcomeOf(::syscall(SYS_mprotect, mapped, Bytes, protection));
#else
        auto outcome = outcomeOf(::mprotect(mapped, Bytes, protection));
#endif
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

#ifdef __APPLE__
    /// Maps memory for code made at run time (MAP_JIT), writable only, as WAMR maps all of its memory unless
    /// patched (cmake/patches/): what the sandbox says of it, beside what the hardened runtime says.
    [[nodiscard]] std::string mapForJit(OperationContext&)
    {
        return mapAnonymous(PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_JIT);
    }
#endif

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

    /// The word held-limits says for each DescriptorLimit, at its enumerator's index.
    constexpr auto LimitWords = std::array<std::string_view, 3> { "unlimited", "as-its-role", "otherwise" };
    static_assert(LimitWords.size()
                      == std::size_t { std::to_underlying(host::DescriptorLimit::Otherwise) } + 1,
                  "LimitWords names every DescriptorLimit");

    /// Says how far the system limits each descriptor the sandbox was handed, in order, comma-separated.
    [[nodiscard]] std::string heldLimits(OperationContext& context)
    {
        auto words = std::string {};
        for (auto const& descriptor: context.held)
        {
            if (!words.empty())
                words += ',';
            words += LimitWords[std::to_underlying(host::limitOf(descriptor))];
        }
        return words;
    }

    /// @return Whether this process gains no privilege through exec: `on`, `off`, or `unknown` where the
    ///         system does not say. Asked before the lockdown, which may refuse the question.
    [[nodiscard]] std::string_view noNewPrivileges()
    {
#ifdef __linux__
        auto const status = ::prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0);
        if (status < 0)
            return "unknown";
        return status == 1 ? "on" : "off";
#elif defined(__FreeBSD__) && defined(PROC_NO_NEW_PRIVS_STATUS)
        auto status = 0;
        if (::procctl(P_PID, 0, PROC_NO_NEW_PRIVS_STATUS, &status) != 0)
            return "unknown";
        return status == PROC_NO_NEW_PRIVS_ENABLE ? "on" : "off";
#else
        return "unknown";
#endif
    }

#ifdef __APPLE__
    /// Looks up the system log's Mach service, as a library that logs does: what no locked-down host may
    /// reach. `refused:<kern_return_t>` where the lookup failed.
    [[nodiscard]] std::string lookUpMachService(OperationContext&)
    {
        auto port = mach_port_t { MACH_PORT_NULL };
        auto const result = ::bootstrap_look_up(bootstrap_port, "com.apple.system.logger", &port);
        if (result != KERN_SUCCESS)
            return std::format("refused:{}", result);
        ::mach_port_deallocate(::mach_task_self(), port);
        return "ok";
    }
#endif

    [[nodiscard]] std::string nothing(OperationContext&)
    {
        return "ok";
    }

    /// What the locked-down host does: takes a grant off control and maps it over a reservation, opens it,
    /// writes it, unmaps and closes it; asks the page size; draws random bytes, reads the clock, waits on a
    /// condition, asks for its alternate signal stack, and (aarch64) flushes the instruction caches.
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
        // The page size, as an engine that maps an instance's memory itself asks it (WAMR's os_mmap()). On
        // macOS by the sysctl the C library asks, which it may have cached before the lockdown: asked here
        // directly, so that the profile's rule for it is what answers.
#ifdef __APPLE__
        auto pageSize = 0;
        auto pageSizeLength = sizeof(pageSize);
        if (::sysctlbyname("hw.pagesize_compat", &pageSize, &pageSizeLength, nullptr, 0) != 0
            || pageSize <= 0)
            return "failed:page-size";
#else
        if (::getpagesize() <= 0)
            return "failed:page-size";
#endif
        auto random = std::array<std::byte, 8> {};
#ifdef __APPLE__
        // macOS has no getrandom(); getentropy() is what its C++ library draws on.
        if (::getentropy(random.data(), random.size()) != 0)
            return "failed:getentropy";
#else
        if (::getrandom(random.data(), random.size(), 0) != static_cast<ssize_t>(random.size()))
            return "failed:getrandom";
#endif
        auto now = timespec {};
#ifdef __linux__
        // Through the system call itself, as the filter sees it.
        if (::syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &now) != 0)
            return "failed:clock";
#else
        if (::clock_gettime(CLOCK_MONOTONIC, &now) != 0)
            return "failed:clock";
#endif
        auto mutex = std::mutex {};
        auto condition = std::condition_variable {};
        {
            auto lock = std::unique_lock { mutex };
            std::ignore = condition.wait_for(lock, std::chrono::milliseconds { 1 });
        }
        auto stack = stack_t {};
        if (::sigaltstack(nullptr, &stack) != 0)
            return "failed:sigaltstack";
#if defined(__linux__) && defined(__aarch64__)
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
        Operation { .name = "connect-inet", .run = &connectInet },
        Operation { .name = "connect-unix", .run = &connectUnix },
        Operation { .name = "sendto-inet", .run = &sendToInet },
        Operation { .name = "listen-inet", .run = &listenInet },
        Operation { .name = "bind-unix", .run = &bindUnix },
        Operation { .name = "socket-route", .run = &socketRoute },
        Operation { .name = "list-interfaces", .run = &listInterfaces },
        Operation { .name = "ioctl-control", .run = &ioctlControl },
        Operation { .name = "send-descriptor", .run = &sendDescriptor },
        Operation { .name = "fork", .run = &forkProcess },
#ifdef __FreeBSD__
        Operation { .name = "pdfork", .run = &forkToDescriptor },
#endif
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
        Operation { .name = "held-limits", .run = &heldLimits },
        Operation { .name = "permitted", .run = &permitted },
#ifdef __APPLE__
        Operation { .name = "mach-lookup", .run = &lookUpMachService },
        Operation { .name = "map-jit", .run = &mapForJit },
        Operation { .name = "socket-system", .run = &socketSystem },
#endif
    };

#ifdef __linux__
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

    /// Installs @p outer, as an outer sandbox would have before the host starts: on the process, or
    /// (`thread`) on a thread of its own, @p filtered.
    /// @return Whether it was installed.
    [[nodiscard]] bool installOuter(OuterFilter const& outer, FilteredThread& filtered)
    {
        if (outer.name == "thread")
            return filtered.start().has_value();
        auto const program = outerProgram(outer.refusals);
        return program && ::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0 && host::installFilter(*program);
    }
#else
    /// An outer sandbox's filter, by name. There is none to install where the host's sandbox filters no
    /// system call.
    struct OuterFilter
    {
        std::string_view name;
    };

    constexpr auto OuterFilters = std::array<OuterFilter, 0> {};

    /// A thread that runs a filter of its own: none here.
    class FilteredThread
    {
      public:
        void stop() noexcept {}
    };

    /// @return Never installed: OuterFilters names none.
    [[nodiscard]] bool installOuter(OuterFilter const& /*outer*/, FilteredThread& /*filtered*/)
    {
        return false;
    }

#endif

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
    // As the host does: no core dump of a process its filter kills; then every handle adopted.
    auto const hardened = host::hardenProcess();
    auto endpoints = host::adoptEndpoints(*parsed);
    if (!endpoints || endpoints->lanes.size() < 2)
        return exitWith(SandboxProbeExit::NoControl);
    // What the host keeps, as it keeps it (an engine's devices aside: the probe runs none), taken before the
    // probe takes its channels out of the endpoints. The page and the wake receiver stay held in them.
    auto const descriptors =
        host::sandboxDescriptorsOf(*endpoints, std::span<transport::NativeHandle const> {});
    auto const& held = descriptors.held;
    auto const controlDescriptor = descriptors.control;
    // Each report on a lane of its own: a reader of the first must not take in the second.
    auto lane = std::move(endpoints->lanes.at(0));
    auto opLane = std::move(endpoints->lanes.at(1));
    auto control = PduStream { std::move(endpoints->control),
                               proto::ChannelKind::Control,
                               proto::Direction::ToHelper,
                               proto::PduLimits {} };

    // The outer sandbox, there before the host starts.
    auto filtered = FilteredThread {};
    if (request->outer && !installOuter(*request->outer, filtered))
        return exitWith(SandboxProbeExit::SetupFailed);

    // Each thread opens a file once told to, between the restriction and the lockdown, then runs what it is
    // given once told to go.
    auto openNow = std::latch { 1 };
    auto opened = std::latch { 2 };
    auto finish = std::latch { 1 };
    auto beforeRestrict = std::string {};
    auto afterRestrict = std::string {};
    auto early = host::ExecutorThread::start(ThreadStackBytes, [&] {
        openNow.wait();
        beforeRestrict = openPath(ReadableFile, O_RDONLY);
        opened.count_down();
        finish.wait();
    });
    if (!early)
        return exitWith(SandboxProbeExit::SetupFailed);

    auto const sandbox = request->sandboxing == Sandboxing::System
                             ? host::makeSystemSandbox(descriptors, request->guestCode)
                             : std::unique_ptr<host::ISandbox> {};
    auto const restricted =
        sandbox ? sandbox->restrictBeforeThreads() : std::expected<void, host::ConfinementFailure> {};
    auto const privileges = noNewPrivileges();

    auto context = OperationContext {
        .control = control, .controlDescriptor = controlDescriptor, .parentProcess = ::getppid(), .held = held
    };
    auto go = std::latch { 1 };
    auto reported = std::latch { 1 };
    auto reportSent = false;
    auto executor = host::ExecutorThread::start(ThreadStackBytes, [&] {
        openNow.wait();
        afterRestrict = openPath(ReadableFile, O_RDONLY);
        opened.count_down();
        go.wait();
        auto const outcome = request->operation->run(context);
        reportSent = sendText(opLane, std::format("op={}\nend\n", outcome));
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

    // As the host does: its caps, then the lockdown; the first step that failed, its hardening's included, is
    // the one stated.
    using Confined = std::expected<void, host::ConfinementFailure>;
    auto const capped = sandbox ? host::capResources(parsed->dataBytes, host::BuildCapPolicy) : Confined {};
    auto const lockedDown = sandbox ? sandbox->lockDown() : Confined {};
    auto const lockdown = [&] {
        if (!sandbox)
            return std::string { "none" };
        for (auto const& step: { hardened, restricted, capped, lockedDown })
            if (!step)
                return failedLine(step.error());
        return std::string { "locked" };
    }();
    // What the sandbox went without, as a Hello states it: the step's number, 0 for none.
    auto const unavailable = sandbox ? std::to_underlying(sandbox->unavailableStep()) : 0;
    if (!sendText(lane,
                  std::format("lockdown={}\nno-new-privs={}\nbefore-restrict={}\nafter-restrict={}\n"
                              "unavailable={}\nend\n",
                              lockdown,
                              privileges,
                              beforeRestrict,
                              afterRestrict,
                              unavailable)))
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
