// SPDX-License-Identifier: Apache-2.0
#include <vthost/testing/KeeperFixture.hpp>

#include <vtpty/ExecutablePath.hpp>

#include <core/codec/Stream.hpp>
#include <core/log/LogSink.hpp>
#include <core/platform/Clock.hpp>

#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <ranges>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#ifdef __APPLE__
    #include <libproc.h>
#endif

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <unistd.h>

#include <vthost/Daemon.hpp>
#include <vthost/Transport.hpp>
#include <vthost/keeper/DescriptorSocket.hpp>
#include <vthost/keeper/KeeperProcess.hpp>
#include <vthost/keeper/PosixPorts.hpp>
#include <vthost/keeperlink/KeeperAttachment.hpp>

// POSIX requires no header to declare `environ`; src/vtpty/Process_unix.cpp explains the per-platform story.
#ifdef __APPLE__
    #include <crt_externs.h>
    #define environ (*_NSGetEnviron())
#elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
extern "C" char** environ;
#endif

namespace vthost::testing
{

namespace
{
    constexpr auto KeeperMode = std::string_view { "--keeper" };
    constexpr auto ServeMode = std::string_view { "--keeper-serve" };

    /// The status the harness daemon exits with when the handshake with its keeper failed.
    constexpr auto AttachFailureStatus = 2;

    [[nodiscard]] std::filesystem::path ownExecutable()
    {
        auto path = vtpty::currentExecutablePath();
        if (!path)
            throw std::runtime_error { std::format("cannot locate the test binary: {}",
                                                   path.error().message()) };
        return *std::move(path);
    }

    [[nodiscard]] std::vector<std::string> processEnvironment()
    {
        auto environment = std::vector<std::string> {};
        auto const set = [](char* const* entry) {
            return *entry != nullptr;
        };
        for (auto const* const* entry: std::views::iota(environ) | std::views::take_while(set))
            environment.emplace_back(*entry);
        return environment;
    }

    /// @return A connected or listening AF_UNIX stream socket on @p path; none where it could not be made.
    [[nodiscard]] keeper::UniqueFd unixSocket(std::string const& path,
                                              int (*act)(int, sockaddr const*, socklen_t))
    {
        // Close-on-exec set after the fact: macOS has no SOCK_CLOEXEC.
        auto socket = keeper::UniqueFd { ::socket(AF_UNIX, SOCK_STREAM, 0) };
        auto address = sockaddr_un {};
        address.sun_family = AF_UNIX;
        if (!socket.isOpen() || ::fcntl(socket.get(), F_SETFD, FD_CLOEXEC) != 0
            || path.size() >= sizeof(address.sun_path))
            return {};
        std::ranges::copy(path, std::begin(address.sun_path));
        if (act(socket.get(), reinterpret_cast<sockaddr const*>(&address), sizeof(address)) != 0)
            return {};
        return socket;
    }

    /// The relaying daemon: hands its channel to the test listening on @p path, and lives until the test
    /// closes the connection.
    [[nodiscard]] int relayChannel(std::string const& path, keeperlink::KeeperAttachment attachment)
    {
        auto const connection = unixSocket(path, ::connect);
        if (!connection.isOpen())
        {
            std::cerr << std::format("keeper-serve: cannot reach the test at {}\n", path);
            return AttachFailureStatus;
        }
        auto const marker = std::array<uint8_t, 1> { 1 };
        auto const runs = std::array { std::span<uint8_t const> { marker } };
        auto const channel = std::array { attachment.channel.handle() };
        if (!keeper::sendWithDescriptors(connection.get(), runs, channel))
            return AttachFailureStatus;
        // The test holds the channel now; this process holds none of it.
        {
            auto const handedOver = std::move(attachment);
        }
        auto byte = char {};
        while (true)
        {
            auto const got = ::read(connection.get(), &byte, 1);
            if (got == 0 || (got < 0 && errno != EINTR))
                return 0;
        }
    }

