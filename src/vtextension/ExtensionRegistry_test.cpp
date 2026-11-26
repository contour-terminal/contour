// SPDX-License-Identifier: Apache-2.0
//
// One extension, loaded once per process and shared by its panes; and the breaker that counts its host's
// deaths, once each however many panes saw one.
#include <core/async/testing/ManualExecutor.hpp>
#include <core/platform/Clock.hpp>
#include <core/platform/testing/InMemoryFileSystem.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <vtextension/CompiledExtension.hpp>
#include <vtextension/Engine.hpp>
#include <vtextension/ExtensionHost.hpp>
#include <vtextension/ExtensionRegistry.hpp>
#include <vtextension/HostBreaker.hpp>
#include <vtextension/Instance.hpp>
#include <vtextension/SessionExtensions.hpp>
#include <vtextension/testing/FakeWasmBackend.hpp>
#include <vtextension/testing/Harness.hpp>
#include <vtextension/testing/ManualScheduler.hpp>
#include <vtextension/testing/QueuedOwner.hpp>
#include <vtextension/testing/RunnerHarness.hpp>
#include <vtextension/testing/TestModules.hpp>

using namespace std::chrono_literals;
using namespace vtextension;
using vtextension::testing::FakeWasmBackend;
using vtextension::testing::toModule;

namespace
{

/// The limits every breaker of these tests is built with: three faults within a minute open it.
constexpr auto Limits = ExtensionLimits {};
static_assert(Limits.faultsBeforeDisable == 3 && Limits.faultWindow == 60s);

/// @return A breaker on @p clock with the default limits.
[[nodiscard]] HostBreaker breakerOn(core::platform::IClock& clock)
{
    return HostBreaker { clock, Limits.faultsBeforeDisable, Limits.faultWindow };
}

/// Records a fault against a fresh admission of @p breaker, which must admit.
void faultOnce(HostBreaker& breaker)
{
    auto const admission = breaker.admit();
    REQUIRE(admission.has_value());
    breaker.recordHostFault(*admission);
}

/// @return MinimalModule with a custom section appended: other bytes, the same extension.
[[nodiscard]] std::vector<std::byte> minimalModuleVariant()
{
    auto bytes = toModule(vtextension::testing::MinimalModule);
    // Custom section (id 0), 2 bytes: a name of one byte, "x".
    for (auto const byte: std::to_array<uint8_t>({ 0x00, 0x02, 0x01, 'x' }))
        bytes.push_back(static_cast<std::byte>(byte));
    return bytes;
}

/// @return @p bytes as the text a filesystem holds.
[[nodiscard]] std::string asText(std::vector<std::byte> const& bytes)
{
    auto text = std::string {};
    for (auto const byte: bytes)
        text.push_back(static_cast<char>(byte));
    return text;
}

/// An extension spec of the minimal module, under @p config.
[[nodiscard]] ExtensionSpec minimalSpec(ConfigMap config)
{
    return ExtensionSpec { .name = "clock",
                           .hostId = "clock@/ext/minimal.wasm",
                           .module = toModule(vtextension::testing::MinimalModule),
                           .config = std::move(config),
                           .placement = Placement::Segment,
                           .runCommand = CommandPermission::Deny };
}

/// An engine on a fake backend, a registry on a manual clock, and the thread prepared for both.
struct RegistryFixture
{
    FakeWasmBackend fake;
    core::platform::ManualClock clock {};
    std::shared_ptr<Engine> engine = [this] {
        auto created = Engine::create(fake, Limits);
        REQUIRE(created.has_value());
        return std::move(*created);
    }();
    ExtensionRegistry registry { clock, Limits };

    /// @return The minimal module under @p config, acquired from the registry; it must load.
    [[nodiscard]] std::shared_ptr<CompiledExtension> acquire(ConfigMap config = {})
    {
        auto const spec = minimalSpec(std::move(config));
        auto compiled = registry.acquire(*engine,
                                         registryKeyOf(spec, ExtensionWorld::StatusLine),
                                         toModule(vtextension::testing::MinimalModule));
        REQUIRE(compiled.has_value());
        return std::move(*compiled);
    }
};

} // namespace

