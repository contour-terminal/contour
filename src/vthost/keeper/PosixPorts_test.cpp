// SPDX-License-Identifier: Apache-2.0
#include <core/testing/ScopedTempDir.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <vthost/keeper/Codec.hpp>
#include <vthost/keeper/DescriptorSocket.hpp>
#include <vthost/keeper/Frame.hpp>
#include <vthost/keeper/PosixPorts.hpp>

#ifdef __linux__
    #include <sys/syscall.h>
#endif

using namespace vthost::keeper;
using namespace std::chrono_literals;

namespace
{

/// @return The first of /usr/bin/@p name and /bin/@p name that exists.
std::filesystem::path program(std::string const& name)
{
    for (auto const& directory: { "/usr/bin", "/bin" })
        if (auto path = std::filesystem::path { directory } / name; std::filesystem::exists(path))
            return path;
    FAIL("no " << name << " in /usr/bin or /bin");
    return {};
}

PosixKeeperPorts makePorts(ProcessWatch watch = ProcessWatch::ProcessHandle,
                           std::vector<std::string> daemonArguments = {})
{
    auto ports = makePosixKeeperPorts({ .daemonArguments = std::move(daemonArguments),
                                        .environment = { "PATH=/usr/bin:/bin" },
                                        .processWatch = watch });
    REQUIRE(ports.has_value());
    return std::move(*ports);
}

/// Waits on @p events until @p done holds for what arrived, or five seconds passed.
std::vector<Event> collectUntil(EventSource& events,
                                std::function<bool(std::vector<Event> const&)> const& done)
{
    auto collected = std::vector<Event> {};
    auto const giveUp = std::chrono::steady_clock::now() + 5s;
    while (!done(collected) && std::chrono::steady_clock::now() < giveUp)
        for (auto& event: events.wait(std::chrono::steady_clock::now() + 100ms))
            if (event.kind != EventKind::Timer)
                collected.push_back(std::move(event));
    return collected;
}

/// @return The end of @p pid, once the event source reported it.
std::optional<WireExitStatus> waitForEnd(EventSource& events, int pid)
{
    auto const ended = [pid](Event const& event) {
        return event.kind == EventKind::Ended && event.pid == pid;
    };
    auto const collected =
        collectUntil(events, [&](auto const& seen) { return std::ranges::any_of(seen, ended); });
    auto const found = std::ranges::find_if(collected, ended);
    if (found == collected.end())
        return std::nullopt;
    return found->status;
}

/// @return Whether @p pid is a child of this process that has ended and not been reaped.
bool isUnreapedZombie(int pid)
{
    auto info = siginfo_t {};
    return ::waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOHANG | WNOWAIT) == 0
           && info.si_pid == pid;
}

/// @return Whether @p pid is no child of this process any more: reaped.
bool isReaped(int pid)
{
    auto info = siginfo_t {};
    return ::waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOHANG) == -1 && errno == ECHILD;
}

struct Pipe
{
    UniqueFd reader;
    UniqueFd writer;
};

Pipe makePipe()
{
    auto ends = std::array<int, 2> {};
    REQUIRE(::pipe(ends.data()) == 0);
    for (auto const fd: ends)
        ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    return { .reader = UniqueFd { ends[0] }, .writer = UniqueFd { ends[1] } };
}

struct SocketPair
{
    UniqueFd keeper;
    UniqueFd daemon;
};

SocketPair makeSocketPair()
{
    auto ends = std::array<int, 2> {};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, ends.data()) == 0);
    for (auto const fd: ends)
        ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    return { .keeper = UniqueFd { ends[0] }, .daemon = UniqueFd { ends[1] } };
}