    /// @return How many daemon processes @p launchLog names.
    [[nodiscard]] std::size_t launchCount(std::string const& launchLog)
    {
        auto file = std::ifstream { launchLog };
        return static_cast<std::size_t>(std::ranges::distance(std::views::istream<int>(file)));
    }

    /// @return The daemon @p launch describes.
    [[nodiscard]] DaemonConfig daemonConfigOf(HarnessLaunch const& launch)
    {
        auto config = DaemonConfig {};
        config.socketPath = launch.socketPath;
        config.releaseVersion = "keeper-harness";
        // A run of its own, as `contour daemon` draws it; a daemon restarted after a crash serves as the run
        // its structure names.
        config.instance = (uint64_t { std::random_device {}() } << 32U) | std::random_device {}() | 1U;
        for (auto const& command: launch.panes)
        {
            auto tab = vtworkspace::LayoutTab {};
            tab.root.command = "/bin/sh";
            tab.root.arguments = { "-c", command };
            config.startupLayout.tabs.push_back(std::move(tab));
        }
        if (!launch.snapshotPath.empty())
        {
            config.persistence.snapshotPath = launch.snapshotPath;
            config.persistence.interval =
                launch.snapshotIntervalMs != 0
                    ? std::chrono::duration_cast<decltype(config.persistence.interval)>(
                          std::chrono::milliseconds { launch.snapshotIntervalMs })
                    : std::chrono::seconds { 1 };
        }
        if (launch.lifecycle == HarnessLifecycle::ExitWhenIdle)
        {
            config.lifecycle = DaemonLifecycle::ExitWhenIdle;
            config.idleExitGrace = std::chrono::seconds { 1 };
        }
        if (launch.start == HarnessStart::IgnoresStop)
        {
            // Not even its own watch hears a stop: what a forwarded one meets is SIG_IGN.
            config.shutdownSignals = std::make_shared<NoShutdownSignals>();
            for (auto const signum: { SIGTERM, SIGINT, SIGHUP })
                std::signal(signum, SIG_IGN);
        }
        if (launch.tcp == HarnessTcp::Loopback)
        {
            config.nativeTcp = NativeTcpListenerConfig {};
            config.nativeTcp->host = "127.0.0.1";
            config.nativeTcp->port = 0;
            config.nativeTcp->token = "harness";
        }
        return config;
    }

    /// The daemon behind the keeper: the handshake on its channel, then runDaemon().
    [[nodiscard]] int serveBehindKeeper()
    {
        auto const clock = core::platform::SteadyClock {};
        auto attachment = keeperlink::attachToKeeper(
            keeper::UniqueFd { keeper::DaemonChannelDescriptor }, clock, keeperlink::AttachBound);
        if (!attachment)
        {
            std::cerr << std::format("keeper-serve: {}\n", keeperlink::describe(attachment.error()));
            return AttachFailureStatus;
        }
        auto launch = HarnessLaunch {};
        if (!core::codec::decodeFields(attachment->options, launch))
        {
            std::cerr << "keeper-serve: the launch record is unreadable\n";
            return AttachFailureStatus;
        }
        auto const first = launchCount(launch.launchLog) == 0;
        std::ofstream { launch.launchLog, std::ios::app } << ::getpid() << '\n';
        if (launch.start == HarnessStart::Fails)
            return HarnessStartFailureStatus;
        if (launch.start == HarnessStart::ServesThenCrashes && !first)
            ::kill(::getpid(), SIGKILL); // a restart that crashes before it serves
        if (launch.start == HarnessStart::Relays || (launch.start == HarnessStart::RelaysThenServes && first))
            return relayChannel(launch.relayPath, *std::move(attachment));
        // What the daemon says goes to the keeper's output, for a test to read (KeeperFixture::log()).
        auto const logging = core::log::ScopedOutput::create(
            { .filter = "vthost.daemon,vthost.session", .file = std::nullopt, .showProcessId = true });
        return runDaemon(daemonConfigOf(launch), std::nullopt, *std::move(attachment));
    }

    [[nodiscard]] std::string readFile(std::filesystem::path const& path);

