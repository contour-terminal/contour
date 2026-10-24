// SPDX-License-Identifier: Apache-2.0

#include <vthost/latency/HeadlessBench.hpp>

#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/testing/Workloads.hpp>

#include <vtpty/Process.hpp>
#include <vtpty/Pty.hpp>

#include <core/Environment.hpp>
#include <core/async/Task.hpp>
#include <core/async/WhenAll.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>
#include <core/platform/Clock.hpp>

#include <algorithm>
#include <cstdlib>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <ostream>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include <vthost/Daemon.hpp>
#include <vthost/DaemonBinary.hpp>
#include <vthost/SessionHost.hpp>
#include <vthost/SessionSettings.hpp>
#include <vthost/Transport.hpp>
#include <vthost/client/DaemonControlClient.hpp>
#include <vthost/client/NativeClient.hpp>
#include <vthost/latency/BenchSupport.hpp>
#include <vthost/latency/EchoChild.hpp>
#include <vthost/latency/EchoProtocol.hpp>
#include <vthost/latency/Gate.hpp>
#include <vthost/latency/KeystrokeProbe.hpp>

namespace vthost::latency
{

namespace
{
    using core::platform::SteadyDuration;
    using core::platform::SteadyTimePoint;
    using namespace std::chrono_literals;

    /// Keys typed first whose timings are dropped.
    constexpr auto WarmupKeys = std::size_t { 10 };
    /// The pause between two keys: longer than the daemon's 20 ms coalescing window, so every key finds
    /// the host idle, as one typed after a pause does.
    constexpr auto MinKeyGap = 25ms;
    constexpr auto MaxKeyGap = 35ms;
    /// How often the legs ask their probe for a key.
    constexpr auto KeyPoll = 1ms;
    /// How often the flood legs look for the end of the flood.
    constexpr auto FloodPoll = 5ms;
    /// How long a flood may overrun its duration before the leg gives up.
    constexpr auto FloodGrace = 15s;
    /// How long the daemon has to serve after it was started.
    constexpr auto DaemonReadyTimeout = 10s;
    /// How long a quit byte has to reach the daemon before the client hangs up.
    constexpr auto QuitFlushGrace = 20ms;

    /// What one launch measured.
    struct LegOutcome
    {
        std::string failure;       ///< Why the launch failed; empty if it did not.
        SteadyDuration startup {}; ///< Launch to first prompt.
        std::vector<SteadyDuration> keystrokes;
        double mibPerSecond = 0.0;    ///< Flood only.
        double deltasPerSecond = 0.0; ///< Flood on the daemon only.
    };

    [[nodiscard]] ProbePlan keyPlan(std::size_t keys)
    {
        return ProbePlan {
            .warmupKeys = WarmupKeys, .measuredKeys = keys, .minGap = MinKeyGap, .maxGap = MaxKeyGap
        };
    }

    /// Settles @p outcome from a settled probe.
    void takeProbe(LegOutcome& outcome, KeystrokeProbe const& probe, SteadyTimePoint launchedAt)
    {
        auto const result = probe.result();
        if (auto const judged = judgeProbe(result); !judged)
        {
            outcome.failure = judged.error();
            return;
        }
        if (!result.firstPrompt)
        {
            outcome.failure = "the prompt never showed";
            return;
        }
        outcome.startup = *result.firstPrompt - launchedAt;
        outcome.keystrokes = result.keystrokes;
    }

    /// Runs @p step every @p interval, on the loop, until it says it is done.
    [[nodiscard]] core::async::Task<void> repeatUntil(core::net::EventLoop* loop,
                                                      std::function<bool()> step,
                                                      std::chrono::milliseconds interval)
    {
        while (!step())
            co_await loop->delay(interval);
    }

    [[nodiscard]] std::string oneByte(char byte)
    {
        return { byte };
    }

    [[nodiscard]] std::vector<std::string> echoArguments(HeadlessOptions const& options)
    {
        if (options.workload.empty())
            return { "echo-child" };
        return {
            "echo-child", "flood", options.workload, "seconds", std::to_string(options.floodDuration.count())
        };
    }

    [[nodiscard]] double mibPerSecond(std::size_t bytes, SteadyDuration elapsed)
    {
        constexpr auto Mebibyte = 1024.0 * 1024.0;
        return static_cast<double>(bytes) / Mebibyte / std::chrono::duration<double>(elapsed).count();
    }

    // {{{ in-process leg
    /// Tells the probe where the cursor is whenever a session of the host processed output.
    class CursorFeed final: public SessionStreamEvents
    {
      public:
        CursorFeed(SessionHost& host, KeystrokeProbe& probe): _host { host }, _probe { probe } {}

