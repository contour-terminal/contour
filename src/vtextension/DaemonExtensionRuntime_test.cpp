// SPDX-License-Identifier: Apache-2.0
//
// The daemon world, composed from declarations: a scripted guest on the fake backend, its lane a real worker,
// and fakes of the files, the locks, the processes and the directories.
#include <vtbackend/screen/LocalTime.hpp>

#include <core/platform/testing/InMemoryFileSystem.hpp>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <latch>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <vtextension/DaemonExtensionRuntime.hpp>
#include <vtextension/DaemonImports.hpp>
#include <vtextension/ExtensionComposition.hpp>
#include <vtextension/ExtensionGrantStore.hpp>
#include <vtextension/Manifest.hpp>
#include <vtextension/RootPointer.hpp>
#include <vtextension/testing/DaemonRunnerHarness.hpp>
#include <vtextension/testing/FakeBeneathReader.hpp>
#include <vtextension/testing/FakeDirectoryOpener.hpp>
#include <vtextension/testing/FakeFileLocker.hpp>
#include <vtextension/testing/FakeProcessRunner.hpp>
#include <vtextension/testing/TestModules.hpp>

using namespace std::chrono_literals;
using namespace std::string_view_literals;
using namespace vtextension;
using core::platform::testing::InMemoryFileSystem;
using vtextension::testing::DaemonRunnerHarness;
using vtextension::testing::FakeDirectoryOpener;
using vtextension::testing::FakeFileLocker;
using vtextension::testing::FakeProcessRunner;

namespace
{

/// A daemon extension with one root, `work`, and one command bound to it.
constexpr auto WorkManifest =
    "name = probe\nversion = 2\nplacement daemon\nroot work = config:work\ncommand run short @work = true\n"sv;

/// Its configuration: the root is `/w`.
constexpr auto WorkConfig = R"({"work":"/w"})"sv;

constexpr auto GrantsFile = "/config/extension-grants.daemon-scoped.txt"sv;

/// Where a scripted guest has the host store a call's result, and puts its text arguments.
constexpr auto ResultAddress = uint32_t { 256 };
constexpr auto CommandText = uint32_t { 4096 };
constexpr auto CwdText = uint32_t { 8192 };
constexpr auto TagText = uint32_t { 12288 };

/// @return @p bytes as file content.
[[nodiscard]] std::string textOf(std::vector<std::byte> const& bytes)
{
    auto text = std::string(bytes.size(), '\0');
    std::ranges::transform(bytes, text.begin(), [](std::byte b) { return static_cast<char>(b); });
    return text;
}

/// @return Where the store of @p name is put.
[[nodiscard]] std::filesystem::path storeOf(std::string_view name)
{
    return std::filesystem::path { "/state/extensions/default" } / name / "store";
}

/// The daemon world over fakes: modules under `/ext`, its grants and stores in memory.
struct Fixture
{
    Fixture()
    {
        directories.add("/w");
        fileSystem.addFile(
            "/ext/good.wasm",
            textOf(testing::withManifest(testing::toModule(testing::DaemonProbeModule), WorkManifest)));
        fileSystem.addFile("/ext/bad.wasm", textOf(testing::daemonProbeNoPlacement()));
        // A daemon module by its manifest, that the runtime refuses to load: it has a start function.
        fileSystem.addFile(
            "/ext/broken.wasm",
            textOf(testing::withManifest(testing::toModule(testing::StartFunctionModule), WorkManifest)));
        harness.onLoad = [this]() -> std::expected<void, wasm::CallFailure> {
            auto result = onLoad ? onLoad() : std::expected<void, wasm::CallFailure> {};
            if (loads.fetch_add(1) == 0)
                firstLoad.count_down();
            return result;
        };
    }

    Fixture(Fixture const&) = delete;
    Fixture(Fixture&&) = delete;
    Fixture& operator=(Fixture const&) = delete;
    Fixture& operator=(Fixture&&) = delete;

    ~Fixture()
    {
        if (runtime)
            runtime->stop();
    }

