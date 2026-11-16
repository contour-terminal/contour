// SPDX-License-Identifier: Apache-2.0
//
// Test-only: the services a DaemonExtensionRunner reaches, as test doubles that record what the runner did
// with them, and a scripted daemon guest on the fake backend.
#pragma once

#include <vtbackend/screen/LocalTime.hpp>

#include <core/platform/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vtextension/CanonicalAbi.hpp>
#include <vtextension/CompiledExtension.hpp>
#include <vtextension/DaemonExtensionRunner.hpp>
#include <vtextension/DaemonImports.hpp>
#include <vtextension/DaemonWorld.hpp>
#include <vtextension/Engine.hpp>
#include <vtextension/ExtensionPresence.hpp>
#include <vtextension/ExtensionStore.hpp>
#include <vtextension/ProcessService.hpp>
#include <vtextension/World.hpp>
#include <vtextension/testing/FakeWasmBackend.hpp>
#include <vtextension/testing/ManualScheduler.hpp>
#include <vtextension/testing/RunnerHarness.hpp>
#include <vtextension/testing/TestModules.hpp>

namespace vtextension::testing
{

/// Declared processes in memory: every start succeeds (unless `startError` is set) and runs until the test
/// finishes it.
class FakeProcessService final: public IProcessService
{
  public:
    [[nodiscard]] std::expected<ProcessHandle, ProcessError> start(std::string_view command,
                                                                   ConfigMap const& /*arguments*/,
                                                                   std::string_view /*cwd*/,
                                                                   std::string_view tag) override
    {
        if (startError)
            return std::unexpected(*startError);
        auto const handle = ProcessHandle { _nextHandle++ };
        processes.push_back(ProcessInfo { .handle = handle,
                                          .tag = std::string(tag),
                                          .command = std::string(command),
                                          .result = std::nullopt });
        return handle;
    }

    [[nodiscard]] std::vector<ProcessInfo> list() const override { return processes; }

    [[nodiscard]] std::expected<OutputPage, ProcessError> take(ProcessHandle handle,
                                                               ProcStream /*stream*/,
                                                               std::size_t /*offset*/) const override
    {
        if (!std::ranges::contains(processes, handle, &ProcessInfo::handle))
            return std::unexpected(ProcessError::UnknownHandle);
        return OutputPage {};
    }

    [[nodiscard]] std::expected<void, ProcessError> release(ProcessHandle handle) override
    {
        if (std::erase_if(processes, [&](ProcessInfo const& process) { return process.handle == handle; })
            == 0)
            return std::unexpected(ProcessError::UnknownHandle);
        return {};
    }

    std::vector<ProcessInfo> processes;     ///< Started and not released, in start order.
    std::optional<ProcessError> startError; ///< What every start fails with, if set.

  private:
    uint32_t _nextHandle = 1;
};

/// A store in memory, which can be made to fail and to take time.
class FakeExtensionStore final: public IExtensionStore
{
  public:
    [[nodiscard]] std::expected<std::optional<std::string>, StoreError> get(
        std::string_view key) const override
    {
        if (failWith)
            return std::unexpected(*failWith);
        auto const found = values.find(key);
        if (found == values.end())
            return std::nullopt;
        return found->second;
    }

    [[nodiscard]] std::expected<void, StoreError> set(std::string_view key, std::string_view value) override
    {
        ++writes;
        if (onWrite)
            onWrite();
        if (failWith)
            return std::unexpected(*failWith);
        values.insert_or_assign(std::string(key), std::string(value));
        return {};
    }

    [[nodiscard]] std::expected<void, StoreError> remove(std::string_view key) override
    {
        ++writes;
        if (onWrite)
            onWrite();
        if (failWith)
            return std::unexpected(*failWith);
        if (auto const found = values.find(key); found != values.end())
            values.erase(found);
        return {};
    }

    [[nodiscard]] std::expected<std::vector<std::string>, StoreError> keys(
        std::string_view prefix) const override
    {
        if (failWith)
            return std::unexpected(*failWith);
        return std::ranges::to<std::vector<std::string>>(
            values | std::views::keys
            | std::views::filter([&](std::string const& key) { return key.starts_with(prefix); }));
    }