TEST_CASE("ExtensionRegistry.breakerOpensAtTheLimit", "[vtextension][registry]")
{
    auto clock = core::platform::ManualClock {};
    auto breaker = breakerOn(clock);
    CHECK(breaker.state() == BreakerState::Closed);
    for ([[maybe_unused]] auto const fault: std::views::iota(0, 3))
    {
        faultOnce(breaker);
        clock.advance(10s);
    }
    CHECK(breaker.state() == BreakerState::Open);
    CHECK(breaker.admit().error() == ExtensionError::HostUnavailable);
}

TEST_CASE("ExtensionRegistry.faultsAgeOut", "[vtextension][registry]")
{
    auto clock = core::platform::ManualClock {};
    auto breaker = breakerOn(clock);
    faultOnce(breaker);
    clock.advance(61s);
    faultOnce(breaker);
    faultOnce(breaker);
    CHECK(breaker.state() == BreakerState::Closed);
    CHECK(breaker.faultCount() == 2);
}

TEST_CASE("ExtensionRegistry.staleAdmissionCountsOnce", "[vtextension][registry]")
{
    // Ten panes admitted together saw the one death: the first report counts it, the rest are stale.
    auto clock = core::platform::ManualClock {};
    auto breaker = breakerOn(clock);
    auto const admission = breaker.admit();
    REQUIRE(admission.has_value());
    for ([[maybe_unused]] auto const pane: std::views::iota(0, 10))
        breaker.recordHostFault(*admission);
    CHECK(breaker.faultCount() == 1);
    CHECK(breaker.state() == BreakerState::Closed);
}

TEST_CASE("reload resets breaker", "[vtextension][registry]")
{
    auto fixture = RegistryFixture {};
    auto const compiled = fixture.acquire();
    REQUIRE(compiled->breaker() != nullptr);
    auto const& breaker = *compiled->breaker();

    auto const oldAdmission = compiled->admitHost();
    REQUIRE(oldAdmission.has_value());
    compiled->recordHostFault(*oldAdmission);
    for ([[maybe_unused]] auto const fault: std::views::iota(0, 2))
    {
        auto const admission = compiled->admitHost();
        REQUIRE(admission.has_value());
        compiled->recordHostFault(*admission);
    }
    REQUIRE(breaker.state() == BreakerState::Open);
    CHECK(compiled->admitHost().error() == ExtensionError::HostUnavailable);

    fixture.registry.resetBreakers();
    CHECK(breaker.state() == BreakerState::Closed);
    CHECK(compiled->admitHost().has_value());
    // A pane admitted before the reload reports a death it saw then: it counts no more.
    compiled->recordHostFault(*oldAdmission);
    CHECK(breaker.faultCount() == 0);
}

TEST_CASE("ExtensionRegistry.differentConfigGetsItsOwnEntry", "[vtextension][registry]")
{
    auto fixture = RegistryFixture {};
    auto const plain = fixture.acquire({ { "format", "%H:%M" }, { "zone", "UTC" } });
    auto const plainIdentity = fixture.fake.lastIdentity();
    auto const other = fixture.acquire({ { "format", "%H:%M:%S" }, { "zone", "UTC" } });
    auto const otherIdentity = fixture.fake.lastIdentity();

    CHECK(plain != other);
    CHECK(fixture.registry.liveEntries() == 2);
    CHECK(fixture.fake.loads() == 2);
    CHECK(plainIdentity != otherIdentity);
    CHECK_FALSE(plainIdentity.key.empty());

    // The same configuration again, in another order, is the same entry.
    auto const again = fixture.acquire({ { "zone", "UTC" }, { "format", "%H:%M" } });
    CHECK(again == plain);
    CHECK(fixture.fake.loads() == 2);
}