    /// Records the user's grant of `good` at `/ext/good.wasm` over the root `/w`.
    void grantGood() const
    {
        auto const manifest =
            parseManifest(testing::withManifest(testing::toModule(testing::DaemonProbeModule), WorkManifest));
        REQUIRE(manifest.has_value());
        auto const roots = std::vector<ResolvedRoot> { { .label = "work", .paths = { "/w" } } };
        auto grants = ExtensionGrantStore(fileSystem, GrantsFile);
        grants.record(extensionHostId("good", "/ext/good.wasm"),
                      grantDigestOf(manifest->digest, roots),
                      GrantState::Granted);
    }

    /// Composes the runtime from @p declarations into `runtime`.
    /// @param limits What each extension may cost.
    /// @return The problems it reported.
    std::vector<DeclarationProblem> create(std::vector<DaemonExtensionDeclaration> declarations,
                                           ExtensionLimits limits = daemonWorldLimits())
    {
        auto composed = DaemonExtensionRuntime::create(
            DaemonExtensionRuntimeConfig { .declarations = std::move(declarations),
                                           .grantsFile = GrantsFile,
                                           .home = home,
                                           .inheritedEnvironment = { "PATH=/usr/bin" },
                                           .bundledDirectories = {},
                                           .limits = limits },
            DaemonExtensionRuntimeServices { .backend = harness.backend,
                                             .runner = runner,
                                             .directories = directories,
                                             .files = files,
                                             .fileSystem = fileSystem,
                                             .locker = locker,
                                             .wallClock = harness.wallClock,
                                             .localTime = localTime,
                                             .presence = harness.presence,
                                             .onLog = {} });
        REQUIRE(composed.runtime != nullptr);
        runtime = std::move(composed.runtime);
        return std::move(composed.problems);
    }

    /// Has the guest start `run` in `/w`, as its `load` does.
    [[nodiscard]] std::expected<void, wasm::CallFailure> startRun() const
    {
        harness.put(CommandText, "run");
        harness.put(CwdText, "/w");
        harness.put(TagText, "t");
        if (!harness.call(DaemonImport::ProcStart,
                          { CommandText, 3, 0, 0, CwdText, 2, TagText, 1, ResultAddress }))
            return std::unexpected(wasm::CallFailure::HostFailure);
        return {};
    }

    /// What the backend and the scripted guest are. First: it outlives the runtime.
    DaemonRunnerHarness harness;
    InMemoryFileSystem fileSystem;
    FakeFileLocker locker;
    FakeProcessRunner runner;
    FakeDirectoryOpener directories;
    vtextension::testing::FakeBeneathReader files;
    vtbackend::LocalTimeConverter localTime { &vtbackend::systemLocalTime };
    std::string home = "/home/me"; ///< The user's home directory, as the runtime is told it.
    std::latch firstLoad { 1 };    ///< Counted down when the first `load` returned, on the lane.
    std::atomic<int> loads = 0;    ///< How many `load` calls returned.
    std::function<std::expected<void, wasm::CallFailure>()> onLoad; ///< What `load` does, on the lane.
    std::unique_ptr<DaemonExtensionRuntime> runtime; ///< Last: destroyed before what it reaches.
};

/// @return The declaration of @p name at @p module, with WorkConfig.
[[nodiscard]] DaemonExtensionDeclaration declared(std::string name,
                                                  std::filesystem::path module,
                                                  ExtensionAutostart autostart)
{
    auto store = storeOf(name);
    return DaemonExtensionDeclaration { .name = std::move(name),
                                        .modulePath = std::move(module),
                                        .autostart = autostart,
                                        .configJson = std::string(WorkConfig),
                                        .storeDirectory = std::move(store) };
}

/// Waits, on the real lane, until @p condition holds; ten seconds at most.
/// @return Whether it held.
template <typename Condition>
[[nodiscard]] bool eventually(Condition const& condition)
{
    auto const deadline = std::chrono::steady_clock::now() + 10s;
    while (!condition())
    {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        std::this_thread::yield();
    }
    return true;
}

/// The daemon world's limits with a back-off short enough that faults follow each other at once.
[[nodiscard]] ExtensionLimits quickRecovery()
{
    auto limits = daemonWorldLimits();
    limits.initialBackoff = 1ms;
    return limits;
}

} // namespace

