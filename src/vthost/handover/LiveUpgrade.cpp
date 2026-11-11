// SPDX-License-Identifier: Apache-2.0
#include <vthost/handover/LiveUpgrade.hpp>

#include <core/Utf8Path.hpp>
#include <core/net/EventLoop.hpp>

#include <algorithm>
#include <cstddef>
#include <exception>
#include <ranges>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <vthost/ConnectionAcceptor.hpp>
#include <vthost/DaemonImage.hpp>
#include <vthost/Logging.hpp>
#include <vthost/SessionHost.hpp>
#include <vthost/handover/HandoverCapture.hpp>
#include <vthost/handover/HandoverCodec.hpp>
#include <vthost/handover/HandoverMemory.hpp>
#include <vthost/handover/HandoverRestore.hpp>
#include <vthost/handover/Readiness.hpp>

namespace vthost::handover
{

namespace
{
    /// @return The refusal a capture's refusal is to the one who asked for the upgrade.
    [[nodiscard]] UpgradeRefusal refusalOf(HandoverError error) noexcept
    {
        switch (error)
        {
            case HandoverError::UnsupportedSession: return UpgradeRefusal::UnsupportedSession;
            case HandoverError::SessionStarting: return UpgradeRefusal::SessionStarting;
            case HandoverError::InFlightTailOverflowed: return UpgradeRefusal::InFlightTailTooLarge;
            case HandoverError::ForeignConsoleHost: return UpgradeRefusal::ForeignConsoleHost;
            case HandoverError::Truncated:
            case HandoverError::UnknownFormatVersion:
            case HandoverError::Malformed:
            case HandoverError::TerminalState:
            case HandoverError::MemoryUnavailable: break;
        }
        // A capture refuses nothing else: these are a reader's.
        return UpgradeRefusal::NotSupported;
    }

    /// @return Whether some session of @p host that has not ended runs on a bundled pseudoconsole, which only
    ///         a target that adopts them can take over.
    [[nodiscard]] bool hostsPseudoconsole(SessionHost& host)
    {
        return std::ranges::any_of(host.sessionIds(), [&](auto const id) {
            auto const& device = host.hostedSession(id)->pty();
            return !device.isClosed()
                   && device.handover()
                          .transform([](vtpty::InheritedPty const& pty) { return pty.conpty.has_value(); })
                          .value_or(false);
        });
    }

    /// @param target What the target binary reports.
    /// @param host The sessions this daemon would hand over.
    /// @return Nothing unless @p target cannot adopt pseudoconsoles and @p host runs a session on one, then
    ///         TargetCannotAdoptTerminals. The report is asked first: it is a comparison, the sessions a
    ///         walk.
    [[nodiscard]] std::expected<void, UpgradeRefusal> checkHostedPseudoconsoles(ReadinessReport const& target,
                                                                                SessionHost& host)
    {
        return checkAdoptsPseudoconsoles(target).or_else(
            [&](UpgradeRefusal refusal) -> std::expected<void, UpgradeRefusal> {
                if (!hostsPseudoconsole(host))
                    return {};
                return std::unexpected(refusal);
            });
    }

    /// A captured image as a handoff takes it.
    struct EncodedImage
    {
        std::vector<vtpty::CrossingHandle> crossing; ///< Every handle it names (handlesNamedBy()).
        std::vector<uint8_t> bytes;                  ///< It, encoded (encodeHandover()).
    };

    /// @param image A captured image, taken: it is gone once this returns.
    /// @return @p image as a handoff takes it.
    [[nodiscard]] EncodedImage encodedOf(HandoverImage&& image)
    {
        // Moved into this frame, so that it is destroyed as this returns, whatever the caller holds on to.
        auto const taken = std::move(image);
        auto crossing = handlesNamedBy(taken);
        auto bytes = encodeHandover(taken);
        return EncodedImage { .crossing = std::move(crossing), .bytes = std::move(bytes) };
    }

    /// Marks an upgrade under way for as long as it lives.
    class RunningScope
    {
      public:
        explicit RunningScope(UpgradeState& state) noexcept: _state { state }
        {
            _state = UpgradeState::Running;
        }
        RunningScope(RunningScope const&) = delete;
        RunningScope(RunningScope&&) = delete;
        RunningScope& operator=(RunningScope const&) = delete;
        RunningScope& operator=(RunningScope&&) = delete;
        ~RunningScope() { _state = UpgradeState::Idle; }

      private:
        UpgradeState& _state;
    };