    std::map<std::string, std::string, std::less<>> values; ///< What the store holds.
    std::size_t writes = 0;                                 ///< How many set() and remove() calls reached it.
    std::optional<StoreError> failWith;                     ///< What every call fails with, if set.
    std::function<void()> onWrite; ///< Runs inside every set() and remove(): a disk that takes time.
};

/// One summary an extension published.
struct PublishedSummary
{
    std::string extension;
    std::string shortText;
    std::string detail;
    [[nodiscard]] bool operator==(PublishedSummary const&) const = default;
};

/// Records every summary published.
class RecordingPresence final: public IExtensionPresence
{
  public:
    void publishSummary(std::string_view extension,
                        std::string_view shortText,
                        std::string_view detail) override
    {
        auto const lock = std::scoped_lock { _mutex };
        _published.push_back(PublishedSummary { .extension = std::string(extension),
                                                .shortText = std::string(shortText),
                                                .detail = std::string(detail) });
    }

    /// @return Every summary published, in order.
    [[nodiscard]] std::vector<PublishedSummary> published() const
    {
        auto const lock = std::scoped_lock { _mutex };
        return _published;
    }

  private:
    mutable std::mutex _mutex;
    std::vector<PublishedSummary> _published;
};

/// Every service a DaemonExtensionRunner reaches, single-threaded on a ManualScheduler, and a daemon guest
/// on the fake backend whose `load` and `update` run what the test scripts. Its `cabi_realloc` bumps from
/// AllocatorStart, starting over at every callback.
class DaemonRunnerHarness
{
  public:
    /// Where the guest's allocator starts.
    static constexpr auto AllocatorStart = uint32_t { 32 * 1024 };

    /// What a scripted `load` or `update` does; a failure faults the instance.
    using Script = std::function<std::expected<void, wasm::CallFailure>()>;

    DaemonRunnerHarness()
    {
        backend.guest.exports["cabi_realloc"] =
            [this](std::span<uint32_t> argv) -> std::expected<void, wasm::CallFailure> {
            _next = abi::alignTo(_next, argv[2]);
            argv[0] = _next;
            _next += argv[3];
            return {};
        };
        backend.guest.exports["load"] =
            [this](std::span<uint32_t> argv) -> std::expected<void, wasm::CallFailure> {
            _next = AllocatorStart;
            auto memory = FakeInstanceMemory { guest() };
            auto reader = abi::FlatReader(argv);
            auto const config = abi::Codec<std::string>::liftFlat(memory, reader);
            if (!config)
                return std::unexpected(wasm::CallFailure::Trap);
            auto const hostInfo = abi::Codec<DaemonHostInfo>::liftFlat(memory, reader);
            if (!hostInfo)
                return std::unexpected(wasm::CallFailure::Trap);
            loadedConfigs.push_back(*config);
            loadedHostInfos.push_back(*hostInfo);
            return onLoad ? onLoad() : std::expected<void, wasm::CallFailure> {};
        };
        backend.guest.exports["update"] =
            [this](std::span<uint32_t> argv) -> std::expected<void, wasm::CallFailure> {
            _next = AllocatorStart;
            updates.push_back(static_cast<DaemonHint>(argv[0]));
            return onUpdate ? onUpdate() : std::expected<void, wasm::CallFailure> {};
        };
    }

    DaemonRunnerHarness(DaemonRunnerHarness const&) = delete;
    DaemonRunnerHarness(DaemonRunnerHarness&&) = delete;
    DaemonRunnerHarness& operator=(DaemonRunnerHarness const&) = delete;
    DaemonRunnerHarness& operator=(DaemonRunnerHarness&&) = delete;

    /// Destroys the runner, runs its last job, and releases what it retired.
    ~DaemonRunnerHarness()
    {
        runner.reset();
        scheduler.runPending();
        auto const released = retirement.take();
    }

