// SPDX-License-Identifier: Apache-2.0

#include <vthost/latency/E2eBench.hpp>

#include <core/platform/Clock.hpp>

#include <algorithm>
#include <cstdlib>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <ostream>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <vthost/Daemon.hpp>
#include <vthost/DaemonBinary.hpp>
#include <vthost/Transport.hpp>
#include <vthost/client/DaemonControlClient.hpp>
#include <vthost/latency/BenchSupport.hpp>
#include <vthost/latency/EchoProtocol.hpp>
#include <vthost/latency/Gate.hpp>
#include <vthost/latency/Report.hpp>

namespace vthost::latency
{

namespace
{
    using core::platform::SteadyDuration;
    using core::platform::SteadyTimePoint;
    using namespace std::chrono_literals;

    /// The profile every launch runs.
    constexpr auto ProfileName = std::string_view { "bench" };
    /// The page the profile's terminal has.
    constexpr auto PageColumns = 120;
    constexpr auto PageLines = 40;
    /// How long a GUI launch may take altogether, probe timeouts included.
    constexpr auto LaunchTimeout = 90s;
    /// How long the daemon has to serve once started, and to be gone once stopped.
    constexpr auto DaemonReadyTimeout = 10s;
    constexpr auto DaemonGoneTimeout = 5s;

    /// Which GUI a launch runs.
    enum class LaunchMode : uint8_t
    {
        InProcess = 0, ///< `contour terminal`.
        Daemon,        ///< `contour client`.
    };

    /// What one GUI launch measured.
    struct Launch
    {
        SteadyDuration startup {};
        std::vector<SteadyDuration> keystrokes;
    };

    /// @param text A string for a YAML single-quoted scalar.
    [[nodiscard]] std::string quoteYaml(std::string const& text)
    {
        auto quoted = std::string { "'" };
        for (auto const c: text)
        {
            if (c == '\'')
                quoted += '\'';
            quoted += c;
        }
        return quoted + "'";
    }

    /// @return The configuration every launch reads: one profile whose shell is the echo child.
    [[nodiscard]] std::string benchConfig(std::filesystem::path const& self)
    {
        return std::format("default_profile: {0}\n"
                           "live_config: false\n"
                           "profiles:\n"
                           "    {0}:\n"
                           "        shell: {1}\n"
                           "        arguments: [echo-child]\n"
                           "        terminal_size: {{columns: {2}, lines: {3}}}\n"
                           "        cursor: {{blinking: false}}\n",
                           ProfileName,
                           quoteYaml(self.string()),
                           PageColumns,
                           PageLines);
    }

    /// The world one benchmark run launches GUIs in.
    class Rig
    {
      public:
        Rig(E2eOptions const& options,
            std::filesystem::path directory,
            std::filesystem::path config,
            std::string label):
            _options { options },
            _directory { std::move(directory) },
            _config { std::move(config) },
            _label { std::move(label) },
            _socket { _directory / "daemon" }
        {
        }

        [[nodiscard]] std::filesystem::path const& socketPath() const noexcept { return _socket; }

        /// @return The argv that starts the benchmark's daemon.
        [[nodiscard]] std::vector<std::string> daemonArguments() const
        {
            auto argv = daemonSpawnArgs(_options.contour.string(),
                                        _socket,
                                        DaemonSpawnOptions { .logFile = (_directory / "daemon.log").string(),
                                                             .configPath = _config.string(),
                                                             .profileName = std::string(ProfileName) },
                                        DaemonLifecycle::Persistent);
            argv.erase(argv.begin());
            return argv;
        }

        /// Launches one GUI with `--latency-probe` and reads what it reported.
        /// @param mode Which GUI.
        /// @param index Makes the report's file name unique.
        /// @return What the launch measured, or why it did not.
        [[nodiscard]] std::expected<Launch, std::string> launch(LaunchMode mode, std::size_t index) const
        {
            auto const& clock = core::platform::defaultSteadyClock();
            auto const reportFile = _directory / std::format("probe-{}.txt", index);
            auto argv = guiArguments(mode, reportFile);
            auto const spawnedAt = clock.now();
            auto process = BenchProcess::start(_options.contour, std::move(argv));
            if (!process)
                return std::unexpected(process.error());
            auto const status = (*process)->waitFor(LaunchTimeout);
            if (!status)
                return std::unexpected("the GUI did not end in time");
            return readReport(reportFile, spawnedAt);
        }

