// SPDX-License-Identifier: Apache-2.0
#include <vthost/handover/LiveUpgrade.hpp>

#include <core/Utf8Path.hpp>
#include <core/net/EventLoop.hpp>

#include <algorithm>
#include <cstddef>
#include <exception>
#include <format>
#include <ranges>
#include <system_error>
#include <utility>

#include <vthost/ConnectionAcceptor.hpp>
#include <vthost/DaemonBinary.hpp>
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
    /// The argument that names the handover image's descriptor to the new image (`contour daemon`).
    constexpr auto HandoverArgument = std::string_view { "--handover-fd=" };

    /// @return The refusal a capture's refusal is to the one who asked for the upgrade.
    [[nodiscard]] UpgradeRefusal refusalOf(HandoverError error) noexcept
    {
        switch (error)
        {
            case HandoverError::UnsupportedSession: return UpgradeRefusal::UnsupportedSession;
            case HandoverError::SessionStarting: return UpgradeRefusal::SessionStarting;
            case HandoverError::InFlightTailOverflowed: return UpgradeRefusal::InFlightTailTooLarge;
            case HandoverError::Truncated:
            case HandoverError::UnknownFormatVersion:
            case HandoverError::Malformed:
            case HandoverError::TerminalState:
            case HandoverError::MemoryUnavailable:
            case HandoverError::NotSupported: break;
        }
        // A capture refuses nothing else but on a platform it does not support.
        return UpgradeRefusal::NotSupported;
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

    /// The descriptors kept across an exec, and the memory object the image is in: made close-on-exec again,
    /// and the memory object discarded, as it goes. Only an exec that succeeded never lets it go.
    class KeptAcrossExec
    {
      public:
        /// @param memory What keeps the descriptors, and holds the image.
        /// @param imageFd The memory object the image was written into, which this discards.
        /// @param descriptors The descriptors to keep (keepAll()). Not owned; they outlive this.
        KeptAcrossExec(HandoverMemory& memory, int imageFd, std::span<int const> descriptors) noexcept:
            _memory { memory }, _imageFd { imageFd }, _descriptors { descriptors }
        {
        }
        KeptAcrossExec(KeptAcrossExec const&) = delete;
        KeptAcrossExec(KeptAcrossExec&&) = delete;
        KeptAcrossExec& operator=(KeptAcrossExec const&) = delete;
        KeptAcrossExec& operator=(KeptAcrossExec&&) = delete;
        ~KeptAcrossExec()
        {
            for (auto const descriptor: _descriptors.first(_keptCount))
                if (!_memory.closeOnExecAgain(descriptor))
                    errorLog()("live upgrade: descriptor {} stays inheritable after the failed exec",
                               descriptor);
            _memory.discard(_imageFd);
        }

        /// Keeps every descriptor across the exec, in order, up to the first that cannot be kept.
        /// @return Nothing, or why one could not be kept.
        [[nodiscard]] std::expected<void, HandoverError> keepAll()
        {
            for (auto const descriptor: _descriptors)
            {
                if (auto kept = _memory.keepAcrossExec(descriptor); !kept)
                    return kept;
                ++_keptCount;
            }
            return {};
        }

      private:
        HandoverMemory& _memory;
        int _imageFd;
        std::span<int const> _descriptors;
        std::size_t _keptCount = 0; ///< How many of _descriptors, from the first, are kept.
    };
} // namespace

LiveUpgrade::LiveUpgrade(core::net::EventLoop& loop,
                         SessionHost& host,
                         DaemonImageStore& store,
                         ImageLeases& leases,
                         ProcessImageReplacer& replacer,
                         HandoverMemory& memory,
                         ReadinessProbe& probe,
                         UpgradeHooks& hooks,
                         std::span<ConnectionAcceptor* const> listeners,
                         LiveUpgradeConfig config):
    _loop { loop },
    _host { host },
    _store { store },
    _leases { leases },
    _replacer { replacer },
    _memory { memory },
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
    // The rest in an else: on a platform without an in-place exec it is no reachable code (MSVC C4702).
    if constexpr (!HasInPlaceExec)
        co_return std::unexpected(UpgradeRefusal::NotSupported);
    else
        co_return co_await upgradeInPlace(std::move(target));
}

