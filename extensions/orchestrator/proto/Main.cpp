// SPDX-License-Identifier: Apache-2.0
//
// The prototype driver: the engine against the real world (git, gh, `contour api`), so the workflow can be
// tried before the extension host exists. It validates the workflow only: it neither enforces the roots nor
// asks for consent. Never installed; POSIX only.
#include <cerrno>
#include <cstddef>
#include <filesystem>
#include <format>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <poll.h>
#include <unistd.h>

#include <orchestrator/proto/Control.hpp>
#include <orchestrator/proto/Driver.hpp>
#include <orchestrator/proto/Environment.hpp>
#include <orchestrator/proto/Host.hpp>
#include <orchestrator/proto/Prompt.hpp>
#include <orchestrator/proto/Signals.hpp>
#include <orchestrator/proto/Spawn.hpp>
#include <orchestrator/proto/Store.hpp>

using namespace orchestrator;
using namespace orchestrator::proto;

namespace
{

/// The exit code after SIGINT or SIGTERM.
constexpr auto StoppedBySignal = 130;

/// @return @p path made absolute when it is a relative path (it holds a `/`): commands run in other
///         directories, where posix_spawnp() and `contour` resolve a relative path against those instead.
std::string absoluteIfPath(std::string_view path)
{
    if (!path.contains('/') || path.starts_with('/') || path.contains("://"))
        return std::string(path);
    return std::filesystem::absolute(path).lexically_normal().string();
}

/// The single-threaded loop: standard input, the signals and the running commands, in one poll(2).
/// @return The process's exit code.
int runLoop(Driver& driver, SignalPipe const& signals)
{
    auto prompt = Prompt { driver };
    auto fds = std::vector<pollfd> {};
    while (true)
    {
        fds.assign({ pollfd { .fd = STDIN_FILENO, .events = POLLIN, .revents = 0 },
                     pollfd { .fd = signals.fd(), .events = POLLIN, .revents = 0 } });
        driver.addPollFds(fds);
        if (::poll(fds.data(), fds.size(), driver.pollTimeoutMs(monotonicNowMs())) < 0 && errno != EINTR)
            return 1;
        if (fds[1].revents != 0 && signals.drain() == StopRequest::Received)
            return StoppedBySignal;
        if (fds[0].revents != 0)
            switch (prompt.readAvailable(STDIN_FILENO))
            {
                case Continue::Quit: return 0;
                case Continue::Failed: return 1;
                case Continue::Yes: break;
            }
        if (driver.service(std::span(fds).subspan(2)) != DriverHealth::Healthy)
            return 1;
    }
}

} // namespace

int main(int argc, char** argv)
{
    auto const args = std::span(argv, static_cast<std::size_t>(argc));
    if (args.size() < 3 || args.size() > 4)
    {
        std::println(
            stderr, "usage: orchestrator_proto <config.json> <daemon-socket> [contour]\n{}", Prompt::usage());
        return 2;
    }
    auto const signals = SignalPipe::install();
    if (!signals)
    {
        std::println(stderr, "error: the signal handlers could not be installed");
        return 1;
    }
    auto environment = inheritedEnvironment();
    auto configJson = readWholeFile(args[1]);
    if (!configJson)
    {
        std::println(stderr, "error: cannot read {}", args[1]);
        return 1;
    }
    auto host = resolveHost(*configJson, environment);
    if (!host)
    {
        std::println(stderr, "error: {}", host.error());
        return 1;
    }
    auto const stateHome =
        lookup(environment, "XDG_STATE_HOME").value_or(std::format("{}/.local/state", host->home));
    auto control = ControlApi { absoluteIfPath(args.size() == 4 ? args[3] : "contour"),
                                absoluteIfPath(args[2]),
                                environment,
                                host->home };
    auto driver = Driver::start(
        DriverSetup { .host = *host,
                      .configJson = std::move(*configJson),
                      .store = FileStore { std::format("{}/contour/orchestrator-proto", stateHome) },
                      .control = std::move(control),
                      .environment = std::move(environment) });
    if (!driver)
        return 1;
    std::println("{}", Prompt::usage());
    // On every way out the driver's destructor stops the commands still running.
    return runLoop(*driver, *signals);
}
