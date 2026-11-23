// SPDX-License-Identifier: Apache-2.0
//
// The agent orchestrator's WebAssembly module (extensions/orchestrator/guest) against the host's own
// services: checked as a module, run by a DaemonExtensionRunner over declared processes answered from the
// orchestrator's recorded fixtures, a FileExtensionStore and a recording presence, and timed in its largest
// callback and its dashboard's repaint. The timings are hidden cases of their own tag, which `[guest]` (what
// CI runs) never selects; run them in a release tree with `vtextension_test "[guest-measure]"`.
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
#include <vtextension/PaneService.hpp>
#include <vtextension/World.hpp>
#include <vtextension/testing/DaemonRunnerHarness.hpp>
#include <vtextension/testing/FakeDaemonHostServices.hpp>
#include <vtextension/testing/FakeDirectoryOpener.hpp>
#include <vtextension/testing/FakeFileLocker.hpp>
#include <vtextension/testing/FakeFileReadService.hpp>
#include <vtextension/testing/FixtureProcessRunner.hpp>
#include <vtextension/testing/GuestModuleCheck.hpp>
#include <vtextension/testing/GuestSlowdown.hpp>
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
using vtextension::testing::FakeFileReadService;
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
                .files = files,
                .panes = panes,
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
    UnservedTabs tabs;         ///< This guest serves no tab here: its tab imports answer not-supported-yet.
    FakeFileReadService files; ///< It reads no file here: `file-read` answers not-supported-yet.
    UnservedPanes panes;       ///< It opens no pane here: the pane imports answer not-supported-yet.
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

/// What a measured guest reads once its dashboard asks: a transcript page and a status board.
struct MeasuredReads
{
    std::string transcriptPage = {}; ///< What `agent-transcript` prints at once; empty: it never finishes.
    std::string board = {};          ///< What `STATUS.md` holds; empty: no file is read.
};