    /// The keeper: keeps `vthost_test --keeper-serve` as the HarnessLaunch in @p record says.
    [[nodiscard]] int keepHarnessDaemon(std::string_view record)
    {
        auto const content = readFile(std::filesystem::path { std::string { record } });
        auto const bytes = std::span { reinterpret_cast<uint8_t const*>(content.data()), content.size() };
        auto launch = HarnessLaunch {};
        if (!core::codec::decodeFields(bytes, launch))
        {
            std::cerr << "keeper: the launch record is unreadable\n";
            return HarnessKeeperStartFailureStatus;
        }
        auto const socket = launch.socketPath;
        // What the keeper says goes to the fixture's output too, beside its daemons' (KeeperFixture::log()).
        auto const logging = core::log::ScopedOutput::create(
            { .filter = "vthost.keeper", .file = std::nullopt, .showProcessId = true });
        auto policy = keeper::RestartPolicyConfig {};
        if (launch.timings.backoffMs != 0)
        {
            policy.backoffStart = std::chrono::milliseconds { launch.timings.backoffMs };
            policy.backoffCap = policy.backoffStart;
        }
        if (launch.timings.graceMs != 0)
            policy.grace = std::chrono::milliseconds { launch.timings.graceMs };
        if (launch.timings.daemonExitBoundMs != 0)
            policy.daemonExitBound = std::chrono::milliseconds { launch.timings.daemonExitBoundMs };
        auto writer = core::codec::Writer {};
        core::codec::encodeFields(writer, launch);
        auto const ran = keeper::runKeeperProcess(keeper::KeeperProcessConfig {
            .daemonImage = ownExecutable(),
            .daemonArguments = { std::string { ServeMode } },
            .environment =
                [] {
                    auto environment = processEnvironment();
                    environment.push_back(std::format("{}=1", KeeperOnlyVariable));
                    return environment;
                }(),
            .socketPath = std::filesystem::path { std::string { socket } },
            .launchOptions = std::move(writer).take(),
            .policy = policy,
            .images = nullptr,
        });
        if (!ran)
        {
            std::cerr << std::format("keeper: {}\n", keeper::describe(ran.error()));
            return HarnessKeeperStartFailureStatus;
        }
        return *ran;
    }

    /// @return @p panes with `@DIR@` replaced by @p directory.
    [[nodiscard]] std::vector<std::string> panesIn(std::vector<std::string> panes,
                                                   std::filesystem::path const& directory)
    {
        constexpr auto Placeholder = std::string_view { "@DIR@" };
        for (auto& command: panes)
        {
            auto at = command.find(Placeholder);
            while (at != std::string::npos)
            {
                command.replace(at, Placeholder.size(), directory.string());
                at = command.find(Placeholder, at);
            }
        }
        return panes;
    }

    /// Writes @p launch, encoded, to @p path.
    /// @return @p path.
    [[nodiscard]] std::filesystem::path writeRecord(std::filesystem::path path, HarnessLaunch const& launch)
    {
        auto writer = core::codec::Writer {};
        core::codec::encodeFields(writer, launch);
        auto const bytes = std::move(writer).take();
        std::ofstream { path, std::ios::binary }.write(reinterpret_cast<char const*>(bytes.data()),
                                                       static_cast<std::streamsize>(bytes.size()));
        return path;
    }

    /// @return A new directory of the test's own, owner-only, as a daemon's socket directory must be.
    [[nodiscard]] std::filesystem::path makeOwnerOnlyDirectory()
    {
        auto directory = std::filesystem::temp_directory_path()
                         / std::format("contour-keeper-{}", std::random_device {}());
        std::filesystem::create_directories(directory);
        std::filesystem::permissions(directory, std::filesystem::perms::owner_all);
        return directory;
    }

    [[nodiscard]] std::string readFile(std::filesystem::path const& path)
    {
        auto file = std::ifstream { path };
        return { std::istreambuf_iterator<char> { file }, std::istreambuf_iterator<char> {} };
    }

