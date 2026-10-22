// SPDX-License-Identifier: Apache-2.0
//
// The extensions of one pane: lazy engine, one runner per extension, snapshots by name, notices for
// extensions that cannot be loaded.
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <expected>
#include <memory>
#include <thread>
#include <vector>

#include <vtextension/ExtensionHost.hpp>
#include <vtextension/ExtensionWorkerPool.hpp>
#include <vtextension/testing/RunnerHarness.hpp>
#include <vtextension/testing/TestModules.hpp>

using namespace vtextension;
using vtextension::testing::CountingEngineProvider;
using vtextension::testing::RunnerHarness;
using vtextension::testing::toModule;

namespace
{

[[nodiscard]] ExtensionSpec minimal(std::string name)
{
    return ExtensionSpec { .name = std::move(name),
                           .hostId = "minimal@test",
                           .module = toModule(vtextension::testing::MinimalModule),
                           .config = {},
                           .placement = Placement::Segment,
                           .runCommand = CommandPermission::Deny };
}

[[nodiscard]] std::unique_ptr<RetiredExtension> retiredEngine(std::shared_ptr<Engine> engine)
{
    auto retired = std::make_unique<RetiredExtension>();
    retired->engine = std::move(engine);
    return retired;
}

[[nodiscard]] vtbackend::PageSize statusLine(int columns)
{
    return vtbackend::PageSize { .lines = vtbackend::LineCount(1),
                                 .columns = vtbackend::ColumnCount(columns) };
}

} // namespace

TEST_CASE("ExtensionHost.lazyEngine", "[vtextension]")
{
    // No extensions, no cost: no engine, no lane (so no worker thread), nothing to subscribe to.
    auto harness = RunnerHarness {};
    auto engines = CountingEngineProvider {};
    auto host =
        ExtensionHost(ExtensionHostConfig {},
                      ExtensionHostServices { .engines = engines,
                                              .runners = harness.services(harness.scheduler, engines) });
    host.post(CwdChanged { .cwd = "/" });
    host.resize(statusLine(80));
    harness.scheduler.runPending();

    CHECK(engines.acquired == 0);
    CHECK(harness.scheduler.lanesAssigned == 0);
    CHECK(host.subscriptions().none());
    CHECK(host.snapshot("anything") == nullptr);

    // On the production pool, that means no worker thread either.
    auto pool = ExtensionWorkerPool(1);
    {
        auto const idle = ExtensionHost(
            ExtensionHostConfig {},
            ExtensionHostServices { .engines = engines, .runners = harness.services(pool, engines) });
        CHECK(idle.subscriptions().none());
    }
    CHECK(pool.runningWorkers() == 0);
    CHECK(engines.acquired == 0);
}

TEST_CASE("ExtensionHost.runners", "[vtextension]")
{
    auto harness = RunnerHarness {};
    auto engines = CountingEngineProvider {};
    auto config = ExtensionHostConfig {};
    config.extensions.push_back(minimal("one"));
    config.extensions.push_back(minimal("two"));
    auto host =
        ExtensionHost(std::move(config),
                      ExtensionHostServices { .engines = engines,
                                              .runners = harness.services(harness.scheduler, engines) });
    CHECK(engines.acquired == 1); // one engine for all of them
    CHECK(harness.scheduler.lanesAssigned == 2);

    host.resize(statusLine(80));
    harness.scheduler.runPending();
    CHECK(host.subscriptions() == AlwaysDelivered);
    REQUIRE(host.snapshot("one") != nullptr);
    REQUIRE(host.snapshot("two") != nullptr);
    CHECK(host.snapshot("one") != host.snapshot("two"));
    CHECK(host.snapshot("three") == nullptr);
}

TEST_CASE("ExtensionHost.unloadable", "[vtextension]")
{
    // A module that does not load shows why, in its place; it gets no runner and no lane.
    auto harness = RunnerHarness {};
    auto engines = CountingEngineProvider {};
    auto config = ExtensionHostConfig {};
    auto broken = minimal("broken");
    broken.module = { std::byte { 0 }, std::byte { 1 } };
    config.extensions.push_back(std::move(broken));
    auto host =
        ExtensionHost(std::move(config),
                      ExtensionHostServices { .engines = engines,
                                              .runners = harness.services(harness.scheduler, engines) });

    CHECK(harness.scheduler.lanesAssigned == 0);
    REQUIRE(host.snapshot("broken") != nullptr);
    CHECK(host.snapshot("broken")->sgrText == "\u26A0 broken disabled: invalid WebAssembly module");
    CHECK(host.subscriptions().none());
}

TEST_CASE("ExtensionHost.moduleUnavailable", "[vtextension]")
{
    // No bytes at all: the host found no module to load. It says so, and starts no runtime for it.
    auto harness = RunnerHarness {};
    auto engines = CountingEngineProvider {};
    auto config = ExtensionHostConfig {};
    auto missing = minimal("git");
    missing.module.clear();
    config.extensions.push_back(std::move(missing));
    auto host =
        ExtensionHost(std::move(config),
                      ExtensionHostServices { .engines = engines,
                                              .runners = harness.services(harness.scheduler, engines) });

    CHECK(engines.acquired == 0);
    CHECK(harness.scheduler.lanesAssigned == 0);
    REQUIRE(host.snapshot("git") != nullptr);
    CHECK(host.snapshot("git")->sgrText == "\u26A0 git disabled: extension module not found");
}