TEST_CASE("ExtensionRegistry.keyOfConfigIgnoresItsOrder", "[vtextension][registry]")
{
    auto const forward =
        registryKeyOf(minimalSpec({ { "a", "1" }, { "b", "2" } }), ExtensionWorld::StatusLine);
    auto const backward =
        registryKeyOf(minimalSpec({ { "b", "2" }, { "a", "1" } }), ExtensionWorld::StatusLine);
    CHECK(forward == backward);
    CHECK(forward.configDigest.size() == 64);
    CHECK(forward.moduleDigest.size() == 64);

    // A field's bytes cannot slide into its neighbour's: each is length-prefixed.
    auto const joined = registryKeyOf(minimalSpec({ { "ab", "" } }), ExtensionWorld::StatusLine);
    auto const split = registryKeyOf(minimalSpec({ { "a", "b" } }), ExtensionWorld::StatusLine);
    CHECK(joined.configDigest != split.configDigest);

    auto allowed = minimalSpec({});
    allowed.runCommand = CommandPermission::Allow;
    CHECK(registryKeyOf(allowed, ExtensionWorld::StatusLine).configDigest
          != registryKeyOf(minimalSpec({}), ExtensionWorld::StatusLine).configDigest);

    // The module's identity names no module bytes: a changed module under it reaches the same host.
    auto changed = minimalSpec({});
    changed.module = minimalModuleVariant();
    auto const changedKey = registryKeyOf(changed, ExtensionWorld::StatusLine);
    auto const unchangedKey = registryKeyOf(minimalSpec({}), ExtensionWorld::StatusLine);
    CHECK(changedKey.moduleDigest != unchangedKey.moduleDigest);
    CHECK(moduleIdentityOf(changedKey) == moduleIdentityOf(unchangedKey));
}

TEST_CASE("ExtensionRegistry.anEntryIsPerEngine", "[vtextension][registry]")
{
    // An entry loaded on another engine is one this engine cannot run.
    auto fixture = RegistryFixture {};
    auto const first = fixture.acquire();
    auto other = Engine::create(fixture.fake, Limits);
    REQUIRE(other.has_value());
    auto const spec = minimalSpec({});
    auto const second = fixture.registry.acquire(**other,
                                                 registryKeyOf(spec, ExtensionWorld::StatusLine),
                                                 toModule(vtextension::testing::MinimalModule));
    REQUIRE(second.has_value());
    CHECK(&(*second)->engine() == other->get());
    CHECK(*second != first);
    CHECK(fixture.fake.loads() == 2);
}

TEST_CASE("ExtensionRegistry.aFailedLoadLeavesNoEntry", "[vtextension][registry]")
{
    auto fixture = RegistryFixture {};
    fixture.fake.loadFailure = ExtensionError::InvalidComponent;
    auto const spec = minimalSpec({});
    auto const failed = fixture.registry.acquire(*fixture.engine,
                                                 registryKeyOf(spec, ExtensionWorld::StatusLine),
                                                 toModule(vtextension::testing::MinimalModule));
    CHECK(failed.error() == ExtensionError::InvalidComponent);
    CHECK(fixture.registry.liveEntries() == 0);

    // Tried again on the next acquire.
    fixture.fake.loadFailure.reset();
    CHECK(fixture.acquire() != nullptr);
    CHECK(fixture.fake.loads() == 2);
}

namespace
{
/// @return What acquiring the minimal module from @p fixture's registry failed with; nothing if it loaded.
[[nodiscard]] std::optional<ExtensionError> acquireError(RegistryFixture& fixture)
{
    auto const spec = minimalSpec({});
    auto const acquired = fixture.registry.acquire(*fixture.engine,
                                                   registryKeyOf(spec, ExtensionWorld::StatusLine),
                                                   toModule(vtextension::testing::MinimalModule));
    return acquired ? std::nullopt : std::optional { acquired.error() };
}
} // namespace

TEST_CASE("load timeout counts a breaker fault", "[vtextension][registry]")
{
    // A host that dies or hangs while loading is the host's fault: three of them open the breaker, after
    // which nothing is loaded, or started, any more.
    auto fixture = RegistryFixture {};
    fixture.fake.loadFailure = ExtensionError::HostUnavailable;
    for ([[maybe_unused]] auto const attempt: std::views::iota(0, 3))
        CHECK(acquireError(fixture) == ExtensionError::HostUnavailable);
    CHECK(fixture.fake.loads() == 3);
    CHECK(acquireError(fixture) == ExtensionError::HostUnavailable);
    CHECK(fixture.fake.loads() == 3);
}