/// Sends @p bytes on @p socket in one sendmsg, with @p fds riding on the first byte.
void sendRaw(int socket, std::vector<uint8_t> const& bytes, std::vector<int> const& fds)
{
    auto iov = ::iovec { .iov_base = const_cast<uint8_t*>(bytes.data()), .iov_len = bytes.size() };
    auto control = std::vector<char>(CMSG_SPACE(sizeof(int) * std::max<std::size_t>(fds.size(), 1)));
    auto message = ::msghdr {};
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    if (!fds.empty())
    {
        message.msg_control = control.data();
        message.msg_controllen =
            static_cast<decltype(message.msg_controllen)>(CMSG_SPACE(sizeof(int) * fds.size()));
        auto* header = CMSG_FIRSTHDR(&message);
        header->cmsg_level = SOL_SOCKET;
        header->cmsg_type = SCM_RIGHTS;
        header->cmsg_len = static_cast<decltype(header->cmsg_len)>(CMSG_LEN(sizeof(int) * fds.size()));
        std::memcpy(CMSG_DATA(header), fds.data(), sizeof(int) * fds.size());
    }
    REQUIRE(::sendmsg(socket, &message, 0) == static_cast<ssize_t>(bytes.size()));
}

/// @return How many descriptors this process has open.
std::size_t openDescriptorCount()
{
#ifdef __linux__
    auto const listing = std::filesystem::path { "/proc/self/fd" };
#else
    auto const listing = std::filesystem::path { "/dev/fd" };
#endif
    // Less the one the listing itself holds open.
    return static_cast<std::size_t>(std::ranges::distance(std::filesystem::directory_iterator { listing }))
           - 1;
}

#ifdef __linux__
/// @return The descriptors @p pid has open.
std::set<int> descriptorsOf(int pid)
{
    auto numbers = std::set<int> {};
    auto error = std::error_code {};
    for (auto const& entry: std::filesystem::directory_iterator { std::format("/proc/{}/fd", pid), error })
        numbers.insert(std::stoi(entry.path().filename().string()));
    return numbers;
}

/// @return Whether @p pid waits in nanosleep() (or clock_nanosleep()), as sleep(1) does once it started.
bool isSleeping(int pid)
{
    auto stream = std::ifstream { std::format("/proc/{}/syscall", pid) };
    auto number = -1L;
    stream >> number;
    return number == SYS_nanosleep || number == SYS_clock_nanosleep;
}

/// @return The value of the line of /proc/@p pid/@p file that starts with @p key, trimmed.
std::string procLine(int pid, std::string const& file, std::string const& key)
{
    auto stream = std::ifstream { std::format("/proc/{}/{}", pid, file) };
    for (auto line = std::string {}; std::getline(stream, line);)
        if (line.starts_with(key))
        {
            auto value = line.substr(key.size());
            value.erase(0, value.find_first_not_of(" \t"));
            return value;
        }
    return {};
}
#endif

Frame frameOf(Message const& message, std::vector<int> fds = {})
{
    return Frame { .body = encode(message), .fds = std::move(fds) };
}

/// Restores a signal's disposition when it goes.
class SignalDisposition
{
  public:
    SignalDisposition(int signal, void (*handler)(int)): _signal { signal }
    {
        struct sigaction action {};
        action.sa_handler = handler;
        ::sigaction(signal, &action, &_previous);
    }
    SignalDisposition(SignalDisposition const&) = delete;
    SignalDisposition& operator=(SignalDisposition const&) = delete;
    SignalDisposition(SignalDisposition&&) = delete;
    SignalDisposition& operator=(SignalDisposition&&) = delete;
    ~SignalDisposition() { ::sigaction(_signal, &_previous, nullptr); }

  private:
    int _signal;
    struct sigaction _previous {};
};

} // namespace

