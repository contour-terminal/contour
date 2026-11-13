// SPDX-License-Identifier: Apache-2.0

#include <vtpty/ExecutablePath.hpp>
#include <vtpty/SpawnTrampoline.hpp>

#include <core/Environment.hpp>
#include <core/cli/App.hpp>
#include <core/cli/CLI.hpp>
#include <core/platform/Clock.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <iostream>
#include <iterator>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include <vthost/Daemon.hpp>
#include <vthost/DaemonBinary.hpp>
#include <vthost/DaemonSpawn.hpp>
#include <vthost/latency/BenchSupport.hpp>
#include <vthost/latency/E2eBench.hpp>
#include <vthost/latency/EchoChild.hpp>
#include <vthost/latency/HeadlessBench.hpp>

#ifndef _WIN32
    #include <vthost/keeper/KeeperProcess.hpp>
    #include <vthost/keeper/PosixPorts.hpp>
    #include <vthost/keeper/UniqueFd.hpp>
    #include <vthost/keeperlink/KeeperAttachment.hpp>
#endif

namespace CLI = core::cli;
using namespace std::string_literals;
using namespace std::string_view_literals;

namespace
{

/// `bench-latency`: measures what the daemon path costs on top of an in-process terminal, in first-prompt
/// time and in keystroke-to-pixel time. @see docs/internals/latency.md
class BenchLatency final: public core::cli::App
{
  public:
    /// @param env The process environment every part of the benchmark reads through.
    explicit BenchLatency(core::Environment const& env):
        App(env, "bench-latency", "Contour Daemon Latency Benchmark", CONTOUR_VERSION_STRING, "Apache-2.0")
    {
        link("bench-latency.echo-child", [this] { return echoChild(); });
        link("bench-latency.serve", [this] { return serve(); });
        link("bench-latency.headless", [this] { return headless(); });
        link("bench-latency.e2e", [this] { return e2e(); });
    }

    [[nodiscard]] CLI::Command parameterDefinition() const override
    {
        auto const secondsOption =
            CLI::Option { .name = "seconds", .v = CLI::Value { 3u }, .helpText = "How long a flood lasts." };
        auto const recordOption = CLI::Option { .name = "record",
                                                .v = CLI::Value { ""s },
                                                .helpText = "Writes the summary as TSV to this file.",
                                                .placeholder = "FILE" };
        auto const runsOption = CLI::Option { .name = "runs",
                                              .v = CLI::Value { 10u },
                                              .helpText = "Launches per mode, alternating." };
        auto const keysOption =
            CLI::Option { .name = "keys", .v = CLI::Value { 50u }, .helpText = "Measured keys per launch." };

        // CLI-boundary booleans: HeadlessOptions receives it as DaemonKeeping, serve() switches on it.
        auto const keeperOption =
            CLI::Option { .name = "keeper",
                          .v = CLI::Value { false },
                          .helpText = "Runs the daemon behind a PTY keeper, which parents it and every "
                                      "shell, as `contour daemon` does." };
        auto const mixedOption = CLI::Option {
            .name = "mixed",
            .v = CLI::Value { false },
            .helpText = "Behaves as the echo child until asked to flood (the typing and the flooding pane "
                        "of a mixed run)."
        };
        auto const echoChildOptions = CLI::OptionList {
            CLI::Option { .name = "flood",
                          .v = CLI::Value { ""s },
                          .helpText = "Floods the terminal with this workload, then waits for ^D.",
                          .placeholder = "WORKLOAD" },
            secondsOption,
            mixedOption,
        };
        auto const serveOptions = CLI::OptionList {
            CLI::Option { .name = "socket",
                          .v = CLI::Value { ""s },
                          .helpText = "The daemon's control socket.",
                          .placeholder = "PATH",
                          .presence = CLI::Presence::Required },
            CLI::Option { .name = "flood",
                          .v = CLI::Value { ""s },
                          .helpText = "Its sessions run the echo child flooding this workload.",
                          .placeholder = "WORKLOAD" },
            secondsOption,
            mixedOption,
            keeperOption,
            CLI::Option { .name = "behind-keeper",
                          .v = CLI::Value { false },
                          .helpText = "Serves as the daemon a `keeper` started, on the channel it inherited "
                                      "(internal)." },
        };
        auto const headlessOptions = CLI::OptionList {
            runsOption,
            keysOption,
            CLI::Option { .name = "workload",
                          .v = CLI::Value { ""s },
                          .helpText = "Floods with this workload and reports MiB/s instead of typing keys.",
                          .placeholder = "NAME" },
            secondsOption,
            CLI::Option { .name = "mixed",
                          .v = CLI::Value { false },
                          .helpText = "Types keys into one pane while a second pane floods `ascii` for "
                                      "`seconds`; reports keystroke latency and the flood's MiB/s." },
            CLI::Option { .name = "tabs",
                          .v = CLI::Value { false },
                          .helpText = "Opens a tab every second while a pane floods `ascii` for `seconds`; "
                                      "reports each tab's time to its prompt and the flood's MiB/s." },
            keeperOption,
            recordOption,
        };
        // The three flags are CLI-boundary booleans; runE2eBench() receives them as enums.
        auto const e2eOptions = CLI::OptionList {
            CLI::Option { .name = "contour",
                          .v = CLI::Value { ""s },
                          .helpText = "The contour executable to launch.",
                          .placeholder = "PATH",
                          .presence = CLI::Presence::Required },
            runsOption,
            keysOption,
            CLI::Option { .name = "gate",
                          .v = CLI::Value { false },
                          .helpText = "Exits non-zero unless both budgets pass." },
            CLI::Option { .name = "cold",
                          .v = CLI::Value { false },
                          .helpText = "Also measures launches with no daemon running." },
            CLI::Option { .name = "vsync",
                          .v = CLI::Value { false },
                          .helpText = "Lets the GUI's swaps wait for the vertical blank." },
            recordOption,
        };

        return CLI::Command {
            .name = "bench-latency",
            .helpText = "Contour Terminal Emulator " CONTOUR_VERSION_STRING " - daemon latency benchmark",
            .options = CLI::OptionList {},
            .children =
                CLI::CommandList {
                    CLI::Command { .name = "help", .helpText = "Shows this help and exits." },
                    CLI::Command { .name = "version", .helpText = "Shows the version and exits." },
                    CLI::Command {
                        .name = "license",
                        .helpText = "Shows the license, and project URL of the used projects and Contour." },
                    CLI::Command { .name = "echo-child",
                                   .helpText =
                                       "The application the measurements type at: prints a prompt and "
                                       "echoes what it reads until ^D. With `flood`, writes that "
                                       "workload for `seconds` instead.",
                                   .options = echoChildOptions },
                    CLI::Command { .name = "serve",
                                   .helpText = "Serves a daemon whose sessions run the echo child, until "
                                               "stopped.",
                                   .options = serveOptions },
                    CLI::Command { .name = "headless",
                                   .helpText = "Compares first-prompt and keystroke time of the echo child "
                                               "behind a SessionHost in this process and behind a daemon, "
                                               "without a GUI.",
                                   .options = headlessOptions },
                    CLI::Command { .name = "e2e",
                                   .helpText = "Runs the contour GUI in-process and against a daemon with "
                                               "--latency-probe, and judges the budgets on what the GUIs "
                                               "report.",
                                   .options = e2eOptions },
                }
        };
    }