        void sessionScreenUpdated(vtworkspace::SessionId session) override
        {
            _session = session;
            if (auto* const terminal = _host.terminal(session))
                _probe.frameSwapped(cursorColumnOf(*terminal));
        }

        [[nodiscard]] std::optional<vtworkspace::SessionId> session() const noexcept { return _session; }

        /// @return What session's screen shows now, as text.
        [[nodiscard]] std::string screenText() const
        {
            if (!_session)
                return {};
            auto* const terminal = _host.terminal(*_session);
            if (terminal == nullptr)
                return {};
            return terminal->runUnderStateLock(
                [&] { return terminal->displayedPage().renderMainPageText(); });
        }

      private:
        [[nodiscard]] static int cursorColumnOf(vtbackend::Terminal& terminal)
        {
            return terminal.runUnderStateLock(
                [&] { return unbox<int>(terminal.displayedPage().cursor().position.column); });
        }

        SessionHost& _host;
        KeystrokeProbe& _probe;
        std::optional<vtworkspace::SessionId> _session;
    };

    [[nodiscard]] PtyFactory echoPtyFactory(std::filesystem::path self, std::vector<std::string> arguments)
    {
        return
            [self = std::move(self), arguments = std::move(arguments)](
                vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const& /*commandOverride*/)
                -> std::unique_ptr<vtpty::Pty> {
                return std::make_unique<vtpty::Process>(benchExecInfo(self, arguments),
                                                        vtpty::createPty(size, std::nullopt),
                                                        /*escapeSandbox=*/false,
                                                        std::make_shared<vtpty::NoPlacement>());
            };
    }

    /// One launch with the echo child behind a SessionHost of this process.
    [[nodiscard]] LegOutcome runInProcessLeg(std::filesystem::path const& self,
                                             HeadlessOptions const& options)
    {
        auto const& clock = core::platform::defaultSteadyClock();
        auto outcome = LegOutcome {};
        auto const backend = core::net::makeDefaultBackend();
        auto loop = core::net::EventLoop { *backend };
        auto host = SessionHost { loop,
                                  echoPtyFactory(self, echoArguments(options)),
                                  defaultSessionSettings(),
                                  core::defaultEnvironment(),
                                  SessionHostConfig { .pumping = SessionPumping::Threaded } };
        auto probe = KeystrokeProbe { clock, keyPlan(options.keys) };
        auto feed = CursorFeed { host, probe };
        auto const subscription = makeScopedStreamSubscription(host, feed);

        auto const launchedAt = clock.now();
        if (host.createTab() == nullptr)
        {
            outcome.failure = "the host created no tab";
            return outcome;
        }

        if (options.workload.empty())
        {
            loop.blockOn(repeatUntil(
                &loop,
                [&] {
                    if (auto const key = probe.takeDueKey(); key && feed.session())
                        host.writeInput(*feed.session(), oneByte(*key));
                    return isSettled(probe.phase());
                },
                KeyPoll));
            if (feed.session())
                host.writeInput(*feed.session(), oneByte(QuitByte));
            takeProbe(outcome, probe, launchedAt);
            return outcome;
        }

        auto flooded = std::optional<std::size_t> {};
        auto const deadline = launchedAt + options.floodDuration + FloodGrace;
        auto elapsed = SteadyDuration {};
        loop.blockOn(repeatUntil(
            &loop,
            [&] {
                flooded = floodedBytes(feed.screenText());
                elapsed = clock.now() - launchedAt;
                return flooded.has_value() || clock.now() > deadline;
            },
            FloodPoll));
        if (!flooded)
        {
            outcome.failure = "the flood never ended";
            return outcome;
        }
        outcome.mibPerSecond = mibPerSecond(*flooded, elapsed);
        if (feed.session())
            host.writeInput(*feed.session(), oneByte(QuitByte));
        return outcome;
    }
    // }}}

    // {{{ daemon leg
    /// What a daemon launch learned on the loop.
    struct DaemonRun
    {
        LegOutcome outcome;
        SteadyTimePoint launchedAt {};
        std::size_t deltas = 0;
        SteadyTimePoint endedAt {};
    };