/// The daemon world's served imports for one measured `update(procs)`: at `load`, the pull request list and
/// every seeded run's call run; before the update they all finish, the list with the 100 pull requests and
/// each call with exit 0. Store calls go to the store given, tab calls to the tabs given; a transcript or a
/// board is read as @p reads says.
class MeasuredServices final: public IDaemonHostServices
{
  public:
    MeasuredServices(std::string pullRequests,
                     std::span<MeasuredSeed::Run const> runs,
                     IExtensionStore& store,
                     IServedTabService& tabs,
                     MeasuredReads reads = {}):
        _tabs { tabs }, _store { store }, _reads { std::move(reads) }
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
            process.state = finished(process.output);
    }

    /// @return The store writes since the last call, which starts the count again.
    [[nodiscard]] std::size_t takeStoreWrites() noexcept { return std::exchange(_storeWrites, 0); }

    void log(LogLevel /*level*/, std::string_view /*message*/) override {}
    void wake(std::chrono::milliseconds /*after*/) override {}
    [[nodiscard]] ClockReading now() override
    {
        return ClockReading { .monotonicMs = 1'000, .wallMs = 1'794'582'527'000 };
    }

    [[nodiscard]] std::expected<uint32_t, HostError> procStart(std::string_view command,
                                                               ConfigMap const& /*args*/,
                                                               std::string_view /*cwd*/,
                                                               std::string_view tag) override
    {
        auto const handle = static_cast<uint32_t>(1'000 + ++_started);
        // A transcript page asked for is printed at once, and listed finished.
        if (command == "agent-transcript" && !_reads.transcriptPage.empty())
        {
            ++_transcriptReads;
            _processes.push_back(Process { .handle = handle,
                                           .tag = std::string(tag),
                                           .command = std::string(command),
                                           .output = _reads.transcriptPage,
                                           .state = finished(_reads.transcriptPage) });
        }
        // The poll's own commands run, and are never listed: only the seeded calls answer.
        return handle;
    }

    /// @return How many transcript pages were asked for.
    [[nodiscard]] std::size_t transcriptReads() const noexcept { return _transcriptReads; }

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

    [[nodiscard]] std::expected<void, HostError> urlOpen(std::string_view url, Origin origin) override
    {
        return _tabs.urlOpen(url, origin);
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

    /// No pane is opened in a measurement: the pane imports answer as unserved ones would.
    [[nodiscard]] std::expected<std::string, HostError> paneOpen(
        std::string_view /*paneTemplate*/,
        ConfigMap const& /*args*/,
        std::string_view /*cwd*/,
        std::string_view /*title*/,
        std::string_view /*tag*/,
        std::optional<std::string_view> /*nearTab*/) override
    {
        return std::unexpected(HostError::NotSupportedYet);
    }

    [[nodiscard]] std::expected<void, HostError> paneClose(std::string_view /*pane*/) override
    {
        return std::unexpected(HostError::NotSupportedYet);
    }

    [[nodiscard]] std::expected<std::vector<PaneInfo>, HostError> panes() override
    {
        return std::unexpected(HostError::NotSupportedYet);
    }

    [[nodiscard]] std::expected<void, HostError> paneReveal(std::string_view /*pane*/,
                                                            Origin /*origin*/) override
    {
        return std::unexpected(HostError::NotSupportedYet);
    }

    [[nodiscard]] std::expected<void, HostError> paneSend(std::string_view /*pane*/,
                                                          std::string_view /*text*/) override
    {
        return std::unexpected(HostError::NotSupportedYet);
    }

    [[nodiscard]] std::expected<void, HostError> paneKey(std::string_view /*pane*/,
                                                         PaneKeyName /*key*/) override
    {
        return std::unexpected(HostError::NotSupportedYet);
    }

    /// A status board is read when one is served, written a minute before now; any other read answers as an
    /// unserved import would.
    [[nodiscard]] std::expected<FileContent, FileError> fileRead(std::string_view /*root*/,
                                                                 std::string_view path) override
    {
        if (_reads.board.empty() || !path.ends_with("STATUS.md"))
            return std::unexpected(FileError::NotSupportedYet);
        return FileContent { .bytes = ByteList(_reads.board.begin(), _reads.board.end()),
                             .modifiedUnixMs = now().wallMs - 60'000 };
    }

  private:
    /// @return A process's result: exited 0, having printed @p output.
    [[nodiscard]] static ProcResult finished(std::string_view output) noexcept
    {
        return ProcResult { .completion = ProcCompletion::Exited,
                            .exitCode = 0,
                            .stdoutBytes = static_cast<uint32_t>(output.size()),
                            .stdoutExtent = OutputExtent::Whole,
                            .stderrBytes = 0,
                            .stderrExtent = OutputExtent::Whole };
    }

    IServedTabService& _tabs;
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
    MeasuredReads _reads;
    std::vector<Process> _processes;
    uint32_t _started = 0;
    std::size_t _storeWrites = 0;
    std::size_t _transcriptReads = 0;
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

/// The tabs the measured guest serves: the first one's size and input set by the test, every tab's output
/// counted together.
class MeasuredTab final: public IServedTabService
{
  public:
    TabInfo info {
        .tab = "tab-1", .scheme = "orchestrator", .cols = 120, .rows = 40
    }; ///< The first tab tabs() lists.
    std::vector<TabInfo> alsoServed; ///< The tabs tabs() lists after it: other workspaces' dashboards.
    ByteList input;                  ///< What the next tabRead() answers.
    std::size_t written = 0;         ///< The bytes written to the tab.
    std::string firstFrame;          ///< The first write, when it came.

    [[nodiscard]] std::expected<std::string, HostError> tabOpen(std::string_view /*scheme*/,
                                                                Origin /*origin*/) override
    {
        return info.tab;
    }

    [[nodiscard]] std::expected<void, HostError> tabClose(std::string_view /*tab*/) override { return {}; }

    [[nodiscard]] std::expected<void, HostError> tabReveal(std::string_view /*tab*/,
                                                           Origin /*origin*/) override
    {
        return {};
    }

    [[nodiscard]] std::expected<void, HostError> urlOpen(std::string_view /*url*/, Origin /*origin*/) override
    {
        return {};
    }

    [[nodiscard]] std::expected<std::vector<TabInfo>, HostError> tabs() override
    {
        auto listed = std::vector { info };
        listed.insert(listed.end(), alsoServed.begin(), alsoServed.end());
        return listed;
    }

    [[nodiscard]] std::expected<ByteList, HostError> tabRead(std::string_view /*tab*/) override
    {
        return std::exchange(input, {});
    }

    [[nodiscard]] std::expected<void, HostError> tabWrite(std::string_view /*tab*/,
                                                          std::span<uint8_t const> bytes) override
    {
        if (written == 0)
            firstFrame.assign(bytes.begin(), bytes.end());
        written += bytes.size();
        return {};
    }

    [[nodiscard]] std::expected<std::vector<ActionInfo>, HostError> actionsTake() override { return {}; }

    /// Types @p text into the tab, as one chunk.
    void type(std::string_view text) { input.assign(text.begin(), text.end()); }

    /// @return The bytes written since the last call.
    [[nodiscard]] std::size_t takeWritten() noexcept { return std::exchange(written, 0); }
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
    // findmnt is util-linux, absent on macOS and Windows, and the value is only printed beside a benchmark figure.
#ifdef __linux__
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
#else
    std::ignore = directory;
    return "unknown";
#endif
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
/// outside the timed span. With @p servedTab, each callback must end with the tab's repaint; without, the
/// guest serves no tab, as a host that serves none answers.
[[nodiscard]] Measured measureLargestCallback(IExtensionStore& store,
                                              MeasuredSeed const& seed,
                                              MeasuredTab* servedTab)
{
    auto unserved = UnservedTabs {};
    auto& tabs = servedTab != nullptr ? static_cast<IServedTabService&>(*servedTab) : unserved;
    auto const backend = wasm::makeDefaultBackend();
    auto const limits = daemonWorldLimits();
    auto const engine = Engine::create(*backend, World::Daemon, limits);
    REQUIRE(engine.has_value());
    auto const compiled = CompiledExtension::load(**engine, orchestratorModule());
    REQUIRE(compiled.has_value());
    auto const pullRequests = readTestdata("github/prs-100.json");
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

        auto services = MeasuredServices { pullRequests, seed.runs, store, tabs };
        auto instance = DaemonInstance::create(*compiled, services, **engine, limits);
        REQUIRE(instance.has_value());
        REQUIRE((*instance)->load(seed.configJson, info).has_value());
        services.finish();
        std::ignore = services.takeStoreWrites();
        if (servedTab != nullptr)
            std::ignore = servedTab->takeWritten();
        auto const started = std::chrono::steady_clock::now();
        auto const updated = (*instance)->update(DaemonHint::Procs);
        auto const elapsed = std::chrono::steady_clock::now() - started;
        CAPTURE(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed),
                updated ? -1 : static_cast<int>(updated.error()));
        REQUIRE(updated.has_value());
        if (servedTab != nullptr)
            REQUIRE(servedTab->takeWritten() > 0);
        samples.push_back(std::chrono::duration_cast<std::chrono::microseconds>(elapsed));
        measured.storeWrites += services.takeStoreWrites();
    }
    measured.spread = spreadOf(std::move(samples));
    return measured;
}

/// @return @p made, which must hold a value.
template <typename T>
[[nodiscard]] T required(std::expected<T, ExtensionError> made)
{
    REQUIRE(made.has_value());
    return std::move(*made);
}

/// The orchestrator loaded over a MeasuredTab, the pull requests and the seeded runs' calls taken in.
class RepaintRig
{
  public:
    /// @param seed The configuration and the runs taken in.
    /// @param pullRequestsFixture The pull requests listed.
    /// @param limits The guest's limits: the daemon world's, which a measurement times against.
    /// @param reads What a transcript read and a board read answer.
    explicit RepaintRig(MeasuredSeed const& seed,
                        std::string_view pullRequestsFixture = "github/prs-100.json",
                        ExtensionLimits limits = daemonWorldLimits(),
                        MeasuredReads reads = {}):
        _limits { limits },
        _backend { wasm::makeDefaultBackend() },
        _engine { required(Engine::create(*_backend, World::Daemon, _limits)) },
        _compiled { required(CompiledExtension::load(*_engine, orchestratorModule())) },
        _services { readTestdata(pullRequestsFixture), seed.runs, _store, tab, std::move(reads) },
        _instance { required(DaemonInstance::create(_compiled, _services, *_engine, _limits)) }
    {
        for (auto const& run: seed.runs)
            REQUIRE(_store.set(run.key, run.value).has_value());
        REQUIRE(_instance
                    ->load(seed.configJson,
                           DaemonHostInfo {
                               .roots = orchestratorRoots(), .home = "/home/u", .utcOffsetMinutes = 0 })
                    .has_value());
        _services.finish();
        REQUIRE(_instance->update(DaemonHint::Procs).has_value());
    }

    /// Times one `update(hint)`; the tab must have been drawn by it.
    [[nodiscard]] std::chrono::microseconds timed(DaemonHint hint)
    {
        std::ignore = tab.takeWritten();
        auto const started = std::chrono::steady_clock::now();
        auto const updated = _instance->update(hint);
        auto const elapsed = std::chrono::steady_clock::now() - started;
        CAPTURE(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed),
                updated ? -1 : static_cast<int>(updated.error()));
        REQUIRE(updated.has_value());
        REQUIRE(tab.takeWritten() > 0);
        return std::chrono::duration_cast<std::chrono::microseconds>(elapsed);
    }

    /// Runs one `update(hint)` untimed.
    void update(DaemonHint hint) { REQUIRE(_instance->update(hint).has_value()); }

    /// @return How many transcript pages the guest asked for.
    [[nodiscard]] std::size_t transcriptReads() const noexcept { return _services.transcriptReads(); }

    MeasuredTab tab; ///< The tab it serves.

  private:
    ExtensionLimits _limits;
    MemoryStore _store;
    std::unique_ptr<wasm::IWasmBackend> _backend;
    std::shared_ptr<Engine> _engine;
    std::shared_ptr<CompiledExtension> _compiled;
    MeasuredServices _services;
    std::unique_ptr<DaemonInstance> _instance;
};

/// The escape sequence of the down-arrow key, as a terminal sends it.
constexpr auto DownKey = "\x1b[B"sv;
/// The escape sequence of the up-arrow key.
constexpr auto UpKey = "\x1b[A"sv;
/// How many keys the multi-key chunk carries: a paste-speed burst, or a key held down.
constexpr auto KeysPerChunk = 50;

/// @return @p key, @p count times over.
[[nodiscard]] std::string repeated(std::string_view key, int count)
{
    auto text = std::string {};
    for ([[maybe_unused]] auto const _: std::views::iota(0, count))
        text += key;
    return text;
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
    auto const instance = DaemonInstance::create(*compiled, services, **engine, limits);
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

TEST_CASE("DaemonGuest: [.measure] the callback that receives the largest reply", "[.measure][guest-measure]")
{
    auto const seed = measuredSeed();
    auto memory = MemoryStore {};
    auto const withoutDisk = measureLargestCallback(memory, seed, nullptr);

    auto const directory = std::filesystem::path(VTEXTENSION_MEASURE_STORE_DIR);
    std::filesystem::remove_all(directory);
    auto const fileSystem = core::platform::NativeFileSystem {};
    auto const locker = NativeFileLocker {};
    auto disk = FileExtensionStore::open(fileSystem, locker, directory, StoreLimits {});
    REQUIRE(disk.has_value());
    auto const withDisk = measureLargestCallback(*disk, seed, nullptr);

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

TEST_CASE("DaemonGuest: [.measure] the largest callback with two dashboards open",
          "[.measure][guest-measure]")
{
    // Two workspaces, each with its own dashboard (D12), both at the largest size measured.
    auto const seed = measuredSeed();
    auto memory = MemoryStore {};
    auto tabs = MeasuredTab {};
    tabs.alsoServed.push_back(TabInfo { .tab = "tab-2", .scheme = "orchestrator", .cols = 120, .rows = 40 });
    auto const callback = measureLargestCallback(memory, seed, &tabs);

    auto const deadline = daemonWorldLimits().callDeadline;
    std::println(
        "update(procs) delivering prs-100.json and {} runs' calls, two 120x40 dashboards open, {} calls:",
        seed.runs.size(),
        MeasuredRounds);
    std::println("  min {} median {} max {}; headroom {} of {}",
                 callback.spread.min,
                 callback.spread.median,
                 callback.spread.max,
                 std::chrono::duration_cast<std::chrono::microseconds>(deadline) - callback.spread.max,
                 deadline);
    REQUIRE(callback.spread.max < deadline);
}

TEST_CASE("DaemonGuest: the guest draws its tab and moves on a key", "[daemon][guest][orchestrator]")
{
    // The five-PR list: the deadline holds under AddressSanitizer too; ThreadSanitizer stretches it.
    auto rig = RepaintRig { MeasuredSeed { .configJson = std::string(OrchestratorConfig), .runs = {} },
                            "github/prs-open.json",
                            vtextension::testing::guestTestLimits() };
    CHECK(rig.tab.firstFrame.starts_with("\x1b[?2026h"));
    std::ignore = rig.tab.takeWritten();

    // With no run the Fleet holds `+ new agent` alone, which no move leaves: `?`, which shows the keys, is
    // the key that changes the screen.
    rig.tab.type("?");
    rig.update(DaemonHint::Tabs);
    CHECK(rig.tab.takeWritten() > 0);

    // Nothing typed, nothing changed: nothing is written.
    rig.update(DaemonHint::Tabs);
    CHECK(rig.tab.takeWritten() == 0);
}

TEST_CASE("DaemonGuest: [.measure] the dashboard's repaint", "[.measure][guest-measure]")
{
    auto const seed = measuredSeed();
    auto rig = RepaintRig { seed };
    auto const deadline =
        std::chrono::duration_cast<std::chrono::microseconds>(daemonWorldLimits().callDeadline);

    // (a) A full repaint: the tab grows from 80x24 (Medium) to 120x40 (Wide).
    auto resized = std::vector<std::chrono::microseconds> {};
    for ([[maybe_unused]] auto const _: std::views::iota(0, MeasuredRounds))
    {
        rig.tab.info.cols = 80;
        rig.tab.info.rows = 24;
        rig.update(DaemonHint::Tabs);
        rig.tab.info.cols = 120;
        rig.tab.info.rows = 40;
        resized.push_back(rig.timed(DaemonHint::Tabs));
    }

    // (b) An incremental one: one down-arrow; then a chunk of KeysPerChunk, down and back up in turn.
    auto oneKey = std::vector<std::chrono::microseconds> {};
    for ([[maybe_unused]] auto const _: std::views::iota(0, MeasuredRounds))
    {
        rig.tab.type(DownKey);
        oneKey.push_back(rig.timed(DaemonHint::Tabs));
    }
    rig.tab.type("\x1b[H"sv);
    rig.update(DaemonHint::Tabs);
    auto manyKeys = std::vector<std::chrono::microseconds> {};
    for (auto const round: std::views::iota(0, MeasuredRounds))
    {
        rig.tab.type(repeated(round % 2 == 0 ? DownKey : UpKey, KeysPerChunk));
        manyKeys.push_back(rig.timed(DaemonHint::Tabs));
    }

    // (c) The largest callback with the tab open: the reply taken in, the store in memory, and the repaint.
    auto memory = MemoryStore {};
    auto tab = MeasuredTab {};
    auto const callback = measureLargestCallback(memory, seed, &tab);

    auto const rows = std::array {
        std::pair { "(a) update(tabs) after a resize 80x24 -> 120x40", spreadOf(std::move(resized)) },
        std::pair { "(b) update(tabs) after one down-arrow", spreadOf(std::move(oneKey)) },
        std::pair { "(b) update(tabs) after a chunk of 50 arrows", spreadOf(std::move(manyKeys)) },
        std::pair { "(c) update(procs) delivering prs-100.json, tab open", callback.spread },
    };
    std::println("the dashboard's repaint, 120x40, prs-100.json and {} runs, {} calls each:",
                 seed.runs.size(),
                 MeasuredRounds);
    for (auto const& [name, spread]: rows)
        std::println("  {}: min {} median {} max {}; headroom {} of {}",
                     name,
                     spread.min,
                     spread.median,
                     spread.max,
                     deadline - spread.max,
                     deadline);
    for (auto const& [name, spread]: rows)
    {
        CAPTURE(name);
        REQUIRE(spread.max < deadline);
    }
}

namespace
{

/// @return @p seed, its configuration given a task for pull requests: the measured runs' has one for issues
///         only, and the dispatch form is measured on the first row, a pull request.
[[nodiscard]] MeasuredSeed withPullRequestTask(MeasuredSeed seed)
{
    constexpr auto Tasks = R"("tasks":[)"sv;
    auto const at = seed.configJson.find(Tasks);
    REQUIRE(at != std::string::npos);
    seed.configJson.insert(at + Tasks.size(),
                           R"({"id":"fix_ci","key":"f","label":"fix CI","applies_to":"pr","model":"sonnet",)"
                           R"("prompt":"/contour-workflows:fix-ci {url}"},)");
    return seed;
}

} // namespace

TEST_CASE("DaemonGuest: [.measure] the dispatch form's repaint", "[.measure][guest-measure]")
{
    auto const deadline =
        std::chrono::duration_cast<std::chrono::microseconds>(daemonWorldLimits().callDeadline);
    // The measured runs fill every agent slot, so the form draws the refusal; with no run it draws what the
    // dispatch would start, command line included.
    auto const seeds = std::array {
        std::pair { "the four measured runs (the form refuses: max_agents)",
                    withPullRequestTask(measuredSeed()) },
        std::pair { "no run (the form shows the whole dispatch)",
                    MeasuredSeed { .configJson = std::string(OrchestratorConfig), .runs = {} } },
    };
    for (auto const& [label, seed]: seeds)
    {
        CAPTURE(label);
        // (a) The form opened over the 120x40 dashboard with `d`. The rig's clock stands still, so an Esc
        // would never close it: each round opens it in a rig of its own, the lists already taken in.
        auto opened = std::vector<std::chrono::microseconds> {};
        for ([[maybe_unused]] auto const _: std::views::iota(0, MeasuredRounds))
        {
            auto fresh = RepaintRig { seed };
            fresh.tab.type("d"sv);
            opened.push_back(fresh.timed(DaemonHint::Tabs));
        }

        auto rig = RepaintRig { seed };
        // (b) A full repaint with the form open: the tab grows from 80x24 to 120x40.
        rig.tab.type("d"sv);
        rig.update(DaemonHint::Tabs);
        auto resized = std::vector<std::chrono::microseconds> {};
        for ([[maybe_unused]] auto const _: std::views::iota(0, MeasuredRounds))
        {
            rig.tab.info.cols = 80;
            rig.tab.info.rows = 24;
            rig.update(DaemonHint::Tabs);
            rig.tab.info.cols = 120;
            rig.tab.info.rows = 40;
            resized.push_back(rig.timed(DaemonHint::Tabs));
        }

        // (c) Incremental: one key in the form (the model cycled); 50 characters typed into the note at once.
        rig.tab.type("\t"sv);
        rig.update(DaemonHint::Tabs);
        auto oneKey = std::vector<std::chrono::microseconds> {};
        for ([[maybe_unused]] auto const _: std::views::iota(0, MeasuredRounds))
        {
            rig.tab.type("\x1b[C"sv);
            oneKey.push_back(rig.timed(DaemonHint::Tabs));
        }
        rig.tab.type("\t"sv);
        rig.update(DaemonHint::Tabs);
        auto note = std::vector<std::chrono::microseconds> {};
        for ([[maybe_unused]] auto const _: std::views::iota(0, MeasuredRounds))
        {
            rig.tab.type(repeated("x"sv, KeysPerChunk));
            note.push_back(rig.timed(DaemonHint::Tabs));
        }

        auto const rows = std::array {
            std::pair { "(a) update(tabs) opening the form with d", spreadOf(std::move(opened)) },
            std::pair { "(b) update(tabs) after a resize 80x24 -> 120x40, form open",
                        spreadOf(std::move(resized)) },
            std::pair { "(c) update(tabs) after one key in the form", spreadOf(std::move(oneKey)) },
            std::pair { "(c) update(tabs) after 50 characters of note", spreadOf(std::move(note)) },
        };
        std::println(
            "the dispatch form's repaint, 120x40, prs-100.json, {}, {} calls each:", label, MeasuredRounds);
        for (auto const& [name, spread]: rows)
            std::println("  {}: min {} median {} max {}; headroom {} of {}",
                         name,
                         spread.min,
                         spread.median,
                         spread.max,
                         deadline - spread.max,
                         deadline);
        for (auto const& [name, spread]: rows)
        {
            CAPTURE(name);
            REQUIRE(spread.max < deadline);
        }
    }
}

namespace
{

/// The worst-case transcript page: 30 entries of 2 KiB of text, every byte a `"` or a `\` that JSON escapes,
/// so about 124 KiB as `contour agent transcript --json` prints it.
[[nodiscard]] std::string worstTranscriptPage()
{
    constexpr auto Entries = 30;
    constexpr auto TextBytes = std::size_t { 2048 };
    auto escaped = std::string {};
    while (escaped.size() < 2 * TextBytes)
        escaped += R"(\"\\)";
    escaped.resize(2 * TextBytes);
    auto page = std::string { R"({"format":1,"session":"00000000-0000-4000-8000-000000000001",)"
                              R"("writtenBy":"2.1.328","page":1,"pages":3,"notUnderstood":0,"entries":[)" };
    for (auto const index: std::views::iota(0, Entries))
        page += std::format(R"({}{{"kind":"{}","time":"2026-11-19T18:36:55.086Z","text":"{}"}})",
                            index == 0 ? "" : ",",
                            index % 2 == 0 ? "tool-result" : "assistant",
                            escaped);
    return page + "]}";
}

/// @return A status board of 64 KiB, the most a file read returns: a markdown list of short lines.
[[nodiscard]] std::string largestBoard()
{
    constexpr auto Bytes = std::size_t { 64 } * 1024;
    auto board = std::string { "# Status\n\n" };
    for (auto index = 0; board.size() < Bytes; ++index)
        board += std::format(
            "- step {}: reproduced, fixed and tested the reflow of a wrapped line's last cell\n", index);
    board.resize(Bytes);
    return board;
}

} // namespace

TEST_CASE("DaemonGuest: [.measure] the transcript's and the board's repaint", "[.measure][guest-measure]")
{
    auto const deadline =
        std::chrono::duration_cast<std::chrono::microseconds>(daemonWorldLimits().callDeadline);
    auto const seed = measuredSeed();
    auto const page = worstTranscriptPage();
    auto const board = largestBoard();
    std::println("the worst transcript page is {} bytes; the board {} bytes", page.size(), board.size());

    // (a) The worst page asked for with `t` on the last run's row, then taken in and drawn at 120x40; again
    // after a round through its board (`b`, unserved here), which leaves the page asked for anew.
    auto transcript = RepaintRig {
        seed, "github/prs-100.json", daemonWorldLimits(), MeasuredReads { .transcriptPage = page }
    };
    transcript.tab.type("\x1b[F"sv);
    transcript.update(DaemonHint::Tabs);
    auto asked = std::vector<std::chrono::microseconds> {};
    auto delivered = std::vector<std::chrono::microseconds> {};
    for (auto const round: std::views::iota(0, MeasuredRounds))
    {
        transcript.tab.type(round == 0 ? "t"sv : "bt"sv);
        asked.push_back(transcript.timed(DaemonHint::Tabs));
        delivered.push_back(transcript.timed(DaemonHint::Procs));
    }
    REQUIRE(transcript.transcriptReads() == static_cast<std::size_t>(MeasuredRounds));

    // (b) A full repaint of the page: the tab grows from 80x24 to 120x40, the page wrapped anew.
    auto resized = std::vector<std::chrono::microseconds> {};
    for ([[maybe_unused]] auto const _: std::views::iota(0, MeasuredRounds))
    {
        transcript.tab.info.cols = 80;
        transcript.tab.info.rows = 24;
        transcript.update(DaemonHint::Tabs);
        transcript.tab.info.cols = 120;
        transcript.tab.info.rows = 40;
        resized.push_back(transcript.timed(DaemonHint::Tabs));
    }

    // (c) Scrolling the page by a screen.
    auto scrolled = std::vector<std::chrono::microseconds> {};
    for (auto const round: std::views::iota(0, MeasuredRounds))
    {
        transcript.tab.type(round % 2 == 0 ? "\x1b[6~"sv : "\x1b[5~"sv);
        scrolled.push_back(transcript.timed(DaemonHint::Tabs));
    }

    // (d) The 64 KiB board read and drawn with `b`; again after a round through its transcript (`t`, left
    // unanswered here), which draws the board anew and reads it again now.
    auto boards =
        RepaintRig { seed, "github/prs-100.json", daemonWorldLimits(), MeasuredReads { .board = board } };
    boards.tab.type("\x1b[F"sv);
    boards.update(DaemonHint::Tabs);
    auto shown = std::vector<std::chrono::microseconds> {};
    for (auto const round: std::views::iota(0, MeasuredRounds))
    {
        boards.tab.type(round == 0 ? "b"sv : "tb"sv);
        shown.push_back(boards.timed(DaemonHint::Tabs));
    }
    // (e) A full repaint of the board: the tab grows from 80x24 to 120x40, the board wrapped anew.
    auto boardResized = std::vector<std::chrono::microseconds> {};
    for ([[maybe_unused]] auto const _: std::views::iota(0, MeasuredRounds))
    {
        boards.tab.info.cols = 80;
        boards.tab.info.rows = 24;
        boards.update(DaemonHint::Tabs);
        boards.tab.info.cols = 120;
        boards.tab.info.rows = 40;
        boardResized.push_back(boards.timed(DaemonHint::Tabs));
    }

    auto const rows = std::array {
        std::pair { "(a) update(tabs) asking for the page with t", spreadOf(std::move(asked)) },
        std::pair { "(a) update(procs) delivering the worst page, drawn", spreadOf(std::move(delivered)) },
        std::pair { "(b) update(tabs) after a resize 80x24 -> 120x40, page shown",
                    spreadOf(std::move(resized)) },
        std::pair { "(c) update(tabs) after a page key", spreadOf(std::move(scrolled)) },
        std::pair { "(d) update(tabs) reading and drawing the 64 KiB board", spreadOf(std::move(shown)) },
        std::pair { "(e) update(tabs) after a resize 80x24 -> 120x40, board shown",
                    spreadOf(std::move(boardResized)) },
    };
    std::println("the transcript's and the board's repaint, 120x40, prs-100.json, {} calls each:",
                 MeasuredRounds);
    for (auto const& [name, spread]: rows)
        std::println("  {}: min {} median {} max {}; headroom {} of {}",
                     name,
                     spread.min,
                     spread.median,
                     spread.max,
                     deadline - spread.max,
                     deadline);
    for (auto const& [name, spread]: rows)
    {
        CAPTURE(name);
        REQUIRE(spread.max < deadline);
    }
}

#endif