TEST_CASE("reload resets a load breaker", "[vtextension][registry]")
{
    auto fixture = RegistryFixture {};
    fixture.fake.loadFailure = ExtensionError::HostUnavailable;
    for ([[maybe_unused]] auto const attempt: std::views::iota(0, 3))
        CHECK(acquireError(fixture) == ExtensionError::HostUnavailable);
    REQUIRE(acquireError(fixture) == ExtensionError::HostUnavailable);
    REQUIRE(fixture.fake.loads() == 3);

    // Reloaded: the key's breaker closes, and the next acquire loads again -- and fails again, counting anew.
    fixture.registry.resetBreakers();
    CHECK(acquireError(fixture) == ExtensionError::HostUnavailable);
    CHECK(fixture.fake.loads() == 4);
}

TEST_CASE("InvalidComponent counts no fault", "[vtextension][registry]")
{
    // A module the host refused is the extension's fault, which no breaker counts: it is tried every time.
    auto fixture = RegistryFixture {};
    fixture.fake.loadFailure = ExtensionError::InvalidComponent;
    for ([[maybe_unused]] auto const attempt: std::views::iota(0, 5))
        CHECK(acquireError(fixture) == ExtensionError::InvalidComponent);
    CHECK(fixture.fake.loads() == 5);
}

TEST_CASE("ExtensionRegistry.faultsWhileLoadingCarryOverToTheRunningEntry", "[vtextension][registry]")
{
    // The breaker is the key's, not the entry's: the deaths while loading count with those while running.
    auto fixture = RegistryFixture {};
    fixture.fake.loadFailure = ExtensionError::HostUnavailable;
    CHECK(acquireError(fixture) == ExtensionError::HostUnavailable);
    CHECK(acquireError(fixture) == ExtensionError::HostUnavailable);
    fixture.fake.loadFailure.reset();
    auto const compiled = fixture.acquire();
    REQUIRE(compiled->breaker() != nullptr);
    CHECK(compiled->breaker()->faultCount() == 2);
}

TEST_CASE("ExtensionRegistry.droppedEntryIsLoadedAgain", "[vtextension][registry]")
{
    auto fixture = RegistryFixture {};
    auto compiled = fixture.acquire();
    CHECK(fixture.registry.liveEntries() == 1);
    compiled.reset();
    CHECK(fixture.registry.liveEntries() == 0);
    compiled = fixture.acquire();
    CHECK(fixture.fake.loads() == 2);
}

TEST_CASE("Instance.anOpenBreakerRefusesBeforeInstantiating", "[vtextension][registry]")
{
    auto fixture = RegistryFixture {};
    auto const compiled = fixture.acquire();
    for ([[maybe_unused]] auto const fault: std::views::iota(0, 3))
    {
        auto const admission = compiled->admitHost();
        REQUIRE(admission.has_value());
        compiled->recordHostFault(*admission);
    }
    auto services = vtextension::testing::FakeHostServices {};
    auto const instance = Instance::create(*fixture.engine, *compiled, services, fixture.clock, Limits);
    CHECK(instance.error() == ExtensionError::HostUnavailable);
    CHECK(fixture.fake.lastInstance == nullptr);
}

TEST_CASE("Instance.reportsItsHostsDeathOnce", "[vtextension][registry]")
{
    auto fixture = RegistryFixture {};
    fixture.fake.guest.exports["render"] =
        [](std::span<uint32_t> /*argv*/) -> std::expected<void, wasm::CallFailure> {
        return std::unexpected(wasm::CallFailure::HostCrashed);
    };
    auto const compiled = fixture.acquire();
    auto services = vtextension::testing::FakeHostServices {};
    auto const first = Instance::create(*fixture.engine, *compiled, services, fixture.clock, Limits);
    auto const second = Instance::create(*fixture.engine, *compiled, services, fixture.clock, Limits);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());

    // Both saw the one death, each twice: it counts once.
    for ([[maybe_unused]] auto const call: std::views::iota(0, 2))
    {
        CHECK((*first)->render({}).error() == ExtensionError::HostUnavailable);
        CHECK((*second)->render({}).error() == ExtensionError::HostUnavailable);
    }
    CHECK(compiled->breaker()->faultCount() == 1);

    // A pane admitted after it counts the next one.
    auto const third = Instance::create(*fixture.engine, *compiled, services, fixture.clock, Limits);
    REQUIRE(third.has_value());
    CHECK((*third)->render({}).error() == ExtensionError::HostUnavailable);
    CHECK(compiled->breaker()->faultCount() == 2);
}