TEST_CASE("DaemonExtensionRuntime: bootStartOf", "[daemon][runtime]")
{
    struct Row
    {
        ExtensionAutostart autostart;
        StoreContents store;
        BootStart start;
    };
    for (auto const& row: std::array {
             Row { .autostart = ExtensionAutostart::AtDaemonStart,
                   .store = StoreContents::Empty,
                   .start = BootStart::Yes },
             Row { .autostart = ExtensionAutostart::AtDaemonStart,
                   .store = StoreContents::HasKeys,
                   .start = BootStart::Yes },
             Row { .autostart = ExtensionAutostart::OnDemand,
                   .store = StoreContents::Empty,
                   .start = BootStart::No },
             Row { .autostart = ExtensionAutostart::OnDemand,
                   .store = StoreContents::HasKeys,
                   .start = BootStart::Yes },
         })
    {
        CHECK(bootStartOf(row.autostart, row.store) == row.start);
    }
}

TEST_CASE("DaemonExtensionRuntime: create reports each declaration's problem and runs the rest",
          "[daemon][runtime]")
{
    auto fixture = Fixture {};
    fixture.grantGood();
    auto const problems = fixture.create({
        declared("bad", "/ext/bad.wasm", ExtensionAutostart::AtDaemonStart),
        declared("gone", "/ext/gone.wasm", ExtensionAutostart::AtDaemonStart),
        declared("good", "/ext/good.wasm", ExtensionAutostart::AtDaemonStart),
    });

    REQUIRE(problems.size() == 2);
    CHECK(problems[0].name == "bad");
    CHECK(problems[0].problem == DaemonExtensionProblem::NotADaemonModule);
    CHECK(problems[0].message.contains("daemon.extensions.bad"));
    CHECK(problems[0].message.contains(
        "`bad` is a status-line extension: configure it under a profile's `extensions`"));
    CHECK(problems[1].name == "gone");
    CHECK(problems[1].problem == DaemonExtensionProblem::ModuleUnreadable);
    CHECK(problems[1].message.contains("daemon.extensions.gone"));

    CHECK(fixture.runtime->status("good").has_value());
    CHECK_FALSE(fixture.runtime->status("bad").has_value());
    fixture.firstLoad.wait();
    fixture.runtime->stop();
    CHECK(fixture.harness.loadedConfigs == std::vector<std::string> { std::string(WorkConfig) });
}

TEST_CASE("DaemonExtensionRuntime: a root that cannot be resolved is the declaration's problem",
          "[daemon][runtime]")
{
    auto fixture = Fixture {};
    auto declaration = declared("good", "/ext/good.wasm", ExtensionAutostart::AtDaemonStart);
    declaration.configJson = R"({"work":"relative"})";
    auto const problems = fixture.create({ std::move(declaration) });
    REQUIRE(problems.size() == 1);
    CHECK(problems[0].problem == DaemonExtensionProblem::RootUnresolvable);
    CHECK(problems[0].message.contains("daemon.extensions.good"));
}

TEST_CASE("DaemonExtensionRuntime: a root under ~ without a home directory is the declaration's problem",
          "[daemon][runtime]")
{
    auto fixture = Fixture {};
    fixture.home.clear();
    auto declaration = declared("good", "/ext/good.wasm", ExtensionAutostart::AtDaemonStart);
    declaration.configJson = R"({"work":"~/w"})";
    auto const problems = fixture.create({ std::move(declaration) });
    REQUIRE(problems.size() == 1);
    CHECK(problems[0].problem == DaemonExtensionProblem::RootUnresolvable);
    CHECK(problems[0].message.contains("home directory is unknown"));
}