        /// Stops the daemon at the benchmark's socket, if one runs, and waits until it is gone.
        void stopDaemon() const
        {
            // Nothing listening is the usual case here, and the wait below is the check that matters.
            [[maybe_unused]] auto const stopped =
                client::makeDaemonControl(std::make_shared<UnixTransportFactory>(_socket, nullptr))
                    ->stop(proto::StopReason::User);
            auto const spawner = makeNativeDaemonSpawner();
            auto const deadline = std::chrono::steady_clock::now() + DaemonGoneTimeout;
            while (spawner->probe(_socket) == DaemonPresence::Serving
                   && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(5ms);
            auto error = std::error_code {};
            std::filesystem::remove(_socket, error);
        }

      private:
        [[nodiscard]] std::vector<std::string> guiArguments(LaunchMode mode,
                                                            std::filesystem::path const& reportFile) const
        {
            auto argv = std::vector<std::string> {};
            switch (mode)
            {
                case LaunchMode::InProcess: argv.emplace_back("terminal"); break;
                case LaunchMode::Daemon:
                    argv.emplace_back("client");
                    argv.push_back(std::format("--socket={}", _socket.string()));
                    // Unique, so that no registered service of the user's answers for this launch.
                    argv.push_back(std::format("--label={}", _label));
                    break;
            }
            argv.push_back(std::format("--config={}", _config.string()));
            argv.push_back(std::format("--profile={}", ProfileName));
            argv.push_back(std::format("--latency-probe={}", reportFile.string()));
            argv.push_back(std::format("--latency-probe-keys={}", _options.keys));
            if (_options.pacing == FramePacing::VSync)
                argv.emplace_back("--latency-probe-vsync");
            return argv;
        }

        [[nodiscard]] std::expected<Launch, std::string> readReport(std::filesystem::path const& file,
                                                                    SteadyTimePoint spawnedAt) const
        {
            auto text = std::ostringstream {};
            if (auto in = std::ifstream(file, std::ios::binary); in)
                text << in.rdbuf();
            else
                return std::unexpected("the GUI wrote no probe report");
            auto const report = parseProbeReport(text.str());
            if (!report)
                return std::unexpected(std::format("the probe report is malformed (error {})",
                                                   static_cast<int>(report.error())));
            auto const probe = ProbeResult { .phase = report->phase,
                                             .firstPrompt = report->firstPrompt,
                                             .keystrokes = report->keystrokes,
                                             .awaitedColumn = report->awaitedColumn };
            if (auto const judged = judgeProbe(probe); !judged)
                return std::unexpected(judged.error());
            if (!report->firstPrompt)
                return std::unexpected("the report names no first prompt");
            // One system-wide clock on all three platforms: a report that puts the GUI's start before our
            // spawn, or its prompt before its start, is of another clock.
            if (spawnedAt > report->processStart || report->processStart > *report->firstPrompt)
                return std::unexpected("steady clocks disagree");
            return Launch { .startup = *report->firstPrompt - spawnedAt, .keystrokes = report->keystrokes };
        }

        E2eOptions const& _options;
        std::filesystem::path _directory;
        std::filesystem::path _config;
        std::string _label;
        std::filesystem::path _socket;
    };