    /// Rolls a live upgrade back as it goes, unless the process became the new image: every session resumed
    /// with what was handed to it meanwhile, every listener accepting. Whatever ends the upgrade -- a
    /// refusal, the exec's failure, an exception -- runs it, so that no way out leaves the daemon wedged.
    class UpgradeRollback
    {
      public:
        /// @param host The host whose sessions resume. Not owned.
        /// @param listeners The listeners that accept again, as they are when this goes: emptied by
        ///        LiveUpgrade::forgetListeners() once they are gone. Not owned.
        UpgradeRollback(SessionHost& host, std::vector<ConnectionAcceptor*> const& listeners) noexcept:
            _host { host }, _listeners { listeners }
        {
        }
        UpgradeRollback(UpgradeRollback const&) = delete;
        UpgradeRollback(UpgradeRollback&&) = delete;
        UpgradeRollback& operator=(UpgradeRollback const&) = delete;
        UpgradeRollback& operator=(UpgradeRollback&&) = delete;
        ~UpgradeRollback()
        {
            // Nothing may leave a destructor that runs as an exception unwinds.
            try
            {
                rollbackCapture(_host);
            }
            catch (std::exception const& failure)
            {
                errorLog()("live upgrade: rolling the sessions back failed: {}", failure.what());
            }
            for (auto* const listener: _listeners)
                listener->resumeAccepting();
        }

