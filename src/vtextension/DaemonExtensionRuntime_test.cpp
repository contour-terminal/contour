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
    /// @return The problems it reported.
    std::vector<DeclarationProblem> create(std::vector<DaemonExtensionDeclaration> declarations)
    {
        auto composed = DaemonExtensionRuntime::create(
            DaemonExtensionRuntimeConfig { .declarations = std::move(declarations),
                                           .grantsFile = GrantsFile,
                                           .home = home,
                                           .inheritedEnvironment = { "PATH=/usr/bin" },
                                           .bundledDirectories = {} },
            DaemonExtensionRuntimeServices { .backend = harness.backend,
                                             .runner = runner,
                                             .directories = directories,
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