  private:
    [[nodiscard]] static std::optional<std::filesystem::path> recordPath(std::string const& text)
    {
        if (text.empty())
            return std::nullopt;
        return std::filesystem::path(text);
    }

    int echoChild()
    {
        auto const workload = parameters().str("bench-latency.echo-child.flood");
        auto const seconds = std::chrono::seconds(parameters().uint("bench-latency.echo-child.seconds"));
        if (parameters().boolean("bench-latency.echo-child.mixed"))
            return vthost::latency::runMixedChild(workload, seconds);
        if (workload.empty())
            return vthost::latency::runEchoChild();
        return vthost::latency::runFloodChild(workload, seconds);
    }

    /// @return What `serve`'s options make of the echo child its sessions run: `mixed`, and the flood.
    [[nodiscard]] std::vector<std::string> childArguments() const
    {
        auto arguments = std::vector<std::string> {};
        if (parameters().boolean("bench-latency.serve.mixed"))
            arguments.emplace_back("mixed");
        if (auto const workload = parameters().str("bench-latency.serve.flood"); !workload.empty())
            arguments.insert(arguments.end(),
                             { "flood",
                               workload,
                               "seconds",
                               std::to_string(parameters().uint("bench-latency.serve.seconds")) });
        return arguments;
    }

    /// @param role The option that says which part the started process plays.
    /// @return This process's `serve` arguments, the keeper's own option replaced by @p role: what the daemon
    ///         behind a keeper is started with.
    [[nodiscard]] std::vector<std::string> serveArgumentsFor(std::string_view role) const
    {
        auto arguments =
            std::vector<std::string> { "serve", "socket", parameters().str("bench-latency.serve.socket") };
        std::ranges::move(childArguments(), std::back_inserter(arguments));
        arguments.emplace_back(role);
        return arguments;
    }