namespace
{
/// Loads a pane's modules on a thread of their own, as a GUI does so that a slow disk or a large module
/// does not stall it: one that loads, and one the host found too large to read.
[[nodiscard]] PreparedExtensions preparedOnAnotherThread(std::shared_ptr<Engine> const& engine)
{
    auto config = ExtensionHostConfig {};
    config.extensions.push_back(minimal("clock"));
    auto large = minimal("huge");
    large.module.clear();
    large.moduleError = ExtensionError::ModuleTooLarge;
    config.extensions.push_back(std::move(large));

    auto prepared = PreparedExtensions {};
    auto loader = std::thread { [&] { prepared = prepareExtensions(std::move(config), engine); } };
    loader.join();
    REQUIRE(prepared.extensions.size() == 2);
    CHECK(prepared.extensions[0].compiled.has_value());
    CHECK(prepared.extensions[0].spec.module.empty());
    REQUIRE_FALSE(prepared.extensions[1].compiled.has_value());
    return prepared;
}
} // namespace

TEST_CASE("ExtensionHost.preparedElsewhere", "[vtextension]")
{
    // Loaded elsewhere, started on the owner's thread.
    auto harness = RunnerHarness {};
    auto engines = CountingEngineProvider {};
    auto engine = engines.acquire();
    REQUIRE(engine.has_value());
    {
        auto host =
            ExtensionHost(preparedOnAnotherThread(*engine),
                          ExtensionHostServices { .engines = engines,
                                                  .runners = harness.services(harness.scheduler, engines) });
        CHECK(harness.scheduler.lanesAssigned == 1);
        REQUIRE(host.snapshot("huge") != nullptr);
        CHECK(host.snapshot("huge")->sgrText == "\u26A0 huge disabled: extension module too large");
    }
    harness.scheduler.runPending();
}

TEST_CASE("ExtensionHost.preparedRetired", "[vtextension]")
{
    // Loaded but never started: retired, and released by the owner.
    auto engines = CountingEngineProvider {};
    auto engine = engines.acquire();
    REQUIRE(engine.has_value());
    auto prepared = preparedOnAnotherThread(*engine);
    auto const weak = std::weak_ptr<Engine>(*engine);
    engine->reset();
    auto retired = retirementOf(std::move(prepared));
    REQUIRE(retired != nullptr);
    CHECK_FALSE(weak.expired());
    engines.retire(std::move(retired));
    engines.releaseRetired();
    CHECK(weak.expired());
}

TEST_CASE("EngineProvider.shares", "[vtextension]")
{
    auto provider = EngineProvider { ExtensionLimits {} };
    auto first = provider.acquire();
    auto second = provider.acquire();
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(first->get() == second->get());

    // Held weakly: with its last user gone, the engine goes, and the next user gets a new one.
    auto const old = std::weak_ptr<Engine>(*first);
    first->reset();
    second->reset();
    CHECK(old.expired());
    auto third = provider.acquire();
    REQUIRE(third.has_value());
    CHECK(*third != nullptr);
}

TEST_CASE("EngineProvider.retirement", "[vtextension]")
{
    // What a runner retires lives in the provider until an owner thread releases it.
    auto provider = EngineProvider { ExtensionLimits {} };
    auto engine = provider.acquire();
    REQUIRE(engine.has_value());
    auto const weak = std::weak_ptr<Engine>(*engine);
    provider.retire(retiredEngine(std::move(*engine)));
    CHECK_FALSE(weak.expired());
    // An acquire meanwhile reuses it rather than creating a second runtime.
    auto again = provider.acquire();
    REQUIRE(again.has_value());
    CHECK(again->get() == weak.lock().get());
    again->reset();
    CHECK(weak.expired()); // acquire() released the retired reference on this thread.

    auto another = provider.acquire();
    REQUIRE(another.has_value());
    auto const anotherWeak = std::weak_ptr<Engine>(*another);
    provider.retire(retiredEngine(std::move(*another)));
    provider.releaseRetired();
    CHECK(anotherWeak.expired());
}

TEST_CASE("ExtensionHost.unloadableNoticeIsDrawn", "[vtextension]")
{
    auto harness = RunnerHarness {};
    auto engines = CountingEngineProvider {};
    auto config = ExtensionHostConfig {};
    auto broken = minimal("broken");
    broken.module = { std::byte { 0 } };
    config.extensions.push_back(std::move(broken));
    auto host =
        ExtensionHost(std::move(config),
                      ExtensionHostServices { .engines = engines,
                                              .runners = harness.services(harness.scheduler, engines) });
    auto const notice = host.snapshot("broken");
    REQUIRE(notice != nullptr);
    REQUIRE(notice->lines.size() == 1);
    CHECK(notice->lines.front().toUtf8Trimmed().starts_with("\u26A0 broken disabled"));
}