TEST_CASE("DaemonExtensionRuntime: an ungranted extension is not instantiated", "[daemon][runtime]")
{
    auto fixture = Fixture {};
    auto const problems =
        fixture.create({ declared("good", "/ext/good.wasm", ExtensionAutostart::AtDaemonStart) });
    CHECK(problems.empty());
    CHECK_FALSE(fixture.runtime->status("good").has_value());
    // It holds no engine: none was ever created, so no thread can be prepared for one.
    CHECK_FALSE(fixture.harness.backend.isCurrentThreadPrepared());

    // Not on demand either, until the user answers.
    fixture.runtime->start("good");
    CHECK_FALSE(fixture.runtime->status("good").has_value());
    fixture.runtime->stop();
    CHECK(fixture.harness.backend.lastInstance == nullptr);
    CHECK(fixture.harness.loadedConfigs.empty());
}

TEST_CASE("DaemonExtensionRuntime: an on-demand extension starts when asked", "[daemon][runtime]")
{
    auto fixture = Fixture {};
    fixture.grantGood();
    auto const problems =
        fixture.create({ declared("good", "/ext/good.wasm", ExtensionAutostart::OnDemand) });
    CHECK(problems.empty());
    CHECK_FALSE(fixture.runtime->status("good").has_value());

    fixture.runtime->start("good");
    CHECK(fixture.runtime->status("good").has_value());
    // Its module was loaded here, on a thread prepared for the runtime.
    CHECK(fixture.harness.backend.enteredOn(std::this_thread::get_id()));
    fixture.firstLoad.wait();
    fixture.runtime->stop();
    CHECK(fixture.harness.loadedConfigs.size() == 1);

    // Once stopped, nothing starts any more.
    fixture.runtime->start("good");
    CHECK_FALSE(fixture.runtime->status("good").has_value());
}

TEST_CASE("DaemonExtensionRuntime: stop cancels the processes in flight", "[daemon][runtime]")
{
    auto fixture = Fixture {};
    fixture.grantGood();
    fixture.onLoad = [&] {
        return fixture.startRun();
    };
    auto const problems =
        fixture.create({ declared("good", "/ext/good.wasm", ExtensionAutostart::AtDaemonStart) });
    REQUIRE(problems.empty());
    fixture.firstLoad.wait();

    fixture.runtime->stop();
    REQUIRE(fixture.runner.started().size() == 1);
    CHECK(fixture.runner.cancelled() == std::vector { fixture.runner.started()[0].token });
}

TEST_CASE("DaemonExtensionRuntime: the store lives in the directory it was given", "[daemon][runtime]")
{
    auto fixture = Fixture {};
    fixture.grantGood();

    SECTION("it is created there, and held")
    {
        auto const problems =
            fixture.create({ declared("good", "/ext/good.wasm", ExtensionAutostart::OnDemand) });
        CHECK(problems.empty());
        CHECK(fixture.fileSystem.isDirectory(storeOf("good")));
        CHECK(std::ranges::any_of(fixture.locker.held(), [](std::filesystem::path const& lock) {
            return lock.parent_path() == storeOf("good");
        }));
        // An empty store starts nothing on demand.
        CHECK_FALSE(fixture.runtime->status("good").has_value());
    }

    SECTION("a store that holds keys starts an on-demand extension with the daemon")
    {
        fixture.fileSystem.addFile(storeOf("good") / "runs", "1");
        auto const problems =
            fixture.create({ declared("good", "/ext/good.wasm", ExtensionAutostart::OnDemand) });
        CHECK(problems.empty());
        CHECK(fixture.runtime->status("good").has_value());
    }

    SECTION("a store no directory can be named for is the declaration's problem")
    {
        auto declaration = declared("good", "/ext/good.wasm", ExtensionAutostart::OnDemand);
        declaration.storeDirectory.clear();
        auto const problems = fixture.create({ std::move(declaration) });
        REQUIRE(problems.size() == 1);
        CHECK(problems[0].problem == DaemonExtensionProblem::StoreUnavailable);
    }
}

namespace
{
/// @return What the grants file says of `good` at `/ext/good.wasm` for @p grantDigest.
[[nodiscard]] GrantState recordedFor(Fixture const& fixture, std::string_view grantDigest)
{
    return ExtensionGrantStore(fixture.fileSystem, GrantsFile)
        .lookup(extensionHostId("good", "/ext/good.wasm"), grantDigest);
}
} // namespace