    /// Waits for @p pid to end, for up to @p bound.
    /// @return Its wait status; nullopt when it still runs.
    [[nodiscard]] std::optional<int> awaitProcess(int pid, std::chrono::milliseconds bound)
    {
        auto const deadline = std::chrono::steady_clock::now() + bound;
        while (true)
        {
            auto status = 0;
            auto const waited = ::waitpid(pid, &status, WNOHANG);
            if (waited == pid)
                return status;
            if (waited < 0)
                return std::nullopt;
            if (std::chrono::steady_clock::now() >= deadline)
                return std::nullopt;
            std::this_thread::sleep_for(std::chrono::milliseconds { 20 });
        }
    }

    [[nodiscard]] std::optional<int> exitStatusOf(std::optional<int> waitStatus)
    {
        if (!waitStatus || !WIFEXITED(*waitStatus))
            return std::nullopt;
        return WEXITSTATUS(*waitStatus);
    }
} // namespace

int parentProcessOf(int pid)
{
#ifdef __APPLE__
    auto info = proc_bsdinfo {};
    if (::proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, sizeof(info)) != static_cast<int>(sizeof(info)))
        return 0;
    return static_cast<int>(info.pbi_ppid);
#else
    auto file = std::ifstream { std::format("/proc/{}/stat", pid) };
    auto const stat =
        std::string { std::istreambuf_iterator<char> { file }, std::istreambuf_iterator<char> {} };
    // pid (comm) state ppid ...: the command may hold spaces and parentheses, so read after the last ')'.
    auto const closing = stat.rfind(')');
    if (closing == std::string::npos)
        return 0;
    auto rest = std::istringstream { stat.substr(closing + 1) };
    auto state = std::string {};
    auto parent = 0;
    rest >> state >> parent;
    return parent;
#endif
}

std::optional<int> runKeeperHarnessIfRequested(int argc, char const* argv[])
{
    auto const arguments = std::span { argv, static_cast<std::size_t>(argc) };
    if (arguments.size() >= 2 && arguments[1] == ServeMode)
        return serveBehindKeeper();
    constexpr auto KeeperArgumentCount = std::size_t { 3 };
    if (arguments.size() >= KeeperArgumentCount && arguments[1] == KeeperMode)
        return keepHarnessDaemon(arguments[2]);
    return std::nullopt;
}

KeeperFixture::KeeperFixture(HarnessStart start):
    KeeperFixture { HarnessSetup { .start = start,
                                   .panes = {},
                                   .snapshot = HarnessSnapshot::None,
                                   .tcp = HarnessTcp::Off,
                                   .softDescriptorLimit = std::nullopt } }
{
}

KeeperFixture::KeeperFixture(HarnessSetup const& setup):
    _directory { makeOwnerOnlyDirectory() },
    _socket { _directory / "control" },
    _launchLog { _directory / "launches" },
    _output { _directory / "output" },
    _record { writeRecord(_directory / "launch-record",
                          HarnessLaunch { .socketPath = _socket.string(),
                                          .launchLog = _launchLog.string(),
                                          .start = setup.start,
                                          .panes = panesIn(setup.panes, _directory),
                                          .snapshotPath = setup.snapshot == HarnessSnapshot::Kept
                                                              ? (_directory / "workspace.snapshot").string()
                                                              : std::string {},
                                          .tcp = setup.tcp,
                                          .relayPath = (_directory / "relay").string(),
                                          .timings = setup.timings,
                                          .snapshotIntervalMs = setup.snapshotIntervalMs,
                                          .lifecycle = setup.lifecycle }) },
    _softDescriptorLimit { setup.softDescriptorLimit },
    _relayListener { setup.start == HarnessStart::Relays || setup.start == HarnessStart::RelaysThenServes
                         ? unixSocket((_directory / "relay").string(), ::bind)
                         : keeper::UniqueFd {} },
    _keeper { spawnKeeper() }
{
    auto const start = setup.start;
    if (start == HarnessStart::Fails)
        return;
    if (start == HarnessStart::Relays || start == HarnessStart::RelaysThenServes)
    {
        acceptRelay();
        return;
    }
    auto const deadline = std::chrono::steady_clock::now() + StartBound;
    while (daemonPid() == 0)
    {
        if (std::chrono::steady_clock::now() >= deadline)
        {
            // No destructor runs for a constructor that throws: what it started ends here.
            auto const output = log();
            end(Ending::AtOnce);
            throw std::runtime_error { std::format("the keeper's daemon did not serve:\n{}", output) };
        }
        std::this_thread::sleep_for(std::chrono::milliseconds { 20 });
    }
}