    /// Keeps what @p launched measured, or says why it measured nothing.
    [[nodiscard]] bool keep(std::expected<Launch, std::string> const& launched,
                            std::string_view what,
                            std::vector<SteadyDuration>& startups,
                            std::vector<SteadyDuration>* keystrokes,
                            std::ostream& err)
    {
        if (!launched)
        {
            err << std::format("bench-latency: {} failed: {}\n", what, launched.error());
            return false;
        }
        startups.push_back(launched->startup);
        if (keystrokes != nullptr)
            keystrokes->insert(keystrokes->end(), launched->keystrokes.begin(), launched->keystrokes.end());
        return true;
    }
} // namespace

int runE2eBench(E2eOptions const& given, std::ostream& out, std::ostream& err)
{
    // Absolute, as the children start in the temporary directory.
    auto options = given;
    options.contour = std::filesystem::absolute(given.contour);
    auto const self = currentExecutablePath();
    if (!self)
    {
        err << "bench-latency: cannot find its own executable\n";
        return EXIT_FAILURE;
    }
    if (!std::filesystem::exists(options.contour))
    {
        err << std::format("bench-latency: {} does not exist\n", options.contour.string());
        return EXIT_FAILURE;
    }

    auto const scratch = ScratchDirectory::create();
    if (!scratch)
    {
        err << std::format("bench-latency: {}\n", scratch.error());
        return EXIT_FAILURE;
    }
    auto const& directory = scratch->path();
    auto const config = directory / "bench.yml";
    if (!(std::ofstream(config, std::ios::binary) << benchConfig(*self)))
    {
        err << std::format("bench-latency: cannot write {}\n", config.string());
        return EXIT_FAILURE;
    }

    auto const rig = Rig { options, directory, config, directory.filename().string() };
    auto daemon = BenchProcess::start(options.contour, rig.daemonArguments());
    if (!daemon)
    {
        err << std::format("bench-latency: {}\n", daemon.error());
        return EXIT_FAILURE;
    }
    auto const spawner = makeNativeDaemonSpawner();
    if (awaitDaemon(*spawner, rig.socketPath(), DaemonReadyTimeout) != DaemonPresence::Serving)
    {
        err << "bench-latency: the daemon did not start serving";
        if (auto const status = (*daemon)->waitFor(0ms))
            err << std::format(" (it ended: {})", *status);
        auto log = std::ostringstream {};
        log << std::ifstream(directory / "daemon.log", std::ios::binary).rdbuf();
        err << (log.str().empty() ? "" : "\n" + log.str()) << '\n';
        return EXIT_FAILURE;
    }

    auto inProcessStartup = std::vector<SteadyDuration> {};
    auto inProcessKeys = std::vector<SteadyDuration> {};
    auto daemonStartup = std::vector<SteadyDuration> {};
    auto daemonKeys = std::vector<SteadyDuration> {};
    auto coldStartup = std::vector<SteadyDuration> {};
    auto coldStaging = std::vector<SteadyDuration> {};
    auto failed = false;
    auto launchIndex = std::size_t { 0 };

    for (auto const run: std::views::iota(std::size_t { 0 }, options.runs))
    {
        auto const daemonFirst = run % 2 == 1;
        for (auto const step: std::views::iota(0, 2))
        {
            if ((step == 0) == daemonFirst)
            {
                failed |= !keep(rig.launch(LaunchMode::Daemon, launchIndex++),
                                "contour client",
                                daemonStartup,
                                &daemonKeys,
                                err);
                if (!awaitEmptyDaemon(rig.socketPath()))
                {
                    err << "bench-latency: the daemon still hosts a session after the run\n";
                    failed = true;
                }
            }
            else
                failed |= !keep(rig.launch(LaunchMode::InProcess, launchIndex++),
                                "contour terminal",
                                inProcessStartup,
                                &inProcessKeys,
                                err);
        }
        if (failed)
            break;
    }

    if (!failed && options.cold == ColdLaunches::Measure)
    {
        // The daemon the client spawns ends by itself only after an idle grace, which would make the next
        // launch warm; so it is stopped here, before every launch. The first spawn after an upgrade stages a
        // copy of the install, which the timed launches must not pay: it is measured once, apart.
        rig.stopDaemon();
        failed |= !keep(rig.launch(LaunchMode::Daemon, launchIndex++),
                        "contour client (staging)",
                        coldStaging,
                        nullptr,
                        err);
        for ([[maybe_unused]] auto const run: std::views::iota(std::size_t { 0 }, options.runs))
        {
            rig.stopDaemon();
            failed |= !keep(rig.launch(LaunchMode::Daemon, launchIndex++),
                            "contour client (cold)",
                            coldStartup,
                            nullptr,
                            err);
            if (failed)
                break;
        }
        rig.stopDaemon();
    }

    rig.stopDaemon();
    [[maybe_unused]] auto const ended = (*daemon)->waitFor(2s);

    if (failed)
        return EXIT_FAILURE;

    auto report = BenchReport {};
    report.series = {
        MetricSeries { .metric = "startup", .mode = "in-process", .samples = inProcessStartup },
        MetricSeries { .metric = "startup", .mode = "daemon", .samples = daemonStartup },
        MetricSeries { .metric = "keystroke", .mode = "in-process", .samples = inProcessKeys },
        MetricSeries { .metric = "keystroke", .mode = "daemon", .samples = daemonKeys },
    };
    if (options.cold == ColdLaunches::Measure)
    {
        report.series.push_back(
            MetricSeries { .metric = "startup-cold", .mode = "daemon", .samples = coldStartup });
        report.series.push_back(
            MetricSeries { .metric = "startup-cold-staging", .mode = "daemon", .samples = coldStaging });
    }
    auto const verdicts = judge(Budgets {},
                                ModeSamples { .startup = inProcessStartup, .keystroke = inProcessKeys },
                                ModeSamples { .startup = daemonStartup, .keystroke = daemonKeys });
    report.gate = verdicts;
    if (auto const published = publish(report, options.record, out); !published)
    {
        err << std::format("bench-latency: {}\n", published.error());
        return EXIT_FAILURE;
    }
    auto const passed = verdicts.startup == Verdict::Pass && verdicts.keystroke == Verdict::Pass;
    return options.gate == GateMode::Enforced && !passed ? EXIT_FAILURE : EXIT_SUCCESS;
}

} // namespace vthost::latency