TEST_CASE("DaemonExtensionRuntime: answer starts a waiting extension", "[daemon][runtime][consent]")
{
    auto fixture = Fixture {};
    REQUIRE(
        fixture.create({ declared("good", "/ext/good.wasm", ExtensionAutostart::AtDaemonStart) }).empty());
    auto const pending = fixture.runtime->pending();
    REQUIRE(pending.size() == 1);
    CHECK(pending[0].name == "good");
    CHECK(pending[0].question.title == "“probe” wants to run in the background");
    CHECK(pending[0].question.lines.at(1).text == "Directory (work): /w");
    auto const digest = pending[0].grantDigest;

    // An answer to another question, or a dismissal, records nothing.
    fixture.runtime->answer("good", "another digest", GrantState::Granted);
    fixture.runtime->answer("other", digest, GrantState::Granted);
    fixture.runtime->answer("good", digest, GrantState::Pending);
    CHECK_FALSE(fixture.runtime->status("good").has_value());
    CHECK(fixture.runtime->pending().size() == 1);

    fixture.runtime->answer("good", digest, GrantState::Granted);
    CHECK(fixture.runtime->status("good").has_value());
    CHECK(fixture.runtime->pending().empty());
    CHECK(recordedFor(fixture, digest) == GrantState::Granted);
    fixture.firstLoad.wait();
    fixture.runtime->stop();
    CHECK(fixture.harness.loadedConfigs.size() == 1);
}

TEST_CASE("DaemonExtensionRuntime: an extension not asked to start waits for nothing",
          "[daemon][runtime][consent]")
{
    auto fixture = Fixture {};
    REQUIRE(fixture.create({ declared("good", "/ext/good.wasm", ExtensionAutostart::OnDemand) }).empty());
    CHECK(fixture.runtime->pending().empty());
    fixture.runtime->start("good");
    CHECK(fixture.runtime->pending().size() == 1);
}

TEST_CASE("DaemonExtensionRuntime: a refusal is remembered for that digest and roots",
          "[daemon][runtime][consent]")
{
    auto fixture = Fixture {};
    REQUIRE(
        fixture.create({ declared("good", "/ext/good.wasm", ExtensionAutostart::AtDaemonStart) }).empty());
    auto const digest = fixture.runtime->pending().at(0).grantDigest;
    fixture.runtime->answer("good", digest, GrantState::Denied);
    CHECK(fixture.runtime->pending().empty());
    CHECK_FALSE(fixture.runtime->status("good").has_value());
    CHECK(recordedFor(fixture, digest) == GrantState::Denied);
    // Answered once: a second answer to the same question is no longer taken.
    fixture.runtime->answer("good", digest, GrantState::Granted);
    CHECK(recordedFor(fixture, digest) == GrantState::Denied);

    SECTION("the next daemon start does not ask again")
    {
        fixture.runtime.reset();
        REQUIRE(fixture.create({ declared("good", "/ext/good.wasm", ExtensionAutostart::AtDaemonStart) })
                    .empty());
        fixture.runtime->start("good");
        CHECK(fixture.runtime->pending().empty());
        CHECK_FALSE(fixture.runtime->status("good").has_value());
    }

    SECTION("a moved root asks again")
    {
        fixture.runtime.reset();
        auto declaration = declared("good", "/ext/good.wasm", ExtensionAutostart::AtDaemonStart);
        declaration.configJson = R"({"work":"/elsewhere"})";
        auto const problems = fixture.create({ std::move(declaration) });
        REQUIRE(problems.empty());
        auto const pending = fixture.runtime->pending();
        REQUIRE(pending.size() == 1);
        CHECK(pending[0].grantDigest != digest);
        CHECK(pending[0].question.lines.at(1).text == "Directory (work): /elsewhere");
    }
}

