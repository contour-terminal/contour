// SPDX-License-Identifier: Apache-2.0
#include <core/async/testing/ManualExecutor.hpp>
#include <core/platform/testing/InMemoryFileSystem.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <thread>
#include <utility>

#include <vtextension/ExtensionHost.hpp>
#include <vtextension/SessionExtensions.hpp>
#include <vtextension/testing/QueuedOwner.hpp>
#include <vtextension/testing/RunnerHarness.hpp>
#include <vtextension/testing/TestModules.hpp>

using vtextension::DeclaredExtensions;
using vtextension::ExtensionHostConfig;
using vtextension::ExtensionRuntime;
using vtextension::ExtensionRuntimeConfig;
using vtextension::ExtensionRuntimeServices;
using vtextension::ExtensionSpec;
using vtextension::HostedExtensions;
using vtextension::Placement;
using vtextension::ReplaceableStatusLineExtensions;
using vtextension::SessionExtensionCallbacks;
using vtextension::SessionExtensions;
using vtextension::SessionExtensionServices;
using vtextension::testing::QueuedOwner;
using vtextension::testing::RunnerHarness;

namespace
{
[[nodiscard]] std::filesystem::path modulePath()
{
    return { "/ext/minimal.wasm" };
}

[[nodiscard]] std::string minimalModule()
{
    auto text = std::string {};
    for (auto const byte: vtextension::testing::MinimalModule)
        text.push_back(static_cast<char>(byte));
    return text;
}

/// The services a pane's extensions reach: the harness's manual scheduler, a manual build executor, and an
/// in-memory filesystem the test fills.
struct Fixture
{
    RunnerHarness harness;
    vtextension::ExtensionLimits limits {}; ///< What the engines are created with, and each extension costs.
    vtextension::EngineProvider engines { *harness.backend, limits };
    core::async::testing::ManualExecutor builds;
    core::platform::testing::InMemoryFileSystem fileSystem;
    std::shared_ptr<QueuedOwner> owner = std::make_shared<QueuedOwner>();
    int replaced = 0;

    [[nodiscard]] SessionExtensionServices services()
    {
        return { .engines = engines,
                 .retirement = engines,
                 .scheduler = harness.scheduler,
                 .builds = builds,
                 .grants = harness.grants,
                 .capture = harness.capture,
                 .wallClock = harness.wallClock,
                 .localTime = harness.localTime,
                 .environment = harness.environment,
                 .fileSystem = fileSystem,
                 .lookup = { .fileSystem = fileSystem, .bundledDirectories = {} },
                 .limits = limits };
    }

    [[nodiscard]] SessionExtensionCallbacks callbacks()
    {
        return { .onPermissionNeeded = {}, .onLog = {}, .onReplaced = [this] { ++replaced; } };
    }

    /// Runs the builds, then hands their results back to the owner.
    void build()
    {
        builds.drain();
        owner->run();
    }

    /// Runs the runners' last jobs, which they post as they are destroyed, and releases what they retired.
    void settle()
    {
        harness.scheduler.runPending();
        engines.releaseRetired();
    }
};

[[nodiscard]] DeclaredExtensions declaring(std::string name, std::filesystem::path path)
{
    return DeclaredExtensions { .extensions = { vtextension::DeclaredExtension {
                                    .name = std::move(name),
                                    .path = std::move(path),
                                    .config = {},
                                    .runCommand = vtextension::CommandPermission::Deny } },
                                .takeOver = std::nullopt };
}

/// @return A pane's extensions, ready to start: their modules missing, so nothing runs.
[[nodiscard]] std::shared_ptr<HostedExtensions> hostedMissing(Fixture& fixture,
                                                              std::string name,
                                                              Placement placement)
{
    auto config = ExtensionHostConfig {};
    config.extensions.push_back(ExtensionSpec { .name = std::move(name),
                                                .hostId = "test@/gone.wasm",
                                                .module = {},
                                                .config = {},
                                                .placement = placement,
                                                .runCommand = vtextension::CommandPermission::Deny });
    return std::make_shared<HostedExtensions>(
        vtextension::prepareExtensions(std::move(config),
                                       std::unexpected(vtextension::ExtensionError::ModuleUnavailable)),
        vtextension::ExtensionHostServices {
            .engines = fixture.engines,
            .runners = fixture.harness.services(fixture.harness.scheduler, fixture.engines) });
}
} // namespace