KeeperFixture::~KeeperFixture()
{
    end(Ending::Orderly);
}

void KeeperFixture::acceptRelay()
{
    auto const fail = [this](std::string_view what) {
        auto const output = log();
        end(Ending::AtOnce);
        throw std::runtime_error { std::format("the keeper's relaying daemon {}:\n{}", what, output) };
    };
    // Waits for @p fd to turn readable within the start bound.
    auto const readable = [](int fd) {
        auto poller = ::pollfd { .fd = fd, .events = POLLIN, .revents = 0 };
        auto const bound = std::chrono::duration_cast<std::chrono::milliseconds>(StartBound).count();
        return ::poll(&poller, 1, static_cast<int>(bound)) == 1;
    };
    if (!_relayListener.isOpen() || ::listen(_relayListener.get(), 1) != 0)
        fail("has nowhere to connect to");
    if (!readable(_relayListener.get()))
        fail("did not connect");
    _relay = keeper::UniqueFd { ::accept(_relayListener.get(), nullptr, nullptr) };
    if (!_relay.isOpen() || !readable(_relay.get()))
        fail("did not hand over its channel");
    auto buffer = std::array<uint8_t, 1> {};
    auto received = keeper::receiveWithDescriptors(_relay.get(), buffer);
    if (!received || received->fds.size() != 1)
        fail("handed over no channel");
    _channel = keeper::UniqueFd { received->fds.front() };
}

void KeeperFixture::end(Ending ending)
{
    if (_secondKeeper != 0)
    {
        ::kill(_secondKeeper, SIGTERM);
        if (!awaitProcess(_secondKeeper, std::chrono::seconds { 15 }))
        {
            ::kill(_secondKeeper, SIGKILL);
            std::ignore = awaitProcess(_secondKeeper, std::chrono::seconds { 5 });
        }
        _secondKeeper = 0;
    }
    // A relaying daemon ends once the test lets it go; with nowhere to connect to, a restarted one ends too.
    _relayListener.reset();
    _relay.reset();
    _channel.reset();
    if (_keeperState == KeeperState::Running)
    {
        if (ending == Ending::Orderly)
        {
            // The keeper forwards the stop to its daemon, and ends once that did.
            ::kill(_keeper, SIGTERM);
            if (awaitProcess(_keeper, std::chrono::seconds { 15 }))
                _keeperState = KeeperState::Reaped;
        }
        if (_keeperState == KeeperState::Running)
        {
            // By recorded pid, and only while the keeper still runs: a daemon's pid is the keeper's to reap,
            // so only then is it known still to be that daemon.
            for (auto const daemon: launches())
                if (parentProcessOf(daemon) == _keeper)
                    ::kill(daemon, SIGKILL);
            ::kill(_keeper, SIGKILL);
            std::ignore = awaitProcess(_keeper, std::chrono::seconds { 5 });
            _keeperState = KeeperState::Reaped;
        }
    }
    auto ignored = std::error_code {};
    std::filesystem::remove_all(_directory, ignored);
}

