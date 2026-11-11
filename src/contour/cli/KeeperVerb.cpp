// SPDX-License-Identifier: Apache-2.0
#include <contour/cli/KeeperVerb.hpp>

#include <core/platform/Clock.hpp>

#include <cstdlib>
#include <format>
#include <ostream>
#include <ranges>
#include <utility>

#ifndef _WIN32
    #include <unistd.h>

    #include <vthost/keeper/KeeperProcess.hpp>
    #include <vthost/keeper/PosixPorts.hpp>

    // POSIX requires no header to declare `environ`; src/vtpty/Process_unix.cpp explains the per-platform
    // story.
    #ifdef __APPLE__
        #include <crt_externs.h>
        #define environ (*_NSGetEnviron())
    #elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
extern "C" char** environ;
    #endif
#endif

namespace contour::cli
{

#ifndef _WIN32
std::vector<std::string> keeperServeArguments()
{
    return { "daemon",
             "keeper-serve",
             std::format("--keeper-channel={}", vthost::keeper::DaemonChannelDescriptor) };
}

std::vector<std::string> processEnvironmentEntries()
{
    auto entries = std::vector<std::string> {};
    auto const set = [](char* const* entry) {
        return *entry != nullptr;
    };
    for (auto const* const* entry: std::views::iota(environ) | std::views::take_while(set))
        entries.emplace_back(*entry);
    return entries;
}

int runKeeperVerb(KeeperVerbInputs inputs, std::ostream& errors)
{
    auto const socketPath = inputs.launch.socketPath();
    auto const kept = vthost::keeper::runKeeperProcess(vthost::keeper::KeeperProcessConfig {
        .daemonImage = std::move(inputs.daemonImage),
        .daemonArguments = keeperServeArguments(),
        .environment = std::move(inputs.environment),
        .socketPath = socketPath,
        .launchOptions = encodeDaemonLaunch(inputs.launch),
        .policy = {},
    });
    if (!kept)
    {
        errors << std::format("contour daemon: {}\n", vthost::keeper::describe(kept.error()));
        if (kept.error() == vthost::keeper::KeeperStartError::LabelHeld)
            errors << "contour daemon: attach to it with 'contour client', or start a separate one with "
                      "--label NAME\n";
        return EXIT_FAILURE;
    }
    return *kept;
}

#endif

DaemonLaunch launchFor(DaemonLaunch const& launch, vthost::keeper::LaunchContinuity continuity)
{
    switch (continuity)
    {
        case vthost::keeper::LaunchContinuity::Recovered: return launch.relaunched();
        case vthost::keeper::LaunchContinuity::Fresh:
        case vthost::keeper::LaunchContinuity::HandedOver: break;
    }
    return launch;
}

#ifndef _WIN32
std::expected<KeeperServeStart, std::string> attachKeeperServe(int channel)
{
    auto const clock = core::platform::SteadyClock {};
    auto attachment = vthost::keeperlink::attachToKeeper(
        vthost::keeper::UniqueFd { channel }, clock, vthost::keeperlink::AttachBound);
    if (!attachment)
        return std::unexpected { std::format("cannot reach the keeper: {}",
                                             vthost::keeperlink::describe(attachment.error())) };
    auto const launch = decodeDaemonLaunch(attachment->options);
    if (!launch)
        return std::unexpected { std::string { launch.error() == DaemonOptionsError::NewerVersion
                                                   ? "the keeper's launch record is from a newer release"
                                                   : "the keeper's launch record is unreadable" } };
    return KeeperServeStart { .launch = launchFor(*launch, attachment->continuity),
                              .attachment = *std::move(attachment) };
}

#endif

vthost::KeeperSettings keeperSettingsOf(config::DaemonKeeperConfig const& config)
{
    return vthost::KeeperSettings {
        .hungRestart =
            config.restartHung ? vthost::keeper::HungRestart::On : vthost::keeper::HungRestart::Off,
        .heartbeatTimeout = vthost::KeeperSettings {}.heartbeatTimeout,
    };
}

} // namespace contour::cli