TEST_CASE("ReplaceableStatusLineExtensions: forwards to what it holds", "[vtextension][extensions]")
{
    auto fixture = Fixture {};
    auto forwarder = ReplaceableStatusLineExtensions { nullptr };

    // Nothing held: nothing to show, nothing subscribed, and calls are no-ops.
    CHECK(forwarder.empty());
    CHECK(forwarder.subscriptions().none());
    CHECK(forwarder.snapshot("git") == nullptr);
    CHECK_FALSE(forwarder.takeOverName().has_value());
    forwarder.post(vtbackend::StatusLineEvents::CwdChanged { .cwd = "/" });
    forwarder.resize(vtbackend::PageSize { vtbackend::LineCount(1), vtbackend::ColumnCount(80) });

    auto hosted = hostedMissing(fixture, "git", Placement::TakeOver);
    CHECK(forwarder.replace(hosted) == nullptr);
    CHECK(forwarder.takeOverName() == "git");
    REQUIRE(forwarder.snapshot("git") != nullptr);
    CHECK(forwarder.snapshot("git")->sgrText == "⚠ git disabled: extension module not found");

    // Handed back with no other reference left, for the caller to destroy.
    hosted.reset();
    auto previous = forwarder.replace(nullptr);
    REQUIRE(previous != nullptr);
    CHECK(previous.use_count() == 1);
    CHECK(forwarder.snapshot("git") == nullptr);
}

TEST_CASE("ReplaceableStatusLineExtensions: replaced while read", "[vtextension][extensions][threading]")
{
    // A reader on another thread, as the parser and the workers are: replace() hands back each host only once
    // no reader holds it, so the last reference never dies on the reader.
    auto fixture = Fixture {};
    auto forwarder = ReplaceableStatusLineExtensions { hostedMissing(fixture, "git", Placement::Segment) };
    auto stop = std::atomic<bool> { false };
    auto reads = std::atomic<std::size_t> { 0 };
    auto reader = std::thread { [&] {
        while (!stop.load())
        {
            (void) forwarder.snapshot("git");
            (void) forwarder.subscriptions();
            reads.fetch_add(1);
        }
    } };
    for ([[maybe_unused]] auto const round: std::views::iota(0, 200))
    {
        auto previous = forwarder.replace(hostedMissing(fixture, "git", Placement::Segment));
        CHECK(previous.use_count() == 1);
    }
    stop.store(true);
    reader.join();
    CHECK(reads.load() > 0);
}

TEST_CASE("SessionExtensions: built off the owner's thread, then swapped in", "[vtextension][extensions]")
{
    auto fixture = Fixture {};
    {
        auto extensions = SessionExtensions { fixture.callbacks(), fixture.owner };
        auto const services = extensions.terminalServices();
        REQUIRE(extensions.empty());

        // The call returns before anything is found or read: the module is not even there yet.
        extensions.rebuild(declaring("clock", modulePath()), {}, fixture.services());
        CHECK(extensions.empty());
        CHECK(fixture.builds.pending() == 1);
        REQUIRE(fixture.fileSystem.writeFile(modulePath(), minimalModule()).has_value());

        fixture.build();
        CHECK_FALSE(extensions.empty());
        CHECK(fixture.replaced == 1);
        CHECK(fixture.harness.scheduler.lanesAssigned == 1);
        CHECK_FALSE(services.extensions.takeOverName().has_value());

        SECTION("a rebuild superseded by another is dropped")
        {
            auto takingOver = declaring("plasma", "/ext/gone.wasm");
            takingOver.takeOver = "plasma";
            extensions.rebuild(declaring("clock", modulePath()), {}, fixture.services());
            extensions.rebuild(std::move(takingOver), {}, fixture.services());
            fixture.build();
            CHECK(fixture.replaced == 2);
            CHECK(services.extensions.takeOverName() == "plasma");
            REQUIRE(services.extensions.snapshot("plasma") != nullptr);
            CHECK(services.extensions.snapshot("plasma")->sgrText
                  == "⚠ plasma disabled: extension module not found");
        }

        SECTION("to none: at once, and without services")
        {
            extensions.rebuild(DeclaredExtensions {}, {}, std::nullopt);
            CHECK(extensions.empty());
            CHECK(fixture.replaced == 2);
            CHECK(fixture.builds.pending() == 0);
        }
    }
    fixture.settle();
}