TEST_CASE("the spawner runs a program with exactly the given descriptors", "[keeper][posix]")
{
#ifndef __linux__
    SKIP("reads the child's descriptors from /proc");
#else
    auto ports = makePorts();
    auto const output = makePipe();
    auto const input = UniqueFd { ::open("/dev/null", O_RDONLY | O_CLOEXEC) };
    // Open in this process and inheritable, yet no descriptor of the child's.
    auto const stray = UniqueFd { ::dup(input.get()) };
    REQUIRE(stray.isOpen());
    auto const sleep = program("sleep");
    auto const request = Spawn { .request = 1,
                                 .argv = { "sleep", "30" },
                                 .executablePath = sleep.string(),
                                 .childFds = { 0, 7 },
                                 .kept = {} };
    auto const descriptors = std::array { input.get(), output.writer.get() };

    auto const child = ports.spawner->spawn(request, descriptors);
    REQUIRE(child.has_value());
    // The program opens and closes files as it starts (its libraries, the locale); once it sleeps, what it
    // has open is what it was given.
    auto const giveUp = std::chrono::steady_clock::now() + 2s;
    while (!isSleeping(child->pid) && std::chrono::steady_clock::now() < giveUp)
        std::this_thread::sleep_for(10ms);
    CHECK(descriptorsOf(child->pid) == std::set<int> { 0, 7 });
    CHECK(std::filesystem::read_symlink(std::format("/proc/{}/fd/7", child->pid))
          == std::filesystem::read_symlink(std::format("/proc/self/fd/{}", output.writer.get())));
    CHECK(child->processHandle.isOpen());

    ports.events->watchPid(child->pid, Reaping::OnEnd);
    ports.signaller->send(child->pid, KeeperSignal::Kill);
    CHECK(waitForEnd(*ports.events, child->pid).has_value());
#endif
}

TEST_CASE("the child gets a clean signal mask and the original descriptor limit", "[keeper][posix]")
{
#ifndef __linux__
    SKIP("reads the child's signal state and limits from /proc");
#else
    // A soft limit below the hard one, so that raising it shows.
    auto limit = rlimit {};
    REQUIRE(::getrlimit(RLIMIT_NOFILE, &limit) == 0);
    auto const before = limit;
    if (limit.rlim_cur == limit.rlim_max)
        limit.rlim_cur = std::min<rlim_t>(limit.rlim_max, 512);
    REQUIRE(::setrlimit(RLIMIT_NOFILE, &limit) == 0);
    auto blocked = sigset_t {};
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGUSR1);
    auto previousMask = sigset_t {};
    REQUIRE(::pthread_sigmask(SIG_BLOCK, &blocked, &previousMask) == 0);
    auto const ignored = SignalDisposition { SIGUSR2, SIG_IGN };

    {
        auto ports = makePorts();
        auto raised = rlimit {};
        REQUIRE(::getrlimit(RLIMIT_NOFILE, &raised) == 0);
        CHECK(raised.rlim_cur == raised.rlim_max);

        auto const request = Spawn { .request = 1,
                                     .argv = { "sleep", "30" },
                                     .executablePath = program("sleep").string(),
                                     .childFds = {},
                                     .kept = {} };
        auto const child = ports.spawner->spawn(request, {});
        REQUIRE(child.has_value());
        CHECK(procLine(child->pid, "status", "SigBlk:") == "0000000000000000");
        CHECK(procLine(child->pid, "status", "SigIgn:") == "0000000000000000");
        CHECK(procLine(child->pid, "limits", "Max open files")
                  .starts_with(std::to_string(limit.rlim_cur) + " "));

        ports.events->watchPid(child->pid, Reaping::OnEnd);
        ports.signaller->send(child->pid, KeeperSignal::Kill);
        CHECK(waitForEnd(*ports.events, child->pid).has_value());
    }

    ::pthread_sigmask(SIG_SETMASK, &previousMask, nullptr);
    ::setrlimit(RLIMIT_NOFILE, &before);
#endif
}

TEST_CASE("the reaper reports a signalled child", "[keeper][posix]")
{
    auto ports = makePorts();
    auto const request = Spawn { .request = 1,
                                 .argv = { "sleep", "30" },
                                 .executablePath = program("sleep").string(),
                                 .childFds = {},
                                 .kept = {} };
    auto const child = ports.spawner->spawn(request, {});
    REQUIRE(child.has_value());
    ports.events->watchPid(child->pid, Reaping::OnEnd);
    ports.signaller->send(child->pid, KeeperSignal::Terminate);

    auto const status = waitForEnd(*ports.events, child->pid);
    REQUIRE(status.has_value());
    CHECK(*status == WireExitStatus { .kind = ExitKind::Signal, .value = SIGTERM });
    CHECK(isReaped(child->pid));
}