TEST_CASE("Instance.aTrapIsNoHostDeath", "[vtextension][registry]")
{
    // A guest's own fault is the runner's to count, per pane; the breaker counts only its host's deaths.
    auto fixture = RegistryFixture {};
    fixture.fake.guest.exports["render"] =
        [](std::span<uint32_t> /*argv*/) -> std::expected<void, wasm::CallFailure> {
        return std::unexpected(wasm::CallFailure::Trap);
    };
    auto const compiled = fixture.acquire();
    auto services = vtextension::testing::FakeHostServices {};
    auto const instance = Instance::create(*fixture.engine, *compiled, services, fixture.clock, Limits);
    REQUIRE(instance.has_value());
    CHECK((*instance)->render({}).error() == ExtensionError::Trapped);
    CHECK(compiled->breaker()->faultCount() == 0);
}

namespace
{

/// Every service a pane's extensions reach, on a fake backend whose loads the test counts.
struct RebuildFixture
{
    vtextension::testing::RunnerHarness harness;
    FakeWasmBackend fake;
    vtextension::testing::ManualScheduler scheduler { fake };
    EngineProvider engines { fake, Limits };
    core::platform::ManualClock clock {};
    ExtensionRegistry registry { clock, Limits };
    core::async::testing::ManualExecutor builds;
    core::platform::testing::InMemoryFileSystem fileSystem;
    std::shared_ptr<vtextension::testing::QueuedOwner> owner =
        std::make_shared<vtextension::testing::QueuedOwner>();

    [[nodiscard]] SessionExtensionServices services()
    {
        return { .engines = engines,
                 .retirement = engines,
                 .registry = registry,
                 .scheduler = scheduler,
                 .builds = builds,
                 .grants = harness.grants,
                 .capture = harness.capture,
                 .wallClock = harness.wallClock,
                 .localTime = harness.localTime,
                 .environment = harness.environment,
                 .fileSystem = fileSystem,
                 .lookup = { .fileSystem = fileSystem, .bundledDirectories = {} },
                 .limits = Limits };
    }

    /// Runs the builds, then hands their results back to the owner.
    void build()
    {
        builds.drain();
        owner->run();
    }

    /// Runs the runners' last jobs, and releases what they retired.
    void settle()
    {
        scheduler.runPending();
        engines.releaseRetired();
    }
};

[[nodiscard]] DeclaredExtensions declaringClock()
{
    return DeclaredExtensions { .extensions = { DeclaredExtension { .name = "clock",
                                                                    .path = "/ext/minimal.wasm",
                                                                    .config = { { "format", "%H:%M" } },
                                                                    .runCommand = CommandPermission::Deny } },
                                .takeOver = std::nullopt };
}

} // namespace

TEST_CASE("rebuild with unchanged bytes reuses the entry", "[vtextension][registry]")
{
    auto fixture = RebuildFixture {};
    REQUIRE(fixture.fileSystem
                .writeFile("/ext/minimal.wasm", asText(toModule(vtextension::testing::MinimalModule)))
                .has_value());
    {
        auto extensions = SessionExtensions { SessionExtensionCallbacks {}, fixture.owner };
        extensions.rebuild(declaringClock(), {}, fixture.services());
        fixture.build();
        REQUIRE_FALSE(extensions.empty());
        CHECK(fixture.fake.loads() == 1);

        // A configuration reload: the same extension, the same bytes.
        extensions.rebuild(declaringClock(), {}, fixture.services());
        fixture.build();
        CHECK(fixture.fake.loads() == 1);
        CHECK(fixture.registry.liveEntries() == 1);

        // The module changed on disk: loaded anew.
        REQUIRE(
            fixture.fileSystem.writeFile("/ext/minimal.wasm", asText(minimalModuleVariant())).has_value());
        extensions.rebuild(declaringClock(), {}, fixture.services());
        fixture.build();
        CHECK(fixture.fake.loads() == 2);
    }
    fixture.settle();
}