    /// Types the keys the probe asks for into @p session, then quits the child and hangs up.
    [[nodiscard]] core::async::Task<void> driveDaemonSession(core::net::EventLoop* loopPtr,
                                                             client::NativeClient* clientPtr,
                                                             HeadlessOptions const* optionsPtr,
                                                             KeystrokeProbe* probePtr,
                                                             DaemonRun* runPtr)
    {
        // Pointers in the signature, as a coroutine's parameters must not be references; all outlive it.
        auto& loop = *loopPtr;
        auto& client = *clientPtr;
        auto const& options = *optionsPtr;
        auto& probe = *probePtr;
        auto& run = *runPtr;
        auto const& clock = core::platform::defaultSteadyClock();
        auto const sessionOf = [&]() -> std::optional<uint64_t> {
            if (client.screens().empty())
                return std::nullopt;
            return client.screens().begin()->first;
        };
        if (options.workload.empty())
        {
            co_await repeatUntil(
                &loop,
                [&] {
                    if (auto const key = probe.takeDueKey(); key && sessionOf())
                        client.sendInput(*sessionOf(), oneByte(*key));
                    return isSettled(probe.phase());
                },
                KeyPoll);
            takeProbe(run.outcome, probe, run.launchedAt);
        }
        else
        {
            auto flooded = std::optional<std::size_t> {};
            auto const deadline = run.launchedAt + options.floodDuration + FloodGrace;
            co_await repeatUntil(
                &loop,
                [&] {
                    if (auto const session = sessionOf())
                        flooded = floodedBytes(client.screens().at(*session).viewportText());
                    run.endedAt = clock.now();
                    return flooded.has_value() || clock.now() > deadline;
                },
                FloodPoll);
            if (flooded)
            {
                auto const elapsed = run.endedAt - run.launchedAt;
                run.outcome.mibPerSecond = mibPerSecond(*flooded, elapsed);
                run.outcome.deltasPerSecond =
                    static_cast<double>(run.deltas) / std::chrono::duration<double>(elapsed).count();
            }
            else
                run.outcome.failure = "the flood never ended";
        }
        // The child ends on it, which ends its session.
        if (auto const session = sessionOf())
            client.sendInput(*session, oneByte(QuitByte));
        co_await loop.delay(QuitFlushGrace);
        client.detach();
    }

    [[nodiscard]] core::async::Task<void> attachAndDrive(core::net::EventLoop* loopPtr,
                                                         ITransportFactory* transport,
                                                         HeadlessOptions const* optionsPtr,
                                                         KeystrokeProbe* probePtr,
                                                         DaemonRun* runPtr)
    {
        // Pointers in the signature, as a coroutine's parameters must not be references; all outlive it.
        auto& loop = *loopPtr;
        auto const& options = *optionsPtr;
        auto& probe = *probePtr;
        auto& run = *runPtr;
        run.launchedAt = core::platform::defaultSteadyClock().now();
        auto connection = co_await transport->connect(loop);
        if (!connection)
        {
            run.outcome.failure = std::move(connection.error().message);
            co_return;
        }
        auto client = client::NativeClient {
            loop,
            std::move((*connection)->socket),
            client::NativeClient::HandshakeOptions {},
            vtbackend::CommandBlockStoreLimits {},
            client::NativeClient::UpdateHandler {
                [&](client::RemoteScreen const& screen, proto::Delta const&) {
                    ++run.deltas;
                    probe.frameSwapped(screen.cursorColumn);
                } },
            client::NativeClient::ImageHandler {},
            client::NativeClient::SessionEventHandler {},
            client::NativeClient::LayoutHandler {},
        };
        co_await core::async::whenAll(client.run(),
                                      driveDaemonSession(&loop, &client, &options, &probe, &run));
    }

    /// One launch with the echo child behind the daemon at @p socketPath.
    [[nodiscard]] LegOutcome runDaemonLeg(std::filesystem::path const& socketPath,
                                          HeadlessOptions const& options)
    {
        auto const& clock = core::platform::defaultSteadyClock();
        auto const backend = core::net::makeDefaultBackend();
        auto loop = core::net::EventLoop { *backend };
        auto probe = KeystrokeProbe { clock, keyPlan(options.keys) };
        auto run = DaemonRun {};
        // Never starts a daemon: the bench started this one itself, and a second would not be measured.
        auto transport = UnixTransportFactory { socketPath, nullptr };
        try
        {
            loop.blockOn(attachAndDrive(&loop, &transport, &options, &probe, &run));
        }
        catch (std::exception const& error)
        {
            run.outcome.failure = std::format("attaching failed: {}", error.what());
        }
        return std::move(run.outcome);
    }

    // }}}

    /// What the launches of one mode measured.
    struct ModeFigures
    {
        std::vector<SteadyDuration> startup;
        std::vector<SteadyDuration> keystroke;
        std::vector<double> mibPerSecond;
        std::vector<double> deltasPerSecond;

        /// Keeps what @p outcome measured.
        void add(LegOutcome const& outcome)
        {
            startup.push_back(outcome.startup);
            keystroke.insert(keystroke.end(), outcome.keystrokes.begin(), outcome.keystrokes.end());
            mibPerSecond.push_back(outcome.mibPerSecond);
            if (outcome.deltasPerSecond > 0.0)
                deltasPerSecond.push_back(outcome.deltasPerSecond);
        }
    };