      private:
        SessionHost& _host;
        std::vector<ConnectionAcceptor*> const& _listeners;
    };
} // namespace

LiveUpgrade::LiveUpgrade(core::net::EventLoop& loop,
                         SessionHost& host,
                         DaemonImageStore& store,
                         ImageLeases& leases,
                         ImageHandoff& handoff,
                         ReadinessProbe& probe,
                         UpgradeHooks& hooks,
                         std::span<ConnectionAcceptor* const> listeners,
                         LiveUpgradeConfig config):
    _loop { loop },
    _host { host },
    _store { store },
    _leases { leases },
    _handoff { handoff },
    _probe { probe },
    _hooks { hooks },
    _listeners { listeners.begin(), listeners.end() },
    _config { std::move(config) }
{
}

void LiveUpgrade::forgetListeners() noexcept
{
    _listeners.clear();
}

core::async::Task<std::expected<void, UpgradeError>> LiveUpgrade::run(UpgradeTarget target)
{
    // Refused before anything is touched: nothing is leased, probed, announced or stopped.
    if (_state == UpgradeState::Running)
        co_return std::unexpected(UpgradeRefusal::InProgress);
    if (_config.sandbox != vtpty::SandboxState::Host)
        co_return std::unexpected(UpgradeRefusal::Sandboxed);
    auto const running = RunningScope { _state };

    auto resolved = resolve(target);
    if (!resolved)
        co_return std::unexpected(resolved.error());
    // Off the loop: every client is served while the target answers.
    auto const answer = co_await _probe.probe(ReadinessQuery {
        .executable = resolved->executable, .configFile = _config.configFile, .profile = _config.profile });
    auto const report = answer.and_then([this](ReadinessReport const& target) {
        return checkReadable(target)
            .and_then([&] { return checkHostedPseudoconsoles(target, _host); })
            .and_then([&]() -> std::expected<void, UpgradeRefusal> {
                if (_config.runningConfig == RunningConfig::BuiltInDefaults)
                    return {};
                return checkConfigured(target);
            })
            .transform([&] { return target; });
    });
    if (!report)
    {
        if (report.error() == UpgradeRefusal::ConfigUnreadable && answer && answer->configProblem)
            errorLog()("live upgrade refused: {}: {}", describe(report.error()), *answer->configProblem);
        else
            errorLog()("live upgrade refused: {}", describe(report.error()));
        co_return std::unexpected(report.error());
    }
    if (auto const capturable = checkCapturable(_host); !capturable)
        co_return std::unexpected(refusalOf(capturable.error()));

    // From here on, whatever ends this but the exec is rolled back, once, as this frame unwinds.
    auto const rollback = UpgradeRollback { _host, _listeners };
    daemonLog()("live upgrade to {} ({})", report->release, core::pathToUtf8(resolved->executable));
    _hooks.announceUpgrade(report->release);
    _hooks.writeWorkspaceSnapshot();
    for (auto* const listener: _listeners)
        listener->holdAccepting();
    _hooks.closeClients();

    auto image = co_await quiesceAndCapture(resolved->lease ? resolved->lease->nativeHandle()
                                                            : vtpty::CrossingHandle {});
    if (!image)
    {
        daemonLog()("live upgrade refused: {}", describe(image.error()));
        co_return std::unexpected(image.error());
    }

    // Only the encoded image and the handles it names go on: the decoded one, every session's terminal state
    // in it, is let go of here rather than kept through the wait for the target.
    auto [crossing, bytes] = encodedOf(*std::move(image));
    crossing.insert(crossing.end(), _config.alsoCrossing.begin(), _config.alsoCrossing.end());
    // Refused here rather than read never: a new image that cannot read what it was handed orphans every
    // program.
    if (std::cmp_greater(bytes.size(), std::min(_config.maxImageSize, MaxHandoverImageSize)))
    {
        errorLog()("live upgrade refused: the handover image has {} bytes", bytes.size());
        co_return std::unexpected(UpgradeRefusal::ImageTooLarge);
    }

    auto const failure = _handoff.handOver(
        resolved->executable, argvFor(resolved->executable), std::move(crossing), std::move(bytes));
    errorLog()("live upgrade: cannot run {}: {}; serving on",
               core::pathToUtf8(resolved->executable),
               failure.cause.message());
    co_return std::unexpected(failure);
}

std::expected<LiveUpgrade::ResolvedTarget, UpgradeRefusal> LiveUpgrade::resolve(
    UpgradeTarget const& target) const
{
    if (target.imageId.empty())
    {
        // Only a daemon running in place upgrades to its installed executable: one that runs from an image
        // would exec a binary nothing leases, which a package upgrade may replace under it again.
        if (_store.layout() != StagingLayout::InPlace || _config.installedExecutable.empty())
            return std::unexpected(UpgradeRefusal::NotStaged);
        return ResolvedTarget { .executable = _config.installedExecutable, .lease = nullptr };
    }
    auto const executable = _store.executableOfImage(target.imageId);
    if (!executable)
        return std::unexpected(UpgradeRefusal::NotStaged);
    auto lease = _leases.hold(_store.leaseFileOf(_store.imageDirectoryOf(*executable)));
    // Asked again under the lease: a prune that ran before it was taken may have removed the image.
    if (!lease || !_store.executableOfImage(target.imageId))
        return std::unexpected(UpgradeRefusal::NotStaged);
    return ResolvedTarget { .executable = *executable, .lease = *std::move(lease) };
}

core::async::Task<std::expected<HandoverImage, UpgradeRefusal>> LiveUpgrade::quiesceAndCapture(
    vtpty::CrossingHandle lease)
{
    for (auto const attempt: std::views::iota(0U, std::max(1U, _config.sessionStartAttempts)))
    {
        if (attempt > 0)
            co_await _loop.delay(_config.sessionStartRetryDelay);
        // Every pump stopped, capturing nothing, so that the loop can work off what they posted before the
        // capture quiesces them -- and captures each terminal -- once.
        if (auto const stopped = _host.stopAllPumps(); !stopped)
        {
            if (stopped.error() == HostQuiesceError::SessionStarting)
                continue;
            co_return std::unexpected(UpgradeRefusal::InFlightTailTooLarge);
        }
        if (auto const settled = co_await awaitSettled(); !settled)
            co_return std::unexpected(settled.error());
        auto image =
            captureHandover(_host, crossingListeners(), lease, _config.daemonInstance, _config.release);
        if (image)
        {
            image->upgradeGeneration = _config.upgradeGeneration;
            co_return *std::move(image);
        }
        if (image.error() != HandoverError::SessionStarting)
            co_return std::unexpected(refusalOf(image.error()));
    }
    co_return std::unexpected(UpgradeRefusal::SessionStarting);
}

core::async::Task<std::expected<void, UpgradeRefusal>> LiveUpgrade::awaitSettled()
{
    auto const start = _loop.clock().now();
    auto const deadline = start + _config.settleTimeout;
    auto const abortAt = start + (_config.settleTimeout / 2);
    auto aborted = false;
    // The connections that are no attached client count too: nothing yields to the loop otherwise, and a
    // Control connection -- the requester's -- would be replaced with its answer still queued.
    auto const pending = [this] {
        return _host.connectedClients() + _hooks.endingConnections();
    };
    while (pending() != 0 || _host.pumpEventsInFlight() != 0)
    {
        auto const now = _loop.clock().now();
        if (now >= deadline)
            co_return std::unexpected(UpgradeRefusal::QuiesceTimedOut);
        if (!aborted && now >= abortAt && pending() != 0)
        {
            // Counted apart: a native client is both attached and an ending connection, and a sum would count
            // it twice.
            daemonLog()("live upgrade: {} attached client(s) and {} ending connection(s) did not go in time; "
                        "closing them",
                        _host.connectedClients(),
                        _hooks.endingConnections());
            _hooks.abortClients();
            aborted = true;
        }
        co_await _loop.delay(_config.settlePollInterval);
    }
    co_return {};
}

std::vector<InheritedListener> LiveUpgrade::crossingListeners() const
{
    auto crossing = std::vector<InheritedListener> {};
    for (auto const* const listener: _listeners)
    {
        auto const handle = listener->listeningHandle();
        if (handle.isNone())
        {
            // The new image binds it afresh: a client that connects meanwhile is refused, and retries.
            errorLog()("live upgrade: the {} listener has no descriptor to hand over", listener->name());
            continue;
        }
        auto const identity = _config.tlsIdentities.find(listener->name());
        crossing.push_back(InheritedListener {
            .name = listener->name(),
            .handle = handle,
            .tlsIdentity =
                identity != _config.tlsIdentities.end() ? std::optional { identity->second } : std::nullopt,
        });
    }
    return crossing;
}

std::vector<std::string> LiveUpgrade::argvFor(std::filesystem::path const& executable) const
{
    // A daemon started from a handover itself carries the argument that named its own image: dropped, so that
    // the command line names only the next one, however many upgrades this daemon has seen.
    auto argv =
        std::ranges::to<std::vector>(_config.argv | std::views::filter([](std::string const& argument) {
                                         return !argument.starts_with(HandoverPrefix);
                                     }));
    if (argv.empty())
        argv.emplace_back();
    argv.front() = core::pathToUtf8(executable);
    return argv;
}

} // namespace vthost::handover