TEST_CASE("ExtensionRuntime.configurationReloadedResetsEveryBreaker", "[vtextension][registry]")
{
    auto harness = vtextension::testing::RunnerHarness {};
    auto fake = FakeWasmBackend {};
    auto fileSystem = core::platform::testing::InMemoryFileSystem {};
    auto runtime = ExtensionRuntime { ExtensionRuntimeConfig { .grantsFile = "/config/extension-grants.txt",
                                                               .bundledDirectories = {},
                                                               .workerCount = 1,
                                                               .limits = Limits },
                                      ExtensionRuntimeServices { .backend = fake,
                                                                 .capture = harness.capture,
                                                                 .wallClock = harness.wallClock,
                                                                 .localTime = harness.localTime,
                                                                 .environment = harness.environment,
                                                                 .fileSystem = fileSystem } };
    auto const engine = runtime.sessionServices().engines.acquire();
    REQUIRE(engine.has_value());
    auto const spec = minimalSpec({});
    auto const compiled =
        runtime.sessionServices().registry.acquire(**engine,
                                                   registryKeyOf(spec, ExtensionWorld::StatusLine),
                                                   toModule(vtextension::testing::MinimalModule));
    REQUIRE(compiled.has_value());
    for ([[maybe_unused]] auto const fault: std::views::iota(0, 3))
    {
        auto const admission = (*compiled)->admitHost();
        REQUIRE(admission.has_value());
        (*compiled)->recordHostFault(*admission);
    }
    REQUIRE((*compiled)->breaker()->state() == BreakerState::Open);

    runtime.configurationReloaded();
    CHECK((*compiled)->breaker()->state() == BreakerState::Closed);
}

TEST_CASE("registry drop races instantiate", "[vtextension][registry][threading]")
{
    // The last pane closing on one thread while a rebuild acquires and instantiates the same entry on
    // another: whichever wins, the instance is made from a live module, and no entry is left behind.
    auto fixture = RegistryFixture {};
    auto services = vtextension::testing::FakeHostServices {};
    for ([[maybe_unused]] auto const round: std::views::iota(0, 200))
    {
        auto first = fixture.acquire();
        auto dropper = std::jthread { [&first] { first.reset(); } };
        auto const spec = minimalSpec({});
        auto const second = fixture.registry.acquire(*fixture.engine,
                                                     registryKeyOf(spec, ExtensionWorld::StatusLine),
                                                     toModule(vtextension::testing::MinimalModule));
        REQUIRE(second);
        auto const instance = Instance::create(*fixture.engine, **second, services, fixture.clock, Limits);
        CHECK(instance);
        dropper.join();
        CHECK(fixture.registry.liveEntries() <= 1);
    }
}

TEST_CASE("ExtensionRegistry.concurrentAcquiresLoadOnce", "[vtextension][registry][threading]")
{
    // Panes built at once (a window restored with many tabs) ask for one extension together: one load.
    auto fixture = RegistryFixture {};
    constexpr auto Threads = 8;
    auto results = std::vector<std::shared_ptr<CompiledExtension>>(Threads);
    {
        auto threads = std::vector<std::jthread> {};
        for (auto const index: std::views::iota(0, Threads))
            threads.emplace_back([&fixture, &results, index] {
                auto const spec = minimalSpec({});
                auto compiled = fixture.registry.acquire(*fixture.engine,
                                                         registryKeyOf(spec, ExtensionWorld::StatusLine),
                                                         toModule(vtextension::testing::MinimalModule));
                if (compiled)
                    results[static_cast<std::size_t>(index)] = std::move(*compiled);
            });
    }
    CHECK(fixture.fake.loads() == 1);
    CHECK(std::ranges::all_of(results,
                              [&](auto const& result) { return result && result == results.front(); }));
}

namespace
{

/// Holds every load that passes it until the test opens it.
class LoadGate
{
  public:
    /// Waits until the gate is open. On the loading thread.
    void pass()
    {
        auto lock = std::unique_lock { _mutex };
        _opened.wait(lock, [this] { return _isOpen; });
    }

    /// Lets every load held, and every later one, through.
    void open()
    {
        {
            auto const lock = std::scoped_lock { _mutex };
            _isOpen = true;
        }
        _opened.notify_all();
    }

