// SPDX-License-Identifier: Apache-2.0
//
// The agent orchestrator's WebAssembly module (extensions/orchestrator/guest) against the host's own
// services: checked as a module, run by a DaemonExtensionRunner over declared processes answered from the
// orchestrator's recorded fixtures, a FileExtensionStore and a recording presence, and timed in its largest
// callback.
#include <vtbackend/screen/LocalTime.hpp>

#include <core/platform/Clock.hpp>
#include <core/platform/NativeFileSystem.hpp>
#include <core/platform/testing/InMemoryFileSystem.hpp>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <print>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <vtextension/CompiledExtension.hpp>
#include <vtextension/DaemonExtensionRunner.hpp>
#include <vtextension/DaemonInstance.hpp>
#include <vtextension/DaemonLimits.hpp>
#include <vtextension/DaemonWorld.hpp>
#include <vtextension/DeclaredProcesses.hpp>
#include <vtextension/Engine.hpp>
#include <vtextension/ExtensionStore.hpp>
#include <vtextension/FileExtensionStore.hpp>
#include <vtextension/FileLocker.hpp>
#include <vtextension/IDaemonHostServices.hpp>
#include <vtextension/World.hpp>
#include <vtextension/testing/DaemonRunnerHarness.hpp>
#include <vtextension/testing/FakeDaemonHostServices.hpp>
#include <vtextension/testing/FakeDirectoryOpener.hpp>
#include <vtextension/testing/FakeFileLocker.hpp>
#include <vtextension/testing/FixtureProcessRunner.hpp>
#include <vtextension/testing/GuestModuleCheck.hpp>
#include <vtextension/testing/ManualScheduler.hpp>
#include <vtextension/testing/RunnerHarness.hpp>
#include <vtextension/testing/TestModules.hpp>
#include <vtextension/wasm/DefaultBackend.hpp>
#include <vtextension/wasm/WasmRuntime.hpp>

#ifdef VTEXTENSION_ORCHESTRATOR_MODULE

using namespace std::string_view_literals;
using namespace vtextension;
using core::platform::testing::InMemoryFileSystem;
using vtextension::testing::FakeDirectoryOpener;
using vtextension::testing::FakeFileLocker;
using vtextension::testing::FixtureProcessRunner;
using vtextension::testing::InMemoryGrantStore;
using vtextension::testing::ManualScheduler;
using vtextension::testing::RecordingPresence;
using vtextension::testing::RetirementCollector;

namespace
{

constexpr auto WorktreeRoot = "/home/u/worktrees"sv;
constexpr auto RepoPath = "/home/u/projects/contour"sv;
constexpr auto HostId = "orchestrator@test"sv;
constexpr auto GrantDigest = "test-digest"sv;
constexpr auto StoreDirectory = "/state/extensions/orchestrator/store"sv;

/// The orchestrator's configuration: one repository, with the issues assigned to the user.
constexpr auto OrchestratorConfig = R"({ "worktree_root": "~/worktrees", "models": ["opus", "sonnet"],
  "repos": [ { "name": "contour", "path": "~/projects/contour", "forge": "github",
               "issues": [ { "source": "forge", "query": "assignee:@me" } ] } ],
  "tasks": [
    { "id": "fix_ci", "key": "f", "label": "fix CI", "applies_to": "pr", "model": "sonnet",
      "prompt": "/contour-workflows:fix-ci {url}" } ] })"sv;

[[nodiscard]] std::vector<ResolvedRoot> orchestratorRoots()
{
    return { ResolvedRoot { .label = "worktrees", .paths = { std::string(WorktreeRoot) } },
             ResolvedRoot { .label = "repos", .paths = { std::string(RepoPath) } } };
}

[[nodiscard]] std::vector<std::byte> orchestratorModule()
{
    return vtextension::testing::readModuleFile(VTEXTENSION_ORCHESTRATOR_MODULE);
}