TEST_CASE("the pidfd-less fallback reaps too", "[keeper][posix]")
{
    auto ports = makePorts(ProcessWatch::SignalOnly);
    auto const request = Spawn { .request = 1,
                                 .argv = { "sh", "-c", "exit 7" },
                                 .executablePath = program("sh").string(),
                                 .childFds = {},
                                 .kept = {} };
    auto const child = ports.spawner->spawn(request, {});
    REQUIRE(child.has_value());
    CHECK(!child->processHandle.isOpen());
    ports.events->watchPid(child->pid, Reaping::OnEnd);

    auto const status = waitForEnd(*ports.events, child->pid);
    REQUIRE(status.has_value());
    CHECK(*status == WireExitStatus { .kind = ExitKind::Normal, .value = 7 });
    CHECK(isReaped(child->pid));
}

TEST_CASE("a stopped child has not ended; its kill ends it", "[keeper][posix]")
{
    // A stopped daemon is the heartbeat's to judge as hung, never an end to restart it on.
    auto const watch = GENERATE(ProcessWatch::ProcessHandle, ProcessWatch::SignalOnly);
    auto ports = makePorts(watch);
    auto const request = Spawn { .request = 1,
                                 .argv = { "sleep", "30" },
                                 .executablePath = program("sleep").string(),
                                 .childFds = {},
                                 .kept = {} };
    auto const child = ports.spawner->spawn(request, {}, std::nullopt);
    REQUIRE(child.has_value());
    ports.events->watchPid(WatchedProcess {
        .pid = child->pid, .role = WatchedRole::Session, .processHandle = child->processHandle.get() });

    REQUIRE(::kill(child->pid, SIGSTOP) == 0);
    auto const ended = [&](Event const& event) {
        return event.kind == EventKind::Ended && event.pid == child->pid;
    };
    auto seen = std::vector<Event> {};
    auto const quietUntil = std::chrono::steady_clock::now() + 500ms;
    while (std::chrono::steady_clock::now() < quietUntil)
        for (auto& event: ports.events->wait(std::chrono::steady_clock::now() + 100ms))
            seen.push_back(std::move(event));
    CHECK_FALSE(std::ranges::any_of(seen, ended));

    REQUIRE(::kill(child->pid, SIGKILL) == 0);
    auto const status = reapEnded(*ports.events, child->pid);
    REQUIRE(status.has_value());
    CHECK(*status == WireExitStatus { .kind = ExitKind::Signal, .value = SIGKILL });
}

TEST_CASE("a daemon stays a signalable zombie until the keeper reaps it", "[keeper][posix]")
{
    auto const watch = GENERATE(ProcessWatch::ProcessHandle, ProcessWatch::SignalOnly);
    auto ports = makePorts(watch, { "-c", "exit 3" });
    auto launched = ports.launcher->launch(program("sh"));
    REQUIRE(launched.has_value());
    ports.events->watchChannel(*launched->channel, DaemonGeneration { 1 });
    ports.events->watchPid(launched->pid, Reaping::ByKeeper);

    auto const status = waitForEnd(*ports.events, launched->pid);
    REQUIRE(status.has_value());
    CHECK(*status == WireExitStatus { .kind = ExitKind::Normal, .value = 3 });
    CHECK(isUnreapedZombie(launched->pid));
    // Still the keeper's to signal: the zombie holds the pid.
    ports.signaller->send(launched->pid, KeeperSignal::Kill);
    CHECK(::kill(launched->pid, 0) == 0);

    ports.events->reap(launched->pid);
    CHECK(isReaped(launched->pid));
    ports.events->unwatchChannel(*launched->channel);
}

TEST_CASE("a daemon's last frames and its channel's close come before its end", "[keeper][posix]")
{
    auto const watch = GENERATE(ProcessWatch::ProcessHandle, ProcessWatch::SignalOnly);
    // One frame of two bytes and no descriptors, then the exit.
    auto ports = makePorts(watch, { "-c", R"(printf '\002\000AB' >&3)" });
    auto launched = ports.launcher->launch(program("sh"));
    REQUIRE(launched.has_value());
    auto const generation = DaemonGeneration { 4 };
    ports.events->watchChannel(*launched->channel, generation);
    ports.events->watchPid(launched->pid, Reaping::ByKeeper);

    auto const events = collectUntil(*ports.events, [&](auto const& seen) {
        return std::ranges::any_of(seen, [](Event const& event) { return event.kind == EventKind::Ended; });
    });
    auto const kinds = std::ranges::to<std::vector>(events | std::views::transform(&Event::kind));
    REQUIRE(kinds == std::vector { EventKind::Frame, EventKind::ChannelClosed, EventKind::Ended });
    REQUIRE(events[0].frame.has_value());
    CHECK(events[0].frame->body == std::vector<uint8_t> { 'A', 'B' });
    CHECK(events[0].generation == generation);
    CHECK(events[1].generation == generation);

    ports.events->reap(launched->pid);
    ports.events->unwatchChannel(*launched->channel);
}

