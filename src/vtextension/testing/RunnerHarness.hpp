// SPDX-License-Identifier: Apache-2.0
//
// Test-only: the services a Runner reaches, as test doubles that record what the runner did with them.
#pragma once

#include <vtpty/CommandCapture.hpp>

#include <core/platform/Clock.hpp>
#include <core/platform/testing/InMemoryFileSystem.hpp>
#include <core/testing/Environment.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <expected>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vtextension/CommandLine.hpp>
#include <vtextension/CommandPolicy.hpp>
#include <vtextension/CompiledExtension.hpp>
#include <vtextension/Engine.hpp>
#include <vtextension/ExtensionHost.hpp>
#include <vtextension/ExtensionLimits.hpp>
#include <vtextension/IHostServices.hpp>
#include <vtextension/Manifest.hpp>
#include <vtextension/Runner.hpp>
#include <vtextension/testing/ManualScheduler.hpp>

namespace vtextension::testing
{

/// A module loaded on its own engine, from which runners are made.
struct LoadedModule
{
    std::shared_ptr<Engine> engine;
    std::shared_ptr<CompiledExtension> compiled;
};

/// Loads @p bytes on an engine of their own, which must succeed.
/// @param bytes The module.
/// @param limits What the engine allows.
/// @return The engine and the module loaded on it.
[[nodiscard]] inline LoadedModule loadModule(std::vector<std::byte> bytes, ExtensionLimits const& limits = {})
{
    auto engine = Engine::create(limits);
    REQUIRE(engine.has_value());
    auto compiled = CompiledExtension::load(**engine, std::move(bytes));
    REQUIRE(compiled.has_value());
    return LoadedModule { .engine = std::move(*engine), .compiled = std::move(*compiled) };
}

/// Reads every instant as UTC, so that no test depends on the zone of the machine it runs on.
[[nodiscard]] inline std::chrono::local_seconds utcLocalTime(
    std::chrono::system_clock::time_point when) noexcept
{
    return std::chrono::local_seconds { std::chrono::floor<std::chrono::seconds>(when).time_since_epoch() };
}

/// Grants in memory.
class InMemoryGrantStore final: public IGrantStore
{
  public:
    [[nodiscard]] GrantState lookup(std::string_view hostId, std::string_view digest) const override
    {
        auto const it = _grants.find({ std::string(hostId), std::string(digest) });
        return it != _grants.end() ? it->second : GrantState::Pending;
    }

    void record(std::string_view hostId, std::string_view digest, GrantState state) override
    {
        _grants[{ std::string(hostId), std::string(digest) }] = state;
    }

  private:
    std::map<std::pair<std::string, std::string>, GrantState> _grants;
};

/// Records the command lines it is asked to run, and answers each with `answer`.
class RecordingCommandCapture final: public vtpty::CommandCapture
{
  public:
    [[nodiscard]] std::expected<std::string, vtpty::CommandCaptureError> run(
        std::string_view command, std::chrono::milliseconds timeout, core::async::StopToken /*stop*/) override
    {
        commands.emplace_back(command);
        timeouts.push_back(timeout);
        return answer;
    }

    std::vector<std::string> commands;
    std::vector<std::chrono::milliseconds> timeouts;
    std::expected<std::string, vtpty::CommandCaptureError> answer;
};

/// Keeps what runners retire until the test releases it, on the test's thread. Allocation-free, as
/// the sink contract asks: the retired nodes are chained.
class RetirementCollector final: public IRetirementSink
{
  public:
    void retire(std::unique_ptr<RetiredExtension> retired) noexcept override
    {
        auto const lock = std::scoped_lock { _mutex };
        retired->next = std::move(_retired);
        _retired = std::move(retired);
        ++_count;
    }

    /// @return How many runners have retired.
    [[nodiscard]] std::size_t count() const
    {
        auto const lock = std::scoped_lock { _mutex };
        return _count;
    }

    /// @return What was retired, now owned by the caller (newest first, linked by `next`).
    [[nodiscard]] std::unique_ptr<RetiredExtension> take()
    {
        auto const lock = std::scoped_lock { _mutex };
        return std::move(_retired);
    }

  private:
    mutable std::mutex _mutex;
    std::unique_ptr<RetiredExtension> _retired;
    std::size_t _count = 0;
};

/// One request to ask the user about an extension's commands.
struct PermissionRequest
{
    std::string hostId;
    std::string digest;

    [[nodiscard]] bool operator==(PermissionRequest const&) const = default;
};

/// An IEngineProvider over a real EngineProvider that counts how often it is asked.
class CountingEngineProvider final: public IEngineProvider, public IRetirementSink
{
  public:
    [[nodiscard]] std::expected<std::shared_ptr<Engine>, ExtensionError> acquire() override
    {
        ++acquired;
        return _provider.acquire();
    }

    void releaseRetired() noexcept override { _provider.releaseRetired(); }
    void retire(std::unique_ptr<RetiredExtension> retired) noexcept override
    {
        _provider.retire(std::move(retired));
    }

    std::size_t acquired = 0;

  private:
    EngineProvider _provider { ExtensionLimits {} };
};

/// Every service a runner reaches, single-threaded on a ManualScheduler, recording the callbacks.
class RunnerHarness
{
  public:
    RunnerHarness() = default;

    /// @return The services, for a Runner or an ExtensionHost built on this harness.
    [[nodiscard]] RunnerServices services() { return services(scheduler); }

    /// @return The services, with @p laneScheduler in place of the harness's own. The callbacks record
    ///         on the lane: read what they recorded only once the runner is gone.
    [[nodiscard]] RunnerServices services(IExtensionScheduler& laneScheduler)
    {
        return services(laneScheduler, retirement);
    }

    /// @return The services, with @p laneScheduler and @p sink in place of the harness's own.
    [[nodiscard]] RunnerServices services(IExtensionScheduler& laneScheduler, IRetirementSink& sink)
    {
        return RunnerServices {
            .scheduler = laneScheduler,
            .grants = grants,
            .capture = capture,
            .shellDialect = shellDialect,
            .wallClock = wallClock,
            .localTime = localTime,
            .environment = environment,
            .fileSystem = fileSystem,
            .retirement = sink,
            .onSnapshotPublished = [this] { ++published; },
            .onPermissionNeeded =
                [this](std::string_view hostId, Manifest const& manifest) {
                    permissionRequests.push_back(
                        PermissionRequest { .hostId = std::string(hostId), .digest = manifest.digest });
                },
            .onLog = [this](std::string_view /*name*/,
                            LogLevel /*level*/,
                            std::string_view message) { logged.emplace_back(message); },
        };
    }

    ManualScheduler scheduler;
    RetirementCollector retirement;
    InMemoryGrantStore grants;
    RecordingCommandCapture capture;
    ShellDialect shellDialect = ShellDialect::Posix; ///< The dialect commands are recorded in, on every host.
    core::platform::ManualWallClock wallClock;
    vtbackend::LocalTimeConverter localTime = &utcLocalTime; ///< The time zone; UTC unless a test sets one.
    core::testing::FakeEnvironment environment;
    core::platform::testing::InMemoryFileSystem fileSystem;
    std::size_t published = 0; ///< How often onSnapshotPublished was called.
    std::vector<PermissionRequest> permissionRequests;
    std::vector<std::string> logged;
};

} // namespace vtextension::testing