    int serve()
    {
        auto const self = vtpty::currentExecutablePath();
        if (!self)
        {
            std::cerr << "bench-latency: cannot find its own executable\n";
            return EXIT_FAILURE;
        }
        if (parameters().boolean("bench-latency.serve.keeper"))
        {
#ifdef _WIN32
            std::cerr << "bench-latency: `keeper` needs the PTY keeper, which this platform does not have\n";
            return EXIT_FAILURE;
#else
            return keep(*self);
#endif
        }
        auto config = vthost::DaemonConfig {};
        config.instance = vthost::drawDaemonInstance();
        config.socketPath = parameters().str("bench-latency.serve.socket");
        auto arguments = std::vector<std::string> { "echo-child" };
        std::ranges::move(childArguments(), std::back_inserter(arguments));
        config.shell = vthost::latency::benchExecInfo(*self, std::move(arguments));
        config.executablePath = *self;
        config.lifecycle = vthost::DaemonLifecycle::Persistent;
#ifndef _WIN32
        if (parameters().boolean("bench-latency.serve.behind-keeper"))
        {
            auto const clock = core::platform::SteadyClock {};
            auto attachment = vthost::keeperlink::attachToKeeper(
                vthost::keeper::UniqueFd { vthost::keeper::DaemonChannelDescriptor },
                clock,
                vthost::keeperlink::AttachBound);
            if (!attachment)
            {
                std::cerr << std::format("bench-latency: cannot reach the keeper: {}\n",
                                         vthost::keeperlink::describe(attachment.error()));
                return EXIT_FAILURE;
            }
            return vthost::runDaemon(config, std::nullopt, *std::move(attachment));
        }
#endif
        return vthost::runDaemon(config);
    }

#ifndef _WIN32
    /// Keeps the daemon: this process becomes the PTY keeper, which starts the daemon (this executable's
    /// `serve … behind-keeper`) and forks every shell on its behalf. Blocks until the keeper ends.
    /// @param self This executable, which the daemon runs as.
    /// @return The keeper's exit status.
    int keep(std::filesystem::path const& self)
    {
        auto const kept = vthost::keeper::runKeeperProcess(vthost::keeper::KeeperProcessConfig {
            .daemonImage = self,
            .daemonArguments = serveArgumentsFor("behind-keeper"),
            .environment = vthost::keeper::processEnvironmentEntries(),
            .socketPath = parameters().str("bench-latency.serve.socket"),
            .launchOptions = {},
            .policy = {},
            .images = nullptr,
        });
        if (!kept)
        {
            std::cerr << std::format("bench-latency: {}\n", vthost::keeper::describe(kept.error()));
            return EXIT_FAILURE;
        }
        return *kept;
    }
#endif

    int headless()
    {
        return vthost::latency::runHeadlessBench(
            vthost::latency::HeadlessOptions {
                .runs = parameters().uint("bench-latency.headless.runs"),
                .keys = parameters().uint("bench-latency.headless.keys"),
                .workload = parameters().str("bench-latency.headless.workload"),
                .keystrokeLoad = parameters().boolean("bench-latency.headless.mixed")
                                     ? vthost::latency::KeystrokeLoad::FloodedPane
                                     : vthost::latency::KeystrokeLoad::Idle,
                .floodDuration = std::chrono::seconds(parameters().uint("bench-latency.headless.seconds")),
                .tabs = parameters().boolean("bench-latency.headless.tabs")
                            ? vthost::latency::TabLoad::EverySecond
                            : vthost::latency::TabLoad::None,
                .keeping = parameters().boolean("bench-latency.headless.keeper")
                               ? vthost::latency::DaemonKeeping::Keeper
                               : vthost::latency::DaemonKeeping::Plain,
                .executable = std::nullopt,
                .record = recordPath(parameters().str("bench-latency.headless.record")),
            },
            std::cout,
            std::cerr);
    }

    int e2e()
    {
        using namespace vthost::latency;
        constexpr auto Prefix = "bench-latency.e2e."sv;
        auto const flag = [&](std::string_view name) {
            return parameters().boolean(std::string(Prefix) + std::string(name));
        };
        return runE2eBench(
            E2eOptions {
                .contour = parameters().str(std::string(Prefix) + "contour"),
                .runs = parameters().uint(std::string(Prefix) + "runs"),
                .keys = parameters().uint(std::string(Prefix) + "keys"),
                .gate = flag("gate") ? GateMode::Enforced : GateMode::Informational,
                .cold = flag("cold") ? ColdLaunches::Measure : ColdLaunches::Skip,
                .pacing = flag("vsync") ? FramePacing::VSync : FramePacing::Unpaced,
                .record = recordPath(parameters().str(std::string(Prefix) + "record")),
            },
            std::cout,
            std::cerr);
    }
};

} // namespace

int main(int argc, char const* argv[])
{
    // First of all: a trampoline invocation runs as nothing but the trampoline. @see SpawnTrampoline.hpp.
    vtpty::runSpawnTrampolineIfRequested(argc, argv);

    auto app = BenchLatency { core::defaultEnvironment() };
    return app.run(argc, argv);
}