core::async::Task<std::expected<void, UpgradeError>> LiveUpgrade::upgradeInPlace(UpgradeTarget target)
{
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
            .and_then([&]() -> std::expected<void, UpgradeRefusal> {
                if (_config.runningConfig == RunningConfig::BuiltInDefaults)
                    return {};
                return checkConfigured(target);
            })
            .transform([&] { return target; });
    });
    if (!report)
    {
        if (answer && answer->configProblem)
            errorLog()("live upgrade refused: {}", *answer->configProblem);
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

    auto const leaseFd = resolved->lease ? resolved->lease->descriptor() : -1;
    auto const image = co_await quiesceAndCapture(leaseFd);
    if (!image)
    {
        daemonLog()("live upgrade refused: {}", describe(image.error()));
        co_return std::unexpected(image.error());
    }

    // Refused here rather than read never: a new image that cannot read what it was handed orphans every
    // program.
    auto const bytes = encodeHandover(*image);
    if (std::cmp_greater(bytes.size(), std::min(_config.maxImageSize, MaxHandoverImageSize)))
    {
        errorLog()("live upgrade refused: the handover image has {} bytes", bytes.size());
        co_return std::unexpected(UpgradeRefusal::ImageTooLarge);
    }

    auto const failure = execInto(resolved->executable, *image, bytes);
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

core::async::Task<std::expected<HandoverImage, UpgradeRefusal>> LiveUpgrade::quiesceAndCapture(int leaseFd)
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
            captureHandover(_host, crossingListeners(), leaseFd, _config.daemonInstance, _config.release);
        if (image)
            co_return *std::move(image);
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
    while (_host.connectedClients() != 0 || _host.pumpEventsInFlight() != 0)
    {
        auto const now = _loop.clock().now();
        if (now >= deadline)
            co_return std::unexpected(UpgradeRefusal::QuiesceTimedOut);
        if (!aborted && now >= abortAt && _host.connectedClients() != 0)
        {
            daemonLog()("live upgrade: {} client(s) did not go in time; closing them",
                        _host.connectedClients());
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
        auto const fd = listener->listeningDescriptor();
        if (!fd)
        {
            // The new image binds it afresh: a client that connects meanwhile is refused, and retries.
            errorLog()("live upgrade: the {} listener has no descriptor to hand over", listener->name());
            continue;
        }
        auto const identity = _config.tlsIdentities.find(listener->name());
        crossing.push_back(InheritedListener {
            .name = listener->name(),
            .fd = *fd,
            .tlsIdentity =
                identity != _config.tlsIdentities.end() ? std::optional { identity->second } : std::nullopt,
        });
    }
    return crossing;
}

std::vector<std::string> LiveUpgrade::argvFor(std::filesystem::path const& executable, int handoverFd) const
{
    // A daemon started from a handover itself carries the argument that named its own image: dropped, so that
    // the command line names only this one, however many upgrades this daemon has seen.
    auto argv =
        std::ranges::to<std::vector>(_config.argv | std::views::filter([](std::string const& argument) {
                                         return !argument.starts_with(HandoverArgument);
                                     }));
    if (argv.empty())
        argv.emplace_back();
    argv.front() = core::pathToUtf8(executable);
    argv.push_back(std::format("{}{}", HandoverArgument, handoverFd));
    return argv;
}

ExecFailure LiveUpgrade::execInto(std::filesystem::path const& executable,
                                  HandoverImage const& image,
                                  std::span<uint8_t const> bytes)
{
    // Room for the image's own descriptor made before it exists: nothing between writing the memory object
    // and handing it to the guard that discards it may throw.
    auto crossing = descriptorsNamedBy(image);
    crossing.reserve(crossing.size() + 1);
    auto const fd = _memory.write(bytes);
    if (!fd)
        return ExecFailure { .cause = std::make_error_code(std::errc::not_enough_memory) };
    crossing.push_back(*fd);
    auto kept = KeptAcrossExec { _memory, *fd, crossing };
    if (!kept.keepAll())
        return ExecFailure { .cause = std::make_error_code(std::errc::bad_file_descriptor) };

    // Returns only on failure.
    auto const failure = _replacer.replaceWith(executable, argvFor(executable, *fd));
    return ExecFailure { .cause = failure ? failure : std::make_error_code(std::errc::io_error) };
}

} // namespace vthost::handover