TEST_CASE("DaemonExtensionRuntime: revoke stops it and asks again", "[daemon][runtime][consent]")
{
    auto fixture = Fixture {};
    fixture.grantGood();
    REQUIRE(
        fixture.create({ declared("good", "/ext/good.wasm", ExtensionAutostart::AtDaemonStart) }).empty());
    REQUIRE(fixture.runtime->status("good").has_value());
    CHECK(fixture.runtime->pending().empty());
    fixture.firstLoad.wait();

    fixture.runtime->revoke("good");
    CHECK_FALSE(fixture.runtime->status("good").has_value());
    auto const pending = fixture.runtime->pending();
    REQUIRE(pending.size() == 1);
    CHECK(recordedFor(fixture, pending[0].grantDigest) == GrantState::Pending);

    // Granted again, it runs again, on the module it already loaded.
    fixture.runtime->answer("good", pending[0].grantDigest, GrantState::Granted);
    CHECK(fixture.runtime->status("good").has_value());
    CHECK(fixture.runtime->pending().empty());

    // A name that does not run is no one's to revoke.
    fixture.runtime->revoke("other");
    CHECK(fixture.runtime->status("good").has_value());
}

TEST_CASE("DaemonExtensionRuntime: entries say where each extension stands", "[daemon][runtime][consent]")
{
    auto fixture = Fixture {};
    REQUIRE(fixture
                .create({ declared("good", "/ext/good.wasm", ExtensionAutostart::OnDemand),
                          declared("bad", "/ext/bad.wasm", ExtensionAutostart::OnDemand) })
                .size()
            == 1);
    auto const phaseOf = [&] {
        auto const entries = fixture.runtime->entries();
        REQUIRE(entries.size() == 1);
        CHECK(entries[0].name == "good");
        return entries[0].phase;
    };
    CHECK(phaseOf() == DaemonExtensionPhase::Stopped);
    fixture.runtime->start("good");
    CHECK(phaseOf() == DaemonExtensionPhase::Pending);
    auto const digest = fixture.runtime->pending().at(0).grantDigest;
    CHECK(fixture.runtime->entries().at(0).grantDigest == digest);
    fixture.runtime->answer("good", digest, GrantState::Denied);
    CHECK(phaseOf() == DaemonExtensionPhase::Denied);
    // Not running: revoking forgets the refusal; it is asked once it is next started.
    fixture.runtime->revoke("good");
    CHECK(phaseOf() == DaemonExtensionPhase::Stopped);
    fixture.runtime->start("good");
    CHECK(phaseOf() == DaemonExtensionPhase::Pending);
    fixture.runtime->answer("good", digest, GrantState::Granted);
    CHECK(phaseOf() == DaemonExtensionPhase::Running);
    fixture.firstLoad.wait();
    fixture.runtime->stop();
    CHECK(fixture.runtime->entries().empty());
}

TEST_CASE("DaemonExtensionRuntime: a granted extension whose module does not load is Failed, and says why",
          "[daemon][runtime][consent]")
{
    auto fixture = Fixture {};
    REQUIRE(fixture.create({ declared("broken", "/ext/broken.wasm", ExtensionAutostart::OnDemand) }).empty());
    fixture.runtime->start("broken");
    REQUIRE(fixture.runtime->pending().size() == 1);
    auto const digest = fixture.runtime->pending().at(0).grantDigest;

    // Granted in a window, it cannot run: not "waiting for consent", but failed, with the reason.
    fixture.runtime->answer("broken", digest, GrantState::Granted);
    CHECK_FALSE(fixture.runtime->status("broken").has_value());
    CHECK(fixture.runtime->pending().empty());
    auto const failed = fixture.runtime->entries().at(0);
    CHECK(failed.phase == DaemonExtensionPhase::Failed);
    CHECK(failed.problem.contains("daemon.extensions.broken"));
    CHECK(failed.problem.contains("its module does not load"));

    // Started again, it fails again; revoked, the failure goes with the grant.
    fixture.runtime->start("broken");
    CHECK(fixture.runtime->entries().at(0).phase == DaemonExtensionPhase::Failed);
    fixture.runtime->revoke("broken");
    CHECK(fixture.runtime->entries().at(0).phase == DaemonExtensionPhase::Stopped);
    CHECK(fixture.runtime->entries().at(0).problem.empty());
}