TEST_CASE("SessionExtensions: a module larger than the limit is not read", "[vtextension][extensions]")
{
    auto fixture = Fixture {};
    fixture.limits.maxModuleBytes = 8;
    REQUIRE(fixture.fileSystem.writeFile(modulePath(), minimalModule()).has_value());
    {
        auto extensions = SessionExtensions { fixture.callbacks(), fixture.owner };
        extensions.rebuild(declaring("clock", modulePath()), {}, fixture.services());
        fixture.build();
        auto const snapshot = extensions.terminalServices().extensions.snapshot("clock");
        REQUIRE(snapshot != nullptr);
        CHECK(snapshot->sgrText == "⚠ clock disabled: extension module too large");
        CHECK(fixture.harness.scheduler.lanesAssigned == 0);
    }
    fixture.settle();
}

TEST_CASE("SessionExtensions: a build whose pane went away is released by the owner",
          "[vtextension][extensions]")
{
    // What the build loaded (and the engine it holds) must not be released on the build thread: handed back
    // to nobody, it is retired, and the owner's thread releases it.
    auto fixture = Fixture {};
    REQUIRE(fixture.fileSystem.writeFile(modulePath(), minimalModule()).has_value());
    auto engine = std::weak_ptr<vtextension::Engine> {};
    {
        auto extensions = SessionExtensions { fixture.callbacks(), fixture.owner };
        extensions.rebuild(declaring("clock", modulePath()), {}, fixture.services());
        engine = fixture.engines.acquire().value_or(nullptr);
    }
    fixture.builds.drain();
    REQUIRE(fixture.owner->pending() == 1);
    fixture.owner->drop();
    CHECK_FALSE(engine.expired()); // retired, not released where it was dropped
    fixture.engines.releaseRetired();
    CHECK(engine.expired());
    CHECK(fixture.replaced == 0);
}

TEST_CASE("ExtensionRuntime: nothing runs until an extension does", "[vtextension][extensions]")
{
    auto harness = RunnerHarness {};
    auto fileSystem = core::platform::testing::InMemoryFileSystem {};
    REQUIRE(fileSystem.writeFile(modulePath(), minimalModule()).has_value());
    auto runtime = ExtensionRuntime { ExtensionRuntimeConfig { .grantsFile = "/config/extension-grants.txt",
                                                               .bundledDirectories = {},
                                                               .workerCount = 2,
                                                               .limits = {} },
                                      ExtensionRuntimeServices { .backend = *harness.backend,
                                                                 .capture = harness.capture,
                                                                 .wallClock = harness.wallClock,
                                                                 .localTime = harness.localTime,
                                                                 .environment = harness.environment,
                                                                 .fileSystem = fileSystem } };
    CHECK(runtime.runningWorkers() == 0);

    // Built on the runtime's own thread, and handed back here.
    auto owner = std::make_shared<QueuedOwner>();
    {
        auto extensions = SessionExtensions { SessionExtensionCallbacks {}, owner };
        extensions.rebuild(declaring("clock", modulePath()), {}, runtime.sessionServices());
        for ([[maybe_unused]] auto const attempt: std::views::iota(0, 500))
        {
            if (owner->pending() != 0)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds { 10 });
        }
        owner->run();
        CHECK_FALSE(extensions.empty());
        CHECK(runtime.runningWorkers() == 2);
    }
    runtime.releaseRetired();
}