TEST_CASE("a write to a dead peer does not kill the keeper", "[keeper][posix]")
{
    auto const sigpipe = SignalDisposition { SIGPIPE, SIG_DFL };
    auto ports = makePorts();
    auto pair = makeSocketPair();
    auto const channel = makePosixChannel(std::move(pair.keeper));
    ports.events->watchChannel(*channel, DaemonGeneration { 1 });
    pair.daemon.reset();

    channel->send(frameOf(Heartbeat {}));
    channel->send(frameOf(Heartbeat {}));

    auto const events = collectUntil(*ports.events, [](auto const& seen) { return !seen.empty(); });
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == EventKind::ChannelClosed);
    ports.events->unwatchChannel(*channel);
}

TEST_CASE("a sent frame's descriptors stay the caller's", "[keeper][posix]")
{
    auto ports = makePorts();
    auto pair = makeSocketPair();
    auto const sender = makePosixChannel(std::move(pair.daemon));
    auto const receiver = makePosixChannel(std::move(pair.keeper));
    ports.events->watchChannel(*receiver, DaemonGeneration { 1 });
    auto const pipe = makePipe();

    sender->send(frameOf(Structure {}, { pipe.reader.get() }));
    CHECK(::fcntl(pipe.reader.get(), F_GETFD) != -1);

    auto const events = collectUntil(*ports.events, [](auto const& seen) { return !seen.empty(); });
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].frame.has_value());
    REQUIRE(events[0].frame->fds.size() == 1);
    auto const received = UniqueFd { events[0].frame->fds[0] };
    CHECK(received.get() != pipe.reader.get());
    ports.events->unwatchChannel(*receiver);
}

TEST_CASE("received descriptors are close-on-exec", "[keeper][posix]")
{
    auto ports = makePorts();
    auto pair = makeSocketPair();
    auto const channel = makePosixChannel(std::move(pair.keeper));
    ports.events->watchChannel(*channel, DaemonGeneration { 1 });
    auto const pipe = makePipe();
    // Sent without close-on-exec, so that the flag is the receiver's doing.
    auto const inheritable = UniqueFd { ::dup(pipe.reader.get()) };
    sendRaw(
        pair.daemon.get(), encodeFrame(frameOf(Structure {}, { inheritable.get() })), { inheritable.get() });

    auto const events = collectUntil(*ports.events, [](auto const& seen) { return !seen.empty(); });
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].frame.has_value());
    REQUIRE(events[0].frame->fds.size() == 1);
    auto const received = UniqueFd { events[0].frame->fds[0] };
    CHECK((::fcntl(received.get(), F_GETFD) & FD_CLOEXEC) != 0);
    ports.events->unwatchChannel(*channel);
}

TEST_CASE("a plain frame glued to a descriptor-carrying one leaves the descriptors on the second",
          "[keeper][posix]")
{
    auto ports = makePorts();
    auto pair = makeSocketPair();
    auto const channel = makePosixChannel(std::move(pair.keeper));
    auto const pipe = makePipe();
    // Both are queued before the keeper reads: one read may return them together.
    sendRaw(pair.daemon.get(), encodeFrame(frameOf(Heartbeat {})), {});
    sendRaw(
        pair.daemon.get(), encodeFrame(frameOf(Structure {}, { pipe.reader.get() })), { pipe.reader.get() });
    ports.events->watchChannel(*channel, DaemonGeneration { 1 });

    auto const events = collectUntil(*ports.events, [](auto const& seen) { return seen.size() >= 2; });
    REQUIRE(events.size() == 2);
    REQUIRE(events[0].frame.has_value());
    REQUIRE(events[1].frame.has_value());
    CHECK(events[0].frame->fds.empty());
    REQUIRE(events[1].frame->fds.size() == 1);
    auto const received = UniqueFd { events[1].frame->fds[0] };
    CHECK(decode(events[1].frame->body).has_value());
    ports.events->unwatchChannel(*channel);
}