int KeeperFixture::spawnKeeper() const
{
    auto actions = ::posix_spawn_file_actions_t {};
    ::posix_spawn_file_actions_init(&actions);
    ::posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    ::posix_spawn_file_actions_addopen(
        &actions, STDOUT_FILENO, _output.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    ::posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
    auto const image = ownExecutable().string();
    auto arguments = std::array<std::string, 3> { image, std::string { KeeperMode }, _record.string() };
    auto argv = std::array<char*, 4> {};
    for (auto&& [slot, argument]: std::views::zip(argv, arguments))
        slot = argument.data();
    auto pid = ::pid_t {};
    // The keeper inherits the soft limit asked for; this process has its own back at once.
    auto original = rlimit {};
    ::getrlimit(RLIMIT_NOFILE, &original);
    if (_softDescriptorLimit)
    {
        auto lowered = original;
        lowered.rlim_cur = std::min<rlim_t>(static_cast<rlim_t>(*_softDescriptorLimit), original.rlim_max);
        ::setrlimit(RLIMIT_NOFILE, &lowered);
    }
    auto const spawned = ::posix_spawn(&pid, image.c_str(), &actions, nullptr, argv.data(), environ);
    ::setrlimit(RLIMIT_NOFILE, &original);
    ::posix_spawn_file_actions_destroy(&actions);
    if (spawned != 0)
        throw std::runtime_error { std::format("cannot start the keeper: {}",
                                               std::system_category().message(spawned)) };
    return pid;
}

int KeeperFixture::daemonPid() const
{
    auto const status = client()->status();
    return status ? static_cast<int>(status->processId) : 0;
}

void KeeperFixture::killDaemon(int signum) const
{
    if (auto const pid = daemonPid(); pid > 0)
        ::kill(pid, signum);
}

std::unique_ptr<client::DaemonControlPort> KeeperFixture::client() const
{
    return client::makeDaemonControl(std::make_shared<UnixTransportFactory>(_socket, nullptr),
                                     std::chrono::seconds { 2 });
}

std::vector<int> KeeperFixture::launches() const
{
    auto file = std::ifstream { _launchLog };
    return std::ranges::to<std::vector>(std::views::istream<int>(file));
}

std::optional<int> KeeperFixture::awaitKeeperExit(std::chrono::milliseconds bound)
{
    auto const waited = awaitProcess(_keeper, bound);
    if (waited)
        _keeperState = KeeperState::Reaped;
    return exitStatusOf(waited);
}

std::optional<int> KeeperFixture::runSecondKeeper(std::chrono::milliseconds bound) const
{
    auto const second = spawnKeeper();
    auto const waited = awaitProcess(second, bound);
    if (!waited)
    {
        ::kill(second, SIGKILL);
        std::ignore = awaitProcess(second, std::chrono::seconds { 5 });
    }
    return exitStatusOf(waited);
}

int KeeperFixture::startSecondKeeper()
{
    _secondKeeper = spawnKeeper();
    return _secondKeeper;
}

bool KeeperFixture::secondKeeperRuns() const
{
    auto info = siginfo_t {};
    // WNOWAIT: looked at, left for end() to reap.
    return _secondKeeper != 0
           && ::waitid(P_PID, static_cast<id_t>(_secondKeeper), &info, WEXITED | WNOHANG | WNOWAIT) == 0
           && info.si_pid == 0;
}

std::string KeeperFixture::log() const
{
    return readFile(_output);
}

int KeeperFixture::awaitNewDaemon(int previous, std::chrono::milliseconds bound) const
{
    auto const deadline = std::chrono::steady_clock::now() + bound;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (auto const pid = daemonPid(); pid > 0 && pid != previous)
            return pid;
        std::this_thread::sleep_for(std::chrono::milliseconds { 20 });
    }
    return 0;
}

uint16_t KeeperFixture::nativeTcpPort() const
{
    // The last daemon's word for it: "native TCP listener on 127.0.0.1:<port>".
    constexpr auto Marker = std::string_view { "native TCP listener on 127.0.0.1:" };
    auto const output = log();
    auto const at = output.rfind(Marker);
    if (at == std::string::npos)
        return 0;
    auto port = 0U;
    auto const* const first = output.data() + at + Marker.size();
    std::ignore = std::from_chars(first, output.data() + output.size(), port);
    return static_cast<uint16_t>(port);
}

} // namespace vthost::testing
