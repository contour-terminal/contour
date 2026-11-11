// SPDX-License-Identifier: Apache-2.0
#include <vthost/keeper/KeeperProcess.hpp>

#ifdef __linux__
    #include <sys/prctl.h>
#endif

#include <core/log/LogStore.hpp>

#include <charconv>
#include <csignal>
#include <fstream>
#include <memory>
#include <string>
#include <utility>

#include <vthost/keeper/Keeper.hpp>
#include <vthost/keeper/Logging.hpp>
#include <vthost/keeper/PosixPorts.hpp>

namespace vthost::keeper
{

std::string_view describe(KeeperStartError error) noexcept
{
    switch (error)
    {
        case KeeperStartError::MultiThreaded:
            return "the keeper process runs more than one thread, which its signal handling cannot allow";
        case KeeperStartError::LabelHeld: return "another keeper already keeps a daemon on this socket";
        case KeeperStartError::LabelUnlockable: return "the socket's keeper lock file cannot be opened";
        case KeeperStartError::PortsUnavailable: return "the keeper's signal watch cannot be set up";
    }
    return "the keeper cannot start";
}

namespace
{
    [[nodiscard]] std::string_view describe(KeeperOutcome outcome) noexcept
    {
        switch (outcome)
        {
            case KeeperOutcome::Clean: return "clean";
            case KeeperOutcome::DaemonFailedToStart: return "the daemon failed to start";
            case KeeperOutcome::GaveUp: return "gave up";
        }
        return "ended";
    }
} // namespace

std::optional<std::size_t> runningThreadCount()
{
#ifdef __linux__
    auto status = std::ifstream { "/proc/self/status" };
    constexpr auto Key = std::string_view { "Threads:" };
    auto line = std::string {};
    while (std::getline(status, line))
    {
        if (!line.starts_with(Key))
            continue;
        auto const value = std::string_view { line }.substr(Key.size());
        auto const first = value.find_first_not_of(" \t");
        if (first == std::string_view::npos)
            return std::nullopt;
        auto count = std::size_t { 0 };
        auto const digits = value.substr(first);
        auto const [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), count);
        if (error != std::errc {})
            return std::nullopt;
        return count;
    }
    return std::nullopt;
#else
    // macOS has no cheap count of a process's own threads; the keeper's callers start none (the keeper
    // verb runs before anything that would), which only the Linux check proves.
    return std::nullopt;
#endif
}

std::expected<int, KeeperStartError> runKeeperProcess(KeeperProcessConfig config)
{
    // A log line written to a pipe whose reader is gone (`--log-file -` into a pager that quit) must not end
    // the keeper, and every shell with it: the write fails instead. Every child the keeper forks resets its
    // dispositions before its exec (runChild()), so none inherits this.
    ::signal(SIGPIPE, SIG_IGN);

    if (auto const threads = runningThreadCount(); threads && *threads != 1)
    {
        errorLog()(
            "the keeper runs {} threads, not one: {}", *threads, describe(KeeperStartError::MultiThreaded));
        return std::unexpected { KeeperStartError::MultiThreaded };
    }

#ifdef __linux__
    // Not dumpable: /proc/<keeper>/fd and /proc/<keeper>/mem become root's, so that no other process of the
    // user's -- every hosted shell among them -- can open what the keeper holds through them: each PTY
    // master, and the structure's memory object, which carries the private key of the daemon's self-signed
    // TLS identity. The daemon and the shells it forks are dumpable again by their exec.
    ::prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
#endif

    // A second keeper on the label fails here, before any daemon is started or any socket touched.
    auto lock = lockLabel(config.socketPath);
    if (!lock)
        return std::unexpected { lock.error() == LockError::Held ? KeeperStartError::LabelHeld
                                                                 : KeeperStartError::LabelUnlockable };

    auto ports =
        makePosixKeeperPorts(KeeperPortsConfig { .daemonArguments = std::move(config.daemonArguments),
                                                 .environment = std::move(config.environment),
                                                 .processWatch = ProcessWatch::ProcessHandle,
                                                 .images = std::move(config.images) });
    if (!ports)
    {
        errorLog()("the keeper cannot watch its signals: {}", ports.error().message());
        return std::unexpected { KeeperStartError::PortsUnavailable };
    }

    keeperLog()("keeping the daemon {} on {}", config.daemonImage.string(), config.socketPath.string());
    auto keeper = Keeper { ports->ports(),
                           *std::move(lock),
                           config.policy,
                           std::move(config.daemonImage),
                           std::move(config.launchOptions) };
    auto const outcome = keeper.run();
    keeperLog()("the keeper ends ({}), exit status {}", describe(outcome), keeper.exitStatus());
    return keeper.exitStatus();
}

} // namespace vthost::keeper