TEST_CASE("a truncated descriptor list ends the link and leaks nothing", "[keeper][posix]")
{
    auto ports = makePorts();
    auto pair = makeSocketPair();
    auto const channel = makePosixChannel(std::move(pair.keeper));
    ports.events->watchChannel(*channel, DaemonGeneration { 1 });
    auto const pipe = makePipe();
    auto const tooMany = std::vector<int>(MaxFdsPerFrame + 4, pipe.reader.get());
    auto const open = openDescriptorCount();
    sendRaw(pair.daemon.get(), encodeFrame(frameOf(Heartbeat {})), tooMany);

    auto const events = collectUntil(*ports.events, [](auto const& seen) { return !seen.empty(); });
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == EventKind::ChannelClosed);
    CHECK(openDescriptorCount() == open);
    ports.events->unwatchChannel(*channel);
}

TEST_CASE("a receive that finds its descriptor list cut short closes what came and says so",
          "[keeper][posix]")
{
    auto pair = makeSocketPair();
    auto const pipe = makePipe();
    auto const tooMany = std::vector<int>(MaxFdsPerFrame + 4, pipe.reader.get());
    sendRaw(pair.daemon.get(), encodeFrame(frameOf(Heartbeat {})), tooMany);
    auto const open = openDescriptorCount();

    auto buffer = std::array<uint8_t, 256> {};
    auto const received = receiveWithDescriptors(pair.keeper.get(), buffer);
    REQUIRE_FALSE(received.has_value());
    CHECK(received.error() == ReceiveError::Truncated);
    CHECK(openDescriptorCount() == open);
}

TEST_CASE("a list longer than any receive takes in is cut short, and what came is closed", "[keeper][posix]")
{
#ifdef __APPLE__
    SKIP("macOS installs the descriptors a control buffer has no room for, and leaks them");
#else
    auto pair = makeSocketPair();
    auto const pipe = makePipe();
    auto const farTooMany = std::vector<int>(MaxFdsPerFrame * 10, pipe.reader.get());
    sendRaw(pair.daemon.get(), encodeFrame(frameOf(Heartbeat {})), farTooMany);
    auto const open = openDescriptorCount();

    auto buffer = std::array<uint8_t, 256> {};
    auto const received = receiveWithDescriptors(pair.keeper.get(), buffer);
    REQUIRE_FALSE(received.has_value());
    CHECK(received.error() == ReceiveError::Truncated);
    CHECK(openDescriptorCount() == open);
#endif
}

TEST_CASE("a second label lock on the same path fails", "[keeper][posix]")
{
    // Removed with the scope, however the test ends.
    auto const temp = core::testing::ScopedTempDir { "keeper-lock" };
    auto const& directory = temp.path();
    auto const socket = directory / "contour.sock";
    CHECK(labelLockPath(socket) == directory / "contour.sock.keeper.lock");

    {
        auto first = lockLabel(socket);
        REQUIRE(first.has_value());
        auto const second = lockLabel(socket);
        REQUIRE(!second.has_value());
        CHECK(second.error() == LockError::Held);
        first->reset();
        CHECK(lockLabel(socket).has_value());
    }
    CHECK(lockLabel(directory / "missing" / "contour.sock").error() == LockError::Unopenable);
}