TEST_CASE("DaemonExtensionRuntime: revoke starts a new run, which its presence is told",
          "[daemon][runtime][consent]")
{
    auto fixture = Fixture {};
    fixture.grantGood();
    REQUIRE(fixture.create({ declared("good", "/ext/good.wasm", ExtensionAutostart::OnDemand) }).empty());
    fixture.runtime->start("good");
    fixture.firstLoad.wait();
    fixture.runtime->revoke("good");
    // Run 0 was up; the revoke made run 1, which waits for the user's grant. What run 0's lane says from now
    // on is older than that, and is dropped by the presence.
    using Told = std::pair<DaemonRunGeneration, DaemonExtensionPhase>;
    auto const phases = fixture.harness.presence.phases();
    REQUIRE_FALSE(phases.empty());
    CHECK(phases.front() == Told { 0, DaemonExtensionPhase::Running });
    CHECK(phases.back() == Told { 1, DaemonExtensionPhase::Pending });
    fixture.runtime->stop();
}

TEST_CASE("DaemonExtensionRuntime: a boot-time extension that cannot load is Failed", "[daemon][runtime]")
{
    auto fixture = Fixture {};
    // Granted by a former run: the grant is recorded for the module's digest, which the question names.
    REQUIRE(fixture.create({ declared("broken", "/ext/broken.wasm", ExtensionAutostart::OnDemand) }).empty());
    fixture.runtime->start("broken");
    auto const digest = fixture.runtime->pending().at(0).grantDigest;
    fixture.runtime->answer("broken", digest, GrantState::Granted);
    fixture.runtime.reset(); // Its store's lock goes with it.

    auto const problems =
        fixture.create({ declared("broken", "/ext/broken.wasm", ExtensionAutostart::AtDaemonStart) });
    REQUIRE(problems.size() == 1);
    CHECK(problems[0].problem == DaemonExtensionProblem::ModuleUnreadable);
    auto const entry = fixture.runtime->entries().at(0);
    CHECK(entry.phase == DaemonExtensionPhase::Failed);
    CHECK(entry.problem == problems[0].message);
}

TEST_CASE("DaemonExtensionRuntime: revoking one that does not run only forgets its grant",
          "[daemon][runtime][consent]")
{
    auto fixture = Fixture {};
    fixture.grantGood();
    REQUIRE(fixture.create({ declared("good", "/ext/good.wasm", ExtensionAutostart::OnDemand) }).empty());
    REQUIRE_FALSE(fixture.runtime->status("good").has_value());

    fixture.runtime->revoke("good");
    // No question pops up whose Allow would start what nobody asked for.
    CHECK(fixture.runtime->pending().empty());
    CHECK(fixture.runtime->entries().at(0).phase == DaemonExtensionPhase::Stopped);
    // Started on demand, it is asked again.
    fixture.runtime->start("good");
    CHECK(fixture.runtime->pending().size() == 1);
}

TEST_CASE("DaemonExtensionRuntime: revoke cancels the processes in flight; the next grant sees none of them",
          "[daemon][runtime][consent]")
{
    auto fixture = Fixture {};
    fixture.grantGood();
    auto secondLoad = std::latch { 1 };
    auto listed = std::optional<std::expected<std::vector<ProcInfo>, HostError>> {};
    fixture.onLoad = [&]() -> std::expected<void, wasm::CallFailure> {
        if (fixture.loads.load() == 0)
            return fixture.startRun();
        auto const called = fixture.harness.call(DaemonImport::Procs, { ResultAddress });
        if (called)
            if (auto read =
                    fixture.harness.read<std::expected<std::vector<ProcInfo>, HostError>>(ResultAddress))
                listed = std::move(*read);
        secondLoad.count_down();
        return {};
    };
    REQUIRE(
        fixture.create({ declared("good", "/ext/good.wasm", ExtensionAutostart::AtDaemonStart) }).empty());
    fixture.firstLoad.wait();

    fixture.runtime->revoke("good");
    auto const pending = fixture.runtime->pending();
    REQUIRE(pending.size() == 1);
    fixture.runtime->answer("good", pending[0].grantDigest, GrantState::Granted);
    secondLoad.wait();
    fixture.runtime->stop();

    REQUIRE(fixture.runner.started().size() == 1);
    CHECK(fixture.runner.cancelled() == std::vector { fixture.runner.started()[0].token });
    REQUIRE(listed.has_value());
    REQUIRE(listed->has_value());
    CHECK((*listed)->empty());
}