    /// @return The configuration runners are made with unless a test says otherwise.
    [[nodiscard]] static DaemonRunnerConfig defaultConfig()
    {
        return DaemonRunnerConfig {
            .name = "orchestrator", .configJson = R"({"poll_every":"2m"})", .roots = {}, .home = "/home/me"
        };
    }

    /// @return The services, recording into this harness, on @p laneScheduler. The callbacks record on the
    ///         lane: with a scheduler of real threads, read what they recorded only once it has stopped.
    [[nodiscard]] DaemonServices services(IExtensionScheduler& laneScheduler)
    {
        return DaemonServices {
            .processes = processes,
            .store = store,
            .presence = presence,
            .scheduler = laneScheduler,
            .wallClock = wallClock,
            .localTime = localTime,
            .retirement = retirement,
            .onLog = [this](std::string_view /*name*/,
                            LogLevel /*level*/,
                            std::string_view message) { logged.emplace_back(message); },
        };
    }

    /// Makes `runner`, on an engine of its own, and runs its first load.
    void start(DaemonRunnerConfig config = defaultConfig())
    {
        startOn(scheduler, std::move(config));
        scheduler.runPending();
    }

    /// Makes `runner`, on an engine of its own, on @p laneScheduler; its first load is posted there.
    void startOn(IExtensionScheduler& laneScheduler, DaemonRunnerConfig config = defaultConfig())
    {
        auto engine = Engine::create(backend, World::Daemon, config.limits);
        REQUIRE(engine.has_value());
        auto compiled = CompiledExtension::load(**engine, daemonProbe());
        REQUIRE(compiled.has_value());
        runner = std::make_unique<DaemonExtensionRunner>(
            std::move(*engine), std::move(*compiled), std::move(config), services(laneScheduler));
    }

    /// @return The instance the backend created last.
    [[nodiscard]] FakeWasmInstance& guest() const
    {
        REQUIRE(backend.lastInstance != nullptr);
        return *backend.lastInstance;
    }

    /// Writes @p text into the guest's memory at @p address.
    void put(uint32_t address, std::string_view text) const
    {
        REQUIRE(address + text.size() <= guest().linearMemory.size());
        std::ranges::transform(
            text, guest().linearMemory.begin() + address, [](char c) { return static_cast<std::byte>(c); });
    }

    /// Calls the import @p import from inside the guest, as the guest would.
    [[nodiscard]] std::expected<void, ExtensionError> call(DaemonImport import,
                                                           std::vector<uint32_t> const& arguments) const
    {
        return guest().callHost(std::to_underlying(import), arguments);
    }

    /// @return What the host stored at @p address, read as a @p T.
    template <typename T>
    [[nodiscard]] abi::Result<T> read(uint32_t address) const
    {
        auto memory = FakeInstanceMemory { guest() };
        return abi::Codec<T>::load(memory, address);
    }

    /// What the engines are created from. First: it outlives every engine retired here.
    FakeWasmBackend backend;
    ManualScheduler scheduler;
    RetirementCollector retirement;
    FakeProcessService processes;
    FakeExtensionStore store;
    RecordingPresence presence;
    core::platform::ManualWallClock wallClock;
    std::vector<std::string> logged; ///< Every message logged, in order.
    /// The time zone `load` reads its UTC offset in.
    vtbackend::LocalTimeConverter localTime { &vtbackend::systemLocalTime };
    std::vector<std::string> loadedConfigs;        ///< The configuration of every `load`, in order.
    std::vector<DaemonHostInfo> loadedHostInfos;   ///< The host-info of every `load`, in order.
    std::vector<DaemonHint> updates;               ///< The hint of every `update`, in order.
    Script onLoad;                                 ///< What `load` does once it took its configuration.
    Script onUpdate;                               ///< What `update` does.
    std::unique_ptr<DaemonExtensionRunner> runner; ///< Last: destroyed before what it reaches.

  private:
    uint32_t _next = AllocatorStart; ///< The allocator's next address.
};

} // namespace vtextension::testing