/// The orchestrator run by a DaemonExtensionRunner on a ManualScheduler, its processes declared over a
/// FixtureProcessRunner, its store a FileExtensionStore in memory. The processes and the store outlive the
/// runner, as the daemon's do.
class GuestRig
{
  public:
    GuestRig()
    {
        for (auto const path: { WorktreeRoot, RepoPath })
            directories.add(std::string(path));
        grants.record(HostId, GrantDigest, GrantState::Granted);
        auto store = FileExtensionStore::open(
            fileSystem, locker, std::filesystem::path(StoreDirectory), StoreLimits {});
        REQUIRE(store.has_value());
        _store.emplace(std::move(*store));
    }

    GuestRig(GuestRig const&) = delete;
    GuestRig(GuestRig&&) = delete;
    GuestRig& operator=(GuestRig const&) = delete;
    GuestRig& operator=(GuestRig&&) = delete;

    ~GuestRig()
    {
        stopRunner();
        _processes.reset();
    }

    /// Makes a runner on an engine of its own and runs its load.
    void startRunner()
    {
        auto engine = Engine::create(*_backend, World::Daemon, daemonWorldLimits());
        REQUIRE(engine.has_value());
        auto compiled = CompiledExtension::load(**engine, orchestratorModule());
        REQUIRE(compiled.has_value());
        if (!_processes)
            makeProcesses(**compiled);
        _runner = std::make_unique<DaemonExtensionRunner>(
            std::move(*engine),
            std::move(*compiled),
            DaemonRunnerConfig { .name = "orchestrator",
                                 .configJson = std::string(OrchestratorConfig),
                                 .roots = orchestratorRoots(),
                                 .home = "/home/u" },
            DaemonServices {
                .processes = *_processes,
                .store = *_store,
                .presence = presence,
                .tabs = tabs,
                .scheduler = scheduler,
                .wallClock = wallClock,
                .localTime = localTime,
                .retirement = retirement,
                .onLog = [this](std::string_view /*name*/, LogLevel level, std::string_view message) {
                    logged.emplace_back(level, std::string(message));
                } });
        scheduler.runPending();
    }

    /// Destroys the runner, runs its last job, and releases what it retired.
    void stopRunner()
    {
        _runner.reset();
        scheduler.runPending();
        std::ignore = retirement.take();
    }

    /// Answers every waiting process @p holdBack does not hold back, and every process they start, until
    /// none is left to answer; each answer's `procs` hint is served as it comes.
    void pollUntilIdle(FixtureProcessRunner::HoldBack const& holdBack = {})
    {
        for ([[maybe_unused]] auto const _: std::views::iota(0, 64))
        {
            scheduler.runPending();
            if (processRunner.answerWaiting(holdBack) == 0)
                return;
        }
        FAIL("the orchestrator kept starting processes");
    }

    [[nodiscard]] DaemonExtensionRunner const& runner() const
    {
        REQUIRE(_runner != nullptr);
        return *_runner;
    }

    /// @return The short text of the latest summary published.
    [[nodiscard]] std::string lastShortText() const
    {
        auto const published = presence.published();
        REQUIRE_FALSE(published.empty());
        return published.back().shortText;
    }

