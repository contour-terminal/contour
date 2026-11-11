// SPDX-License-Identifier: Apache-2.0
#include <vthost/testing/KeeperFixture.hpp>

#include <vtpty/ExecutablePath.hpp>

#include <core/codec/Stream.hpp>
#include <core/platform/Clock.hpp>

#include <sys/wait.h>

#include <array>
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

#include <fcntl.h>
#include <spawn.h>
#include <unistd.h>

#include <vthost/Daemon.hpp>
#include <vthost/Transport.hpp>
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
    constexpr auto FailsWord = std::string_view { "fails" };

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
        std::ofstream { launch.launchLog, std::ios::app } << ::getpid() << '\n';
        if (launch.start == HarnessStart::Fails)
            return HarnessStartFailureStatus;

        auto config = DaemonConfig {};
        config.socketPath = launch.socketPath;
        config.releaseVersion = "keeper-harness";
        return runDaemon(config, std::nullopt, *std::move(attachment));
    }

    /// The keeper: keeps `vthost_test --keeper-serve` on @p socket.
    [[nodiscard]] int keepHarnessDaemon(std::string_view socket,
                                        std::string_view launchLog,
                                        std::string_view start)
    {
        auto const launch =
            HarnessLaunch { .socketPath = std::string { socket },
                            .launchLog = std::string { launchLog },
                            .start = start == FailsWord ? HarnessStart::Fails : HarnessStart::Serves };
        auto writer = core::codec::Writer {};
        core::codec::encodeFields(writer, launch);
        auto const ran = keeper::runKeeperProcess(keeper::KeeperProcessConfig {
            .daemonImage = ownExecutable(),
            .daemonArguments = { std::string { ServeMode } },
            .environment = processEnvironment(),
            .socketPath = std::filesystem::path { std::string { socket } },
            .launchOptions = std::move(writer).take(),
            .policy = {},
        });
        if (!ran)
        {
            std::cerr << std::format("keeper: {}\n", keeper::describe(ran.error()));
            return HarnessKeeperStartFailureStatus;
        }
        return *ran;
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
}

std::optional<int> runKeeperHarnessIfRequested(int argc, char const* argv[])
{
    auto const arguments = std::span { argv, static_cast<std::size_t>(argc) };
    if (arguments.size() >= 2 && arguments[1] == ServeMode)
        return serveBehindKeeper();
    constexpr auto KeeperArgumentCount = std::size_t { 5 };
    if (arguments.size() >= KeeperArgumentCount && arguments[1] == KeeperMode)
        return keepHarnessDaemon(arguments[2], arguments[3], arguments[4]);
    return std::nullopt;
}

KeeperFixture::KeeperFixture(HarnessStart start):
    _directory { makeOwnerOnlyDirectory() },
    _socket { _directory / "control" },
    _launchLog { _directory / "launches" },
    _output { _directory / "output" },
    _keeper { spawnKeeper(start) }
{
    if (start == HarnessStart::Fails)
        return;
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

void KeeperFixture::end(Ending ending)
{
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

int KeeperFixture::spawnKeeper(HarnessStart start) const
{
    auto actions = ::posix_spawn_file_actions_t {};
    ::posix_spawn_file_actions_init(&actions);
    ::posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    ::posix_spawn_file_actions_addopen(
        &actions, STDOUT_FILENO, _output.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    ::posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
    auto const image = ownExecutable().string();
    auto const socket = _socket.string();
    auto const launchLog = _launchLog.string();
    auto const startWord =
        std::string { start == HarnessStart::Fails ? FailsWord : std::string_view { "serves" } };
    auto arguments =
        std::array<std::string, 5> { image, std::string { KeeperMode }, socket, launchLog, startWord };
    auto argv = std::array<char*, 6> {};
    for (auto&& [slot, argument]: std::views::zip(argv, arguments))
        slot = argument.data();
    auto pid = ::pid_t {};
    auto const spawned = ::posix_spawn(&pid, image.c_str(), &actions, nullptr, argv.data(), environ);
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
    auto const second = spawnKeeper(HarnessStart::Serves);
    auto const waited = awaitProcess(second, bound);
    if (!waited)
    {
        ::kill(second, SIGKILL);
        std::ignore = awaitProcess(second, std::chrono::seconds { 5 });
    }
    return exitStatusOf(waited);
}

std::string KeeperFixture::log() const
{
    return readFile(_output);
}

} // namespace vthost::testing