TEST_CASE("DaemonExtensionRuntime: the same manifest over other code is asked about anew",
          "[daemon][runtime][consent]")
{
    auto fixture = Fixture {};
    fixture.grantGood();
    // The module on disk changed after the grant: same manifest, one more custom section.
    auto changed = testing::withManifest(testing::toModule(testing::DaemonProbeModule), WorkManifest);
    for (auto const byte: { 0x00, 0x03, 0x01, 0x78, 0x2A })
        changed.push_back(static_cast<std::byte>(byte));
    fixture.fileSystem.addFile("/ext/good.wasm", textOf(changed));

    REQUIRE(
        fixture.create({ declared("good", "/ext/good.wasm", ExtensionAutostart::AtDaemonStart) }).empty());
    CHECK_FALSE(fixture.runtime->status("good").has_value());
    CHECK(fixture.runtime->pending().size() == 1);
}

TEST_CASE("DaemonExtensionRuntime: restart starts a disabled extension again, with a fresh fault window",
          "[daemon][runtime][presence]")
{
    auto fixture = Fixture {};
    fixture.grantGood();
    // Loads 0-2 fault (three in the window: disabled); after the restart, loads 3 and 4 fault and 5 runs.
    fixture.onLoad = [&]() -> std::expected<void, wasm::CallFailure> {
        if (fixture.loads.load() == 5)
            return {};
        return std::unexpected(wasm::CallFailure::Trap);
    };
    auto const problems = fixture.create(
        { declared("good", "/ext/good.wasm", ExtensionAutostart::AtDaemonStart) }, quickRecovery());
    REQUIRE(problems.empty());
    auto const stateOf = [&] {
        auto const status = fixture.runtime->status("good");
        return status ? status->state : DaemonRunnerState::Starting;
    };
    REQUIRE(eventually([&] { return stateOf() == DaemonRunnerState::Disabled; }));
    CHECK(fixture.loads.load() == 3);

    fixture.runtime->restart("good");
    REQUIRE(eventually([&] { return fixture.loads.load() == 6 && stateOf() == DaemonRunnerState::Running; }));
    auto const status = fixture.runtime->status("good");
    REQUIRE(status.has_value());
    CHECK(status->faultsInWindow == 2);
    CHECK(fixture.runtime->entries().front().phase == DaemonExtensionPhase::Running);
}

TEST_CASE("DaemonExtensionRuntime: restart does nothing unless disabled", "[daemon][runtime][presence]")
{
    auto fixture = Fixture {};
    // Not granted: it waits for its user, and a restart does not run it.
    auto const problems =
        fixture.create({ declared("good", "/ext/good.wasm", ExtensionAutostart::AtDaemonStart) });
    REQUIRE(problems.empty());
    fixture.runtime->restart("good");
    CHECK_FALSE(fixture.runtime->status("good").has_value());
    CHECK(fixture.runtime->pending().size() == 1);

    // Granted and running: a restart loads it no second time.
    auto const pending = fixture.runtime->pending();
    fixture.runtime->answer("good", pending.front().grantDigest, GrantState::Granted);
    fixture.firstLoad.wait();
    fixture.runtime->restart("good");
    fixture.runtime->restart("unknown");
    fixture.runtime->hint("unknown", DaemonHint::Actions);
    fixture.runtime->stop();
    CHECK(fixture.loads.load() == 1);
    // Once stopped, neither does anything.
    fixture.runtime->restart("good");
    fixture.runtime->hint("good", DaemonHint::Actions);
    CHECK(fixture.loads.load() == 1);
}