    FixtureProcessRunner processRunner { VTEXTENSION_ORCHESTRATOR_TESTDATA };
    FakeDirectoryOpener directories;
    InMemoryGrantStore grants;
    InMemoryFileSystem fileSystem;
    FakeFileLocker locker;
    RecordingPresence presence;
    UnservedTabs tabs; ///< This guest serves no tab here: its tab imports answer not-supported-yet.
    ManualScheduler scheduler;
    core::platform::ManualWallClock wallClock { std::chrono::system_clock::time_point {
        std::chrono::hours { 500'000 } } };
    vtbackend::LocalTimeConverter localTime { &vtbackend::systemLocalTime };
    RetirementCollector retirement;
    std::vector<std::pair<LogLevel, std::string>> logged; ///< Every message the extension logged.

  private:
    void makeProcesses(CompiledExtension const& compiled)
    {
        REQUIRE(compiled.manifest().has_value());
        auto processes = DeclaredProcesses::create(
            DeclaredProcessesConfig { .manifest = *compiled.manifest(),
                                      .roots = orchestratorRoots(),
                                      .inheritedEnvironment = { "PATH=/usr/bin" },
                                      .hostId = std::string(HostId),
                                      .grantDigest = std::string(GrantDigest) },
            DeclaredProcessesServices {
                .runner = processRunner, .directories = directories, .grants = grants, .onFinished = [this] {
                    if (_runner)
                        _runner->hint(DaemonHint::Procs);
                } });
        REQUIRE(processes.has_value());
        _processes = std::move(*processes);
    }

    std::unique_ptr<wasm::IWasmBackend> _backend = wasm::makeDefaultBackend();
    std::optional<FileExtensionStore> _store;
    std::unique_ptr<DeclaredProcesses> _processes;
    std::unique_ptr<DaemonExtensionRunner> _runner; ///< Last: destroyed before what it reaches.
};

/// @return Whether @p argv runs `gh pr`: the pull request list.
[[nodiscard]] bool isPullRequestList(std::vector<std::string> const& argv)
{
    return argv.size() >= 2 && argv[0] == "gh" && argv[1] == "pr";
}

/// @return The short text a whole poll over the fixtures publishes.
[[nodiscard]] std::string summaryOfWholePoll()
{
    auto rig = GuestRig {};
    rig.startRunner();
    rig.pollUntilIdle();
    return rig.lastShortText();
}

// {{{ the measurement

/// The store the measured callback starts from (extensions/orchestrator/core/testdata/store/, made by the
/// orchestrator's own engine): its configuration, and runs that each wait for a call.
struct MeasuredSeed
{
    /// One stored run, and the call it waits for.
    struct Run
    {
        std::string key;     ///< Its store key.
        std::string tag;     ///< The tag of the call it waits for.
        std::string command; ///< That call's declared command.
        std::string value;   ///< Its stored value.
    };

    std::string configJson; ///< The configuration the runs were made with.
    std::vector<Run> runs;  ///< The runs.
};

/// @return @p line split at every tab.
[[nodiscard]] std::vector<std::string_view> fieldsOf(std::string_view line)
{
    auto fields = std::vector<std::string_view> {};
    for (auto const field: std::views::split(line, '\t'))
        fields.emplace_back(field.begin(), field.end());
    return fields;
}

[[nodiscard]] std::string readTestdata(std::string_view relativePath)
{
    auto file = std::ifstream(std::filesystem::path(VTEXTENSION_ORCHESTRATOR_TESTDATA) / relativePath,
                              std::ios::binary);
    REQUIRE(file);
    return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
}

[[nodiscard]] MeasuredSeed measuredSeed()
{
    auto const text = readTestdata("store/runs-awaiting-worktree.tsv");
    auto seed = MeasuredSeed {};
    for (auto const line: std::views::split(std::string_view { text }, '\n'))
    {
        auto const fields = fieldsOf(std::string_view { line.begin(), line.end() });
        if (fields.size() == 2 && fields[0] == "config")
            seed.configJson = fields[1];
        else if (fields.size() == 5 && fields[0] == "run")
            seed.runs.push_back(MeasuredSeed::Run { .key = std::string(fields[1]),
                                                    .tag = std::string(fields[2]),
                                                    .command = std::string(fields[3]),
                                                    .value = std::string(fields[4]) });
    }
    REQUIRE_FALSE(seed.configJson.empty());
    REQUIRE_FALSE(seed.runs.empty());
    return seed;
}

/// The daemon world's served imports for one measured `update(procs)`: at `load`, the pull request list and
/// every seeded run's call run; before the update they all finish, the list with the 100 pull requests and
/// each call with exit 0. Store calls go to the store given.
class MeasuredServices final: public IDaemonHostServices
{
  public:
    MeasuredServices(std::string pullRequests,
                     std::span<MeasuredSeed::Run const> runs,
                     IExtensionStore& store):
        _store { store }
    {
        _processes.push_back(Process {
            .handle = 1, .tag = "poll/contour/prs", .command = "gh-prs", .output = std::move(pullRequests) });
        for (auto const& run: runs)
            _processes.push_back(Process { .handle = static_cast<uint32_t>(_processes.size() + 1),
                                           .tag = run.tag,
                                           .command = run.command,
                                           .output = {} });
    }

    /// Every process finishes.
    void finish()
    {
        for (auto& process: _processes)
            process.state = ProcResult { .completion = ProcCompletion::Exited,
                                         .exitCode = 0,
                                         .stdoutBytes = static_cast<uint32_t>(process.output.size()),
                                         .stdoutExtent = OutputExtent::Whole,
                                         .stderrBytes = 0,
                                         .stderrExtent = OutputExtent::Whole };
    }

    /// @return The store writes since the last call, which starts the count again.
    [[nodiscard]] std::size_t takeStoreWrites() noexcept { return std::exchange(_storeWrites, 0); }

    void log(LogLevel /*level*/, std::string_view /*message*/) override {}
    void wake(std::chrono::milliseconds /*after*/) override {}
    [[nodiscard]] ClockReading now() override
    {
        return ClockReading { .monotonicMs = 1'000, .wallMs = 1'794'582'527'000 };
    }

    [[nodiscard]] std::expected<uint32_t, HostError> procStart(std::string_view /*command*/,
                                                               ConfigMap const& /*args*/,
                                                               std::string_view /*cwd*/,
                                                               std::string_view /*tag*/) override
    {
        // The poll's own commands run, and are never listed: only the seeded calls answer.
        return static_cast<uint32_t>(1'000 + ++_started);
    }

    [[nodiscard]] std::expected<std::vector<ProcInfo>, HostError> procs() override
    {
        auto listed = std::vector<ProcInfo> {};
        for (auto const& process: _processes)
            listed.push_back(ProcInfo { .handle = process.handle,
                                        .tag = process.tag,
                                        .command = process.command,
                                        .state = process.state });
        return listed;
    }

    [[nodiscard]] std::expected<Chunk, HostError> procTake(uint32_t handle,
                                                           ProcStream stream,
                                                           uint32_t offset) override
    {
        auto const* const process = find(handle);
        if (process == nullptr)
            return std::unexpected(HostError::Gone);
        auto const text =
            stream == ProcStream::Stdout ? std::string_view { process->output } : std::string_view {};
        if (offset > text.size())
            return std::unexpected(HostError::BadArgument);
        auto const page = text.substr(offset, PageBytes);
        return Chunk { .bytes = ByteList(page.begin(), page.end()),
                       .remaining = static_cast<uint32_t>(text.size() - offset - page.size()) };
    }

    [[nodiscard]] std::expected<void, HostError> procRelease(uint32_t handle) override
    {
        if (std::erase_if(_processes, [&](Process const& process) { return process.handle == handle; }) == 0)
            return std::unexpected(HostError::Gone);
        return {};
    }

    [[nodiscard]] std::expected<std::optional<ByteList>, HostError> storeGet(std::string_view key) override
    {
        auto value = _store.get(key);
        if (!value)
            return std::unexpected(HostError::Gone);
        if (!*value)
            return std::nullopt;
        return ByteList((*value)->begin(), (*value)->end());
    }

    [[nodiscard]] std::expected<void, HostError> storeSet(std::string_view key,
                                                          std::span<uint8_t const> value) override
    {
        ++_storeWrites;
        return _store.set(key, std::string(value.begin(), value.end())).transform_error([](StoreError) {
            return HostError::Gone;
        });
    }

    [[nodiscard]] std::expected<void, HostError> storeRemove(std::string_view key) override
    {
        ++_storeWrites;
        return _store.remove(key).transform_error([](StoreError) { return HostError::Gone; });
    }

    [[nodiscard]] std::expected<std::vector<std::string>, HostError> storeKeys(
        std::string_view prefix) override
    {
        return _store.keys(prefix).transform_error([](StoreError) { return HostError::Gone; });
    }

    // The measured guest serves no tab here: as a host that serves none would answer.
    [[nodiscard]] std::expected<std::string, HostError> tabOpen(std::string_view scheme,
                                                                Origin origin) override
    {
        return _tabs.tabOpen(scheme, origin);
    }

    [[nodiscard]] std::expected<void, HostError> tabClose(std::string_view tab) override
    {
        return _tabs.tabClose(tab);
    }

    [[nodiscard]] std::expected<void, HostError> tabReveal(std::string_view tab, Origin origin) override
    {
        return _tabs.tabReveal(tab, origin);
    }

    [[nodiscard]] std::expected<std::vector<TabInfo>, HostError> tabs() override { return _tabs.tabs(); }

    [[nodiscard]] std::expected<ByteList, HostError> tabRead(std::string_view tab) override
    {
        return _tabs.tabRead(tab);
    }

    [[nodiscard]] std::expected<void, HostError> tabWrite(std::string_view tab,
                                                          std::span<uint8_t const> bytes) override
    {
        return _tabs.tabWrite(tab, bytes);
    }

    [[nodiscard]] std::expected<std::vector<ActionInfo>, HostError> actionsTake() override
    {
        return _tabs.actionsTake();
    }

    [[nodiscard]] std::expected<void, HostError> summarySet(std::string_view /*shortText*/,
                                                            std::string_view /*detail*/) override
    {
        return {};
    }

  private:
    UnservedTabs _tabs;
    static constexpr auto PageBytes = std::size_t { 64 } * 1024;

    /// A process the guest has not released.
    struct Process
    {
        uint32_t handle = 0;
        std::string tag;
        std::string command;
        std::string output;               ///< Its standard output, once finished.
        ProcState state = ProcRunning {}; ///< Running until finish().
    };

    [[nodiscard]] Process const* find(uint32_t handle) const
    {
        auto const found = std::ranges::find(_processes, handle, &Process::handle);
        return found != _processes.end() ? &*found : nullptr;
    }

    IExtensionStore& _store;
    std::vector<Process> _processes;
    uint32_t _started = 0;
    std::size_t _storeWrites = 0;
};

/// A store in memory: the callback without the disk.
class MemoryStore final: public IExtensionStore
{
  public:
    [[nodiscard]] std::expected<std::optional<std::string>, StoreError> get(
        std::string_view key) const override
    {
        auto const found = _values.find(key);
        if (found == _values.end())
            return std::nullopt;
        return found->second;
    }

    [[nodiscard]] std::expected<void, StoreError> set(std::string_view key, std::string_view value) override
    {
        _values.insert_or_assign(std::string(key), std::string(value));
        return {};
    }

    [[nodiscard]] std::expected<void, StoreError> remove(std::string_view key) override
    {
        if (auto const found = _values.find(key); found != _values.end())
            _values.erase(found);
        return {};
    }

    [[nodiscard]] std::expected<std::vector<std::string>, StoreError> keys(
        std::string_view prefix) const override
    {
        auto keys = std::vector<std::string> {};
        for (auto const& key: _values | std::views::keys)
            if (key.starts_with(prefix))
                keys.push_back(key);
        return keys;
    }

  private:
    std::map<std::string, std::string, std::less<>> _values;
};

/// Min, median and max of a series of durations.
struct Spread
{
    std::chrono::microseconds min {};
    std::chrono::microseconds median {};
    std::chrono::microseconds max {};
};

[[nodiscard]] Spread spreadOf(std::vector<std::chrono::microseconds> samples)
{
    std::ranges::sort(samples);
    return Spread { .min = samples.front(), .median = samples[samples.size() / 2], .max = samples.back() };
}

/// @return What `findmnt -no FSTYPE -T` says of @p directory: its filesystem's type.
[[nodiscard]] std::string fileSystemTypeOf(std::filesystem::path const& directory)
{
    auto const command = std::format("findmnt -no FSTYPE -T '{}'", directory.string());
    auto* pipe = ::popen(command.c_str(), "r");
    if (pipe == nullptr)
        return "unknown";
    auto output = std::string {};
    auto buffer = std::array<char, 256> {};
    while (::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr)
        output += buffer.data();
    std::ignore = ::pclose(pipe);
    while (!output.empty() && (output.back() == '\n' || output.back() == ' '))
        output.pop_back();
    return output.empty() ? std::string("unknown") : output;
}

/// What one series of measured callbacks took, and wrote.
struct Measured
{
    Spread spread;               ///< How long the callbacks took.
    std::size_t storeWrites = 0; ///< The durable writes the callbacks made, together.
};

/// How many callbacks a series times.
constexpr auto MeasuredRounds = 20;

/// Times MeasuredRounds `update(procs)` calls, each in a fresh instance over a freshly seeded @p store,
/// delivering the projected 100 pull requests and every seeded run's call; the seeding and the `load` are
/// outside the timed span.
[[nodiscard]] Measured measureLargestCallback(IExtensionStore& store, MeasuredSeed const& seed)
{
    auto const backend = wasm::makeDefaultBackend();
    auto const limits = daemonWorldLimits();
    auto const engine = Engine::create(*backend, World::Daemon, limits);
    REQUIRE(engine.has_value());
    auto const compiled = CompiledExtension::load(**engine, orchestratorModule());
    REQUIRE(compiled.has_value());
    auto const pullRequests = readTestdata("github/prs-100.json");
    auto clock = core::platform::SteadyClock {};
    auto const info =
        DaemonHostInfo { .roots = orchestratorRoots(), .home = "/home/u", .utcOffsetMinutes = 0 };

    auto measured = Measured {};
    auto samples = std::vector<std::chrono::microseconds> {};
    for ([[maybe_unused]] auto const _: std::views::iota(0, MeasuredRounds))
    {
        auto const keys = store.keys("");
        REQUIRE(keys.has_value());
        for (auto const& key: *keys)
            REQUIRE(store.remove(key).has_value());
        for (auto const& run: seed.runs)
            REQUIRE(store.set(run.key, run.value).has_value());

        auto services = MeasuredServices { pullRequests, seed.runs, store };
        auto instance = DaemonInstance::create(*compiled, services, clock, **engine, limits);
        REQUIRE(instance.has_value());
        REQUIRE((*instance)->load(seed.configJson, info).has_value());
        services.finish();
        std::ignore = services.takeStoreWrites();
        auto const started = std::chrono::steady_clock::now();
        auto const updated = (*instance)->update(DaemonHint::Procs);
        auto const elapsed = std::chrono::steady_clock::now() - started;
        CAPTURE(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed),
                updated ? -1 : static_cast<int>(updated.error()));
        REQUIRE(updated.has_value());
        samples.push_back(std::chrono::duration_cast<std::chrono::microseconds>(elapsed));
        measured.storeWrites += services.takeStoreWrites();
    }
    measured.spread = spreadOf(std::move(samples));
    return measured;
}

// }}}

} // namespace

TEST_CASE("DaemonGuest: the orchestrator is a well-formed daemon module", "[daemon][guest][orchestrator]")
{
    auto const module = orchestratorModule();
    CHECK(vtextension::testing::daemonGuestModuleProblems(module).empty());

    auto const backend = wasm::makeDefaultBackend();
    auto const limits = daemonWorldLimits();
    auto const engine = Engine::create(*backend, World::Daemon, limits);
    REQUIRE(engine.has_value());
    auto const compiled = CompiledExtension::load(**engine, module);
    REQUIRE(compiled.has_value());
    REQUIRE((*compiled)->manifest().has_value());
    CHECK((*compiled)->manifest()->name == "orchestrator");

    auto services = vtextension::testing::FakeDaemonHostServices {};
    services.clock = ClockReading { .monotonicMs = 1'000, .wallMs = 1'794'582'527'000 };
    auto clock = core::platform::ManualClock {};
    auto const instance = DaemonInstance::create(*compiled, services, clock, **engine, limits);
    REQUIRE(instance.has_value());
    REQUIRE((*instance)
                ->load(OrchestratorConfig, DaemonHostInfo { .roots = orchestratorRoots(), .home = "/home/u" })
                .has_value());
    // It read the store and the processes, and started its first poll.
    CHECK(std::ranges::contains(services.calls, DaemonImport::StoreKeys));
    CHECK(std::ranges::contains(services.calls, DaemonImport::Procs));
    CHECK(std::ranges::contains(
        services.procStarts, "gh-auth"sv, &vtextension::testing::ProcStartCall::command));
}

TEST_CASE("DaemonGuest: it polls and publishes its summary", "[daemon][guest][orchestrator]")
{
    auto rig = GuestRig {};
    rig.startRunner();
    rig.pollUntilIdle();

    auto const shortText = rig.lastShortText();
    CHECK(shortText.starts_with("⬢ agents"));
    CHECK(rig.processRunner.startsOf("gh", "pr") == 1);
    CHECK(rig.processRunner.startsOf("gh", "auth") == 1);
    CHECK(rig.processRunner.waiting() == 0);
    CHECK(rig.runner().status().state == DaemonRunnerState::Running);
    CHECK_FALSE(rig.runner().status().lastFault.has_value());
    CHECK(std::ranges::none_of(rig.logged, [](auto const& entry) { return entry.first == LogLevel::Error; }));
}

TEST_CASE("DaemonGuest: a restarted runner picks a finished command up by tag",
          "[daemon][guest][orchestrator]")
{
    auto const expected = summaryOfWholePoll();
    auto rig = GuestRig {};
    rig.startRunner();
    rig.pollUntilIdle(isPullRequestList);
    REQUIRE(rig.processRunner.waiting() == 1);
    REQUIRE(rig.processRunner.startsOf("gh", "pr") == 1);

    // The runner goes while `gh pr list` runs; the list finishes with no runner to tell.
    rig.stopRunner();
    REQUIRE(rig.processRunner.answerWaiting() == 1);

    rig.startRunner();
    rig.pollUntilIdle();

    CHECK(rig.processRunner.startsOf("gh", "pr") == 1);
    CHECK(rig.lastShortText() == expected);
}

TEST_CASE("DaemonGuest: [.measure] the callback that receives the largest reply",
          "[daemon][guest][orchestrator][.measure]")
{
    auto const seed = measuredSeed();
    auto memory = MemoryStore {};
    auto const withoutDisk = measureLargestCallback(memory, seed);

    auto const directory = std::filesystem::path(VTEXTENSION_MEASURE_STORE_DIR);
    std::filesystem::remove_all(directory);
    auto const fileSystem = core::platform::NativeFileSystem {};
    auto const locker = NativeFileLocker {};
    auto disk = FileExtensionStore::open(fileSystem, locker, directory, StoreLimits {});
    REQUIRE(disk.has_value());
    auto const withDisk = measureLargestCallback(*disk, seed);

    auto const deadline = daemonWorldLimits().callDeadline;
    std::println("update(procs) delivering prs-100.json and {} runs' calls, {} calls each:",
                 seed.runs.size(),
                 MeasuredRounds);
    std::println("  (a) store in memory:   min {} median {} max {} ({} store writes); headroom {} of {}",
                 withoutDisk.spread.min,
                 withoutDisk.spread.median,
                 withoutDisk.spread.max,
                 withoutDisk.storeWrites,
                 std::chrono::duration_cast<std::chrono::microseconds>(deadline) - withoutDisk.spread.max,
                 deadline);
    std::println("  (b) FileExtensionStore on {} ({}): min {} median {} max {} ({} store writes)",
                 directory.string(),
                 fileSystemTypeOf(directory),
                 withDisk.spread.min,
                 withDisk.spread.median,
                 withDisk.spread.max,
                 withDisk.storeWrites);
    // Every seeded run's call is taken in and its run stored, on disk too.
    REQUIRE(withDisk.storeWrites >= seed.runs.size() * MeasuredRounds);
    // (a) runs under the deadline; (b), whose durable writes the deadline pauses across, is recorded only.
    REQUIRE(withoutDisk.spread.max < deadline);
}

#endif