TEST_CASE("the daemon child dies with its keeper", "[keeper][posix]")
{
#ifndef __linux__
    SKIP("PR_SET_PDEATHSIG is Linux's; the macOS backstop watches the keeper with kqueue");
#else
    auto const report = makePipe();
    auto const sleep = program("sleep");
    auto const standIn = ::fork();
    REQUIRE(standIn >= 0);
    if (standIn == 0)
    {
        // The stand-in keeper: launch the daemon, tell its pid, and wait to be killed. Neither holds the
        // test's output open, should the daemon outlive its keeper after all.
        auto const null = ::open("/dev/null", O_RDWR);
        if (null < 0)
            ::_exit(1);
        for (auto const fd: { 0, 1, 2 })
            ::dup2(null, fd);
        auto ports =
            makePosixKeeperPorts({ .daemonArguments = { "60" }, .environment = {}, .processWatch = {} });
        auto launched = ports ? (*ports).launcher->launch(sleep) : std::unexpected { SpawnError::Unknown };
        auto const pid = launched ? launched->pid : 0;
        std::ignore = ::write(report.writer.get(), &pid, sizeof(pid));
        if (pid == 0)
            ::_exit(1);
        while (true)
            ::pause();
    }

    auto daemon = 0;
    auto const got = ::read(report.reader.get(), &daemon, sizeof(daemon));
    if (got != sizeof(daemon) || daemon <= 0)
    {
        ::kill(standIn, SIGKILL);
        ::waitpid(standIn, nullptr, 0);
        FAIL("the stand-in keeper launched no daemon");
    }
    auto const handle = UniqueFd { static_cast<int>(::syscall(SYS_pidfd_open, daemon, 0)) };
    REQUIRE(handle.isOpen());
    ::kill(standIn, SIGKILL);
    REQUIRE(::waitpid(standIn, nullptr, 0) == standIn);

    auto ended = ::pollfd { .fd = handle.get(), .events = POLLIN, .revents = 0 };
    CHECK(::poll(&ended, 1, 2'000) == 1);
#endif
}

TEST_CASE("a stop signal reaches the keeper as an event", "[keeper][posix]")
{
    auto ports = makePorts();
    REQUIRE(::kill(::getpid(), SIGTERM) == 0);
    auto const events = collectUntil(*ports.events, [](auto const& seen) { return !seen.empty(); });
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == EventKind::StopSignal);
}

TEST_CASE("a wait with nothing to do ends at its deadline", "[keeper][posix]")
{
    auto ports = makePorts();
    auto const start = ports.clock->now();
    auto const events = ports.events->wait(start + 50ms);
    CHECK(ports.clock->now() - start >= 50ms);
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == EventKind::Timer);
}

TEST_CASE("the budget counts the descriptors in use", "[keeper][posix]")
{
    auto ports = makePorts();
    auto const before = ports.budget->available();
    auto const pipes = std::array { makePipe(), makePipe(), makePipe() };
    CHECK(ports.budget->available() == before - (2 * pipes.size()));
}

TEST_CASE("an executable that cannot run is reported, not started", "[keeper][posix]")
{
    auto ports = makePorts();
    auto const request = Spawn { .request = 1,
                                 .argv = { "missing" },
                                 .executablePath = "/nonexistent/keeper-test-program",
                                 .childFds = {},
                                 .kept = {} };
    auto const child = ports.spawner->spawn(request, {});
    REQUIRE(!child.has_value());
    CHECK(child.error() == SpawnError::ExecutableUnusable);
    CHECK(!ports.launcher->launch("/nonexistent/keeper-test-daemon").has_value());
}

TEST_CASE("a send to a peer that is gone still reports what the peer wrote", "[keeper][posix]")
{
    auto ports = makePorts();
    auto pair = makeSocketPair();
    auto const channel = makePosixChannel(std::move(pair.keeper));
    ports.events->watchChannel(*channel, DaemonGeneration { 1 });
    auto const pipe = makePipe();
    // The daemon writes its last frame, with a descriptor, and exits; the keeper's heartbeat meets EPIPE.
    sendRaw(
        pair.daemon.get(), encodeFrame(frameOf(Structure {}, { pipe.reader.get() })), { pipe.reader.get() });
    pair.daemon.reset();
    channel->send(frameOf(Heartbeat {}));

    auto const events = collectUntil(*ports.events, [](auto const& seen) {
        return std::ranges::any_of(seen,
                                   [](Event const& event) { return event.kind == EventKind::ChannelClosed; });
    });
    auto const kinds = std::ranges::to<std::vector>(events | std::views::transform(&Event::kind));
    REQUIRE(kinds == std::vector { EventKind::Frame, EventKind::ChannelClosed });
    REQUIRE(events[0].frame.has_value());
    REQUIRE(events[0].frame->fds.size() == 1);
    auto const received = UniqueFd { events[0].frame->fds[0] };
    ports.events->unwatchChannel(*channel);
}