  private:
    std::mutex _mutex;
    std::condition_variable _opened;
    bool _isOpen = false;
};

/// Spins until @p condition holds. The bound only keeps a broken registry from hanging the suite: a case that
/// passes never reaches it.
/// @return Whether @p condition came to hold.
template <typename Condition>
[[nodiscard]] bool eventually(Condition const& condition)
{
    auto const giveUp = std::chrono::steady_clock::now() + 10s;
    while (!condition())
    {
        if (std::chrono::steady_clock::now() > giveUp)
            return false;
        std::this_thread::yield();
    }
    return true;
}

using AcquireResult = std::expected<std::shared_ptr<CompiledExtension>, ExtensionError>;

/// What one load in flight handed the call that ran it, and the calls that waited for it.
struct SharedLoad
{
    AcquireResult loader;
    std::vector<AcquireResult> waiters;
};

/// Holds the first acquire of the minimal module in its load, has @p waiterCount more acquire it meanwhile,
/// checks that each waits for that load rather than running its own, then lets the load end with @p failure.
[[nodiscard]] SharedLoad shareOneLoad(RegistryFixture& fixture,
                                      std::size_t waiterCount,
                                      std::optional<ExtensionError> failure)
{
    auto gate = LoadGate {};
    fixture.fake.loadGate = [&gate] {
        gate.pass();
    };
    auto const acquire = [&fixture] {
        auto const spec = minimalSpec({});
        return fixture.registry.acquire(*fixture.engine,
                                        registryKeyOf(spec, ExtensionWorld::StatusLine),
                                        toModule(vtextension::testing::MinimalModule));
    };

    auto loader = std::optional<AcquireResult> {};
    auto waiters = std::vector<std::optional<AcquireResult>>(waiterCount);
    {
        auto threads = std::vector<std::jthread> {};
        threads.emplace_back([&] { loader = acquire(); });
        CHECK(eventually([&] { return fixture.fake.loads() == 1; }));
        for (auto& waiter: waiters)
            threads.emplace_back([&acquire, &waiter] { waiter = acquire(); });

        // Every later call waits for the load in flight; one that took it for a miss would load again.
        CHECK(eventually(
            [&] { return fixture.registry.waitingAcquires() == waiterCount || fixture.fake.loads() > 1; }));
        CHECK(fixture.fake.loads() == 1);
        CHECK(fixture.registry.waitingAcquires() == waiterCount);

        // Ordered before the load reads it by the gate's lock.
        fixture.fake.loadFailure = failure;
        gate.open();
    }
    fixture.fake.loadGate = nullptr;
    CHECK(fixture.registry.waitingAcquires() == 0);

    auto shared = SharedLoad { .loader = *loader, .waiters = {} };
    for (auto& waiter: waiters)
        shared.waiters.push_back(*waiter);
    return shared;
}

} // namespace

TEST_CASE("ExtensionRegistry.waitersShareALoadInFlight", "[vtextension][registry][threading]")
{
    auto fixture = RegistryFixture {};
    auto const shared = shareOneLoad(fixture, 4, std::nullopt);
    REQUIRE(shared.loader.has_value());
    for (auto const& waiter: shared.waiters)
    {
        REQUIRE(waiter.has_value());
        CHECK(*waiter == *shared.loader);
    }
    CHECK(fixture.fake.loads() == 1);
    CHECK(fixture.registry.liveEntries() == 1);
}

TEST_CASE("ExtensionRegistry.aFailedLoadInFlightFailsEveryWaiter", "[vtextension][registry][threading]")
{
    auto fixture = RegistryFixture {};
    auto const shared = shareOneLoad(fixture, 4, ExtensionError::InvalidComponent);
    CHECK(shared.loader.error() == ExtensionError::InvalidComponent);
    for (auto const& waiter: shared.waiters)
        CHECK(waiter.error() == ExtensionError::InvalidComponent);
    CHECK(fixture.registry.liveEntries() == 0);

    // Nothing of the failed load is kept: the next acquire loads again.
    fixture.fake.loadFailure.reset();
    CHECK(fixture.acquire() != nullptr);
    CHECK(fixture.fake.loads() == 2);
}