    /// Keeps a launch's figures in @p figures, or says why it has none.
    /// @return Whether the launch succeeded.
    [[nodiscard]] bool take(LegOutcome const& outcome,
                            std::string_view mode,
                            ModeFigures& figures,
                            std::ostream& err)
    {
        if (!outcome.failure.empty())
        {
            err << std::format("bench-latency: {} run failed: {}\n", mode, outcome.failure);
            return false;
        }
        figures.add(outcome);
        return true;
    }
} // namespace

int runHeadlessBench(HeadlessOptions const& options, std::ostream& out, std::ostream& err)
{
    if (!options.workload.empty()
        && std::ranges::find(vtbackend::test::Workloads, options.workload, &vtbackend::test::Workload::name)
               == std::ranges::end(vtbackend::test::Workloads))
    {
        err << std::format("bench-latency: unknown workload '{}'. Known: {}\n", options.workload, [] {
            auto names = std::string {};
            for (auto const& row: vtbackend::test::Workloads)
                names += (names.empty() ? "" : ", ") + std::string(row.name);
            return names;
        }());
        return EXIT_FAILURE;
    }

    auto const self = currentExecutablePath();
    if (!self)
    {
        err << "bench-latency: cannot find its own executable\n";
        return EXIT_FAILURE;
    }
    auto scratch = ScratchDirectory::create();
    if (!scratch)
    {
        err << std::format("bench-latency: {}\n", scratch.error());
        return EXIT_FAILURE;
    }
    auto const socketPath = scratch->path() / "daemon";

    // The daemon's sessions run the same child the in-process leg does.
    auto serveArguments = std::vector<std::string> { "serve", "socket", socketPath.string() };
    if (!options.workload.empty())
        serveArguments.insert(
            serveArguments.end(),
            { "flood", options.workload, "seconds", std::to_string(options.floodDuration.count()) });
    auto daemon = BenchProcess::start(*self, std::move(serveArguments));
    if (!daemon)
    {
        err << std::format("bench-latency: {}\n", daemon.error());
        return EXIT_FAILURE;
    }
    auto const spawner = makeNativeDaemonSpawner();
    if (awaitDaemon(*spawner, socketPath, DaemonReadyTimeout) != DaemonPresence::Serving)
    {
        err << "bench-latency: the daemon did not start serving\n";
        return EXIT_FAILURE;
    }

    auto inProcess = ModeFigures {};
    auto viaDaemon = ModeFigures {};
    auto failed = false;

    for (auto const run: std::views::iota(std::size_t { 0 }, options.runs))
    {
        // Alternating who goes first spreads the machine's drift over both modes.
        auto const daemonFirst = run % 2 == 1;
        for (auto const step: std::views::iota(0, 2))
        {
            if ((step == 0) == daemonFirst)
            {
                failed |= !take(runDaemonLeg(socketPath, options), "daemon", viaDaemon, err);
                if (!awaitEmptyDaemon(socketPath))
                {
                    err << "bench-latency: the daemon still hosts a session after the run\n";
                    failed = true;
                }
            }
            else
                failed |= !take(runInProcessLeg(*self, options), "in-process", inProcess, err);
        }
        if (failed)
            break;
    }

    if (!client::makeDaemonControl(std::make_shared<UnixTransportFactory>(socketPath, nullptr))
             ->stop(proto::StopReason::User))
        (*daemon)->kill();
    [[maybe_unused]] auto const ended = (*daemon)->waitFor(2s);

    if (failed)
        return EXIT_FAILURE;

    auto report = BenchReport {};
    if (options.workload.empty())
    {
        report.series = {
            MetricSeries { .metric = "startup", .mode = "in-process", .samples = inProcess.startup },
            MetricSeries { .metric = "startup", .mode = "daemon", .samples = viaDaemon.startup },
            MetricSeries { .metric = "keystroke", .mode = "in-process", .samples = inProcess.keystroke },
            MetricSeries { .metric = "keystroke", .mode = "daemon", .samples = viaDaemon.keystroke },
        };
        report.gate = judge(Budgets {},
                            ModeSamples { .startup = inProcess.startup, .keystroke = inProcess.keystroke },
                            ModeSamples { .startup = viaDaemon.startup, .keystroke = viaDaemon.keystroke });
        report.gateNote = "(informational)";
    }
    else
    {
        report.scalars = {
            ScalarSeries { .metric = "flood-mibps", .mode = "in-process", .values = inProcess.mibPerSecond },
            ScalarSeries { .metric = "flood-mibps", .mode = "daemon", .values = viaDaemon.mibPerSecond },
            ScalarSeries {
                .metric = "flood-deltas-per-s", .mode = "daemon", .values = viaDaemon.deltasPerSecond },
        };
    }
    if (auto const published = publish(report, options.record, out); !published)
    {
        err << std::format("bench-latency: {}\n", published.error());
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

} // namespace vthost::latency