TEST_CASE("a shell that is a script runs", "[keeper][posix]")
{
    // Removed with the scope, however the test ends.
    auto const temp = core::testing::ScopedTempDir { "keeper-script" };
    auto const script = temp.path() / "script.sh";
    {
        auto stream = std::ofstream { script };
        stream << "#!/bin/sh\nexit 5\n";
    }
    std::filesystem::permissions(script, std::filesystem::perms::owner_all);
    auto ports = makePorts();
    auto const request = Spawn {
        .request = 1, .argv = { "script" }, .executablePath = script.string(), .childFds = {}, .kept = {}
    };
    auto const child = ports.spawner->spawn(request, {});
    REQUIRE(child.has_value());
    ports.events->watchPid(child->pid, Reaping::OnEnd);
    auto const status = waitForEnd(*ports.events, child->pid);
    REQUIRE(status.has_value());
    CHECK(*status == WireExitStatus { .kind = ExitKind::Normal, .value = 5 });
}

TEST_CASE("a queued frame's descriptors stay the caller's to close", "[keeper][posix]")
{
    auto ports = makePorts();
    auto pair = makeSocketPair();
    // More than the socket takes, so that the next frame is queued, with copies of its descriptors.
    auto constexpr Overfill = std::size_t { 4 } * 1024 * 1024;
    auto const bufferOf = [](int fd, int option) {
        auto size = 0;
        auto length = static_cast<socklen_t>(sizeof(size));
        REQUIRE(::getsockopt(fd, SOL_SOCKET, option, &size, &length) == 0);
        return static_cast<std::size_t>(size);
    };
    auto const socketHolds = bufferOf(pair.keeper.get(), SO_SNDBUF) + bufferOf(pair.daemon.get(), SO_RCVBUF);
    REQUIRE(socketHolds < Overfill);
    auto const sender = makePosixChannel(std::move(pair.keeper));
    auto const receiver = makePosixChannel(std::move(pair.daemon));
    ports.events->watchChannel(*sender, DaemonGeneration { 1 });
    ports.events->watchChannel(*receiver, DaemonGeneration { 2 });
    sender->send(Frame { .body = std::vector<uint8_t>(Overfill, 'x'), .fds = {} });
    auto pipe = makePipe();
    sender->send(frameOf(Structure {}, { pipe.reader.get() }));
    pipe.reader.reset();

    auto const carrying = [](Event const& event) {
        return event.frame && !event.frame->fds.empty();
    };
    auto const events =
        collectUntil(*ports.events, [&](auto const& seen) { return std::ranges::any_of(seen, carrying); });
    auto const found = std::ranges::find_if(events, carrying);
    REQUIRE(found != events.end());
    REQUIRE(found->frame->fds.size() == 1);
    auto const received = UniqueFd { found->frame->fds[0] };
    // Still the pipe the caller closed its end of.
    REQUIRE(::write(pipe.writer.get(), "k", 1) == 1);
    auto byte = char {};
    CHECK(::read(received.get(), &byte, 1) == 1);
    CHECK(byte == 'k');
    ports.events->unwatchChannel(*sender);
    ports.events->unwatchChannel(*receiver);
}

TEST_CASE("a daemon that does not read is dropped", "[keeper][posix]")
{
    auto ports = makePorts();
    auto pair = makeSocketPair();
    auto const channel = makePosixChannel(std::move(pair.keeper));
    ports.events->watchChannel(*channel, DaemonGeneration { 1 });
    auto const largest = Frame { .body = std::vector<uint8_t>(MaxFrameBodySize, 'x'), .fds = {} };
    for ([[maybe_unused]] auto const index: std::views::iota(0, 5))
        channel->send(largest);

    auto const events = ports.events->wait(ports.clock->now() + 5s);
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == EventKind::ChannelClosed);
    ports.events->unwatchChannel(*channel);
}
