// SPDX-License-Identifier: Apache-2.0
//
// The registry and its breaker over a real host: one extension's helper shared by its panes, whose death
// counts once however many of them saw it, and which an open breaker no longer spawns.
#include <core/platform/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include <vtextension/CompiledExtension.hpp>
#include <vtextension/Engine.hpp>
#include <vtextension/ExtensionError.hpp>
#include <vtextension/ExtensionHost.hpp>
#include <vtextension/ExtensionLimits.hpp>
#include <vtextension/ExtensionRegistry.hpp>
#include <vtextension/HostBreaker.hpp>
#include <vtextension/Instance.hpp>
#include <vtextension/testing/Harness.hpp>
#include <vtextension/testing/TestModules.hpp>
#include <vtextension/wasm/BackendConfig.hpp>
#include <vtextension/wasm/DefaultBackend.hpp>
#include <vtextension/wasm/WasmRuntime.hpp>
#include <vtextension/wasm/host/HostServer.hpp>
#include <vtextension/wasm/host/InProcessHost.hpp>
#include <vtextension/wasm/remote/proto/Pdu.hpp>
#include <vtextension/wasm/testing/DecoratedEngine.hpp>
#include <vtextension/wasm/testing/HostedEngine.hpp>

using namespace vtextension;
using vtextension::testing::surface;
using vtextension::testing::toModule;
using vtextension::wasm::host::HostServerConfig;
using vtextension::wasm::host::InProcessHostConnector;

namespace
{

/// Every pane of these tests is held to the default limits.
constexpr auto Limits = ExtensionLimits {};

/// @return A connector starting each host in this process, on an engine of its own.
[[nodiscard]] std::shared_ptr<InProcessHostConnector> makeConnector()
{
    return std::make_shared<InProcessHostConnector>([] { return wasm::testing::makeHostedEngine(); },
                                                    HostServerConfig {});
}

/// A registry and an engine whose hosts run in this process, started through a connector the test sees.
struct RemoteRig
{
    /// @param hosts What starts the hosts.
    explicit RemoteRig(std::shared_ptr<InProcessHostConnector> hosts = makeConnector()):
        connector { std::move(hosts) }
    {
    }

    std::shared_ptr<InProcessHostConnector> connector;
    std::unique_ptr<wasm::IWasmBackend> backend = wasm::makeDefaultBackend(wasm::BackendConfig {
        .timeouts = {}, .connector = connector, .lockdown = wasm::LockdownRequirement::AcceptUnsandboxed });
    std::shared_ptr<Engine> engine = [this] {
        auto created = Engine::create(*backend, Limits);
        REQUIRE(created.has_value());
        return std::move(*created);
    }();
    std::unique_ptr<wasm::IThreadGuard> threadGuard = vtextension::testing::preparedThread(*backend);
    core::platform::ManualClock clock;
    ExtensionRegistry registry { clock, Limits };
    vtextension::testing::FakeHostServices services;

    /// @return The minimal statusline module, acquired from the registry; it must load.
    [[nodiscard]] std::shared_ptr<CompiledExtension> acquire()
    {
        auto compiled = registry.acquire(*engine, key(), toModule(vtextension::testing::MinimalModule));
        REQUIRE(compiled.has_value());
        return std::move(*compiled);
    }

    /// @return A pane's instance of @p compiled, or why there is none.
    [[nodiscard]] std::expected<std::unique_ptr<Instance>, ExtensionError> instantiate(
        CompiledExtension const& compiled)
    {
        return Instance::create(*engine, compiled, services, clock, Limits);
    }

    /// @return The key every pane of these tests names its extension by.
    [[nodiscard]] static RegistryKey key()
    {
        auto const spec = ExtensionSpec { .name = "clock",
                                          .hostId = "clock@/ext/minimal.wasm",
                                          .module = toModule(vtextension::testing::MinimalModule),
                                          .config = {},
                                          .placement = Placement::Segment,
                                          .runCommand = CommandPermission::Deny };
        return registryKeyOf(spec, ExtensionWorld::StatusLine);
    }
};

/// Whether the next memory read a host serves kills it.
enum class ReadFate : uint8_t
{
    Served, ///< The read is served.
    Fatal,  ///< The host dies serving it: it answers nothing more.
};

/// Kills every host a connector started once armed, from inside the next memory read one of them serves:
/// after the host answered the call that the read follows, and before it answers the read.
class DeathOnRead
{
  public:
    /// @param connector The connector whose hosts die; must outlive every read.
    void arm(InProcessHostConnector& connector) noexcept
    {
        _connector = &connector;
        _fate.store(ReadFate::Fatal, std::memory_order_release);
    }

    /// Called by a host serving a read: kills it if armed, once.
    void onRead() noexcept
    {
        if (_fate.exchange(ReadFate::Served, std::memory_order_acq_rel) == ReadFate::Fatal)
            _connector->terminateAll();
    }

  private:
    InProcessHostConnector* _connector = nullptr;
    std::atomic<ReadFate> _fate { ReadFate::Served };
};

/// An instance in a host that consults a DeathOnRead before each memory read it serves.
class DyingOnRead final: public wasm::testing::ForwardingInstance
{
  public:
    DyingOnRead(std::unique_ptr<wasm::IWasmInstance> inner, std::shared_ptr<DeathOnRead> death) noexcept:
        ForwardingInstance { std::move(inner) }, _death { std::move(death) }
    {
    }

    [[nodiscard]] std::expected<std::span<std::byte>, wasm::MemoryFailure> memory(uint32_t address,
                                                                                  uint32_t size) override
    {
        _death->onRead();
        return ForwardingInstance::memory(address, size);
    }

  private:
    std::shared_ptr<DeathOnRead> _death;
};

/// @return A connector whose hosts' instances die on the read @p death arms. They copy memory: a read of
///         shared memory never reaches the host.
[[nodiscard]] std::shared_ptr<InProcessHostConnector> makeDyingConnector(
    std::shared_ptr<DeathOnRead> const& death)
{
    return std::make_shared<InProcessHostConnector>(
        [death] {
            return std::make_unique<wasm::testing::DecoratedEngine>(
                wasm::testing::makeHostedEngine(wasm::remote::proto::MemoryMode::Copy),
                [death](std::unique_ptr<wasm::IWasmInstance> instance) {
                    return std::unique_ptr<wasm::IWasmInstance> { std::make_unique<DyingOnRead>(
                        std::move(instance), death) };
                });
        },
        HostServerConfig {});
}

} // namespace

TEST_CASE("one death counts once across panes", "[vtextension][remote][registry]")
{
    auto rig = RemoteRig {};
    auto const compiled = rig.acquire();
    auto panes = std::vector<std::unique_ptr<Instance>> {};
    for ([[maybe_unused]] auto const pane: std::views::iota(0, 10))
    {
        auto instance = rig.instantiate(*compiled);
        REQUIRE(instance.has_value());
        panes.push_back(std::move(*instance));
    }
    REQUIRE(rig.connector->connects() == 1);

    CHECK(rig.connector->terminateAll() == 1);
    for (auto const& pane: panes)
        CHECK(pane->render(surface(10, 1)).error() == ExtensionError::HostUnavailable);
    CHECK(compiled->breaker()->faultCount() == 1);
    CHECK(compiled->breaker()->state() == BreakerState::Closed);
    CHECK(rig.connector->connects() == 1);

    // One respawn, for the next pane.
    auto const next = rig.instantiate(*compiled);
    REQUIRE(next.has_value());
    CHECK(rig.connector->connects() == 2);
}

TEST_CASE("breaker open fails fast without spawning", "[vtextension][remote][registry]")
{
    auto rig = RemoteRig {};
    auto const compiled = rig.acquire();
    for ([[maybe_unused]] auto const death: std::views::iota(0, 3))
    {
        auto const instance = rig.instantiate(*compiled);
        REQUIRE(instance.has_value());
        REQUIRE(rig.connector->terminateAll() == 1);
        CHECK((*instance)->render(surface(10, 1)).error() == ExtensionError::HostUnavailable);
    }
    REQUIRE(compiled->breaker()->state() == BreakerState::Open);
    auto const connects = rig.connector->connects();

    CHECK(rig.instantiate(*compiled).error() == ExtensionError::HostUnavailable);
    CHECK(rig.connector->connects() == connects);
    CHECK(rig.connector->liveHosts() == 0);
}

TEST_CASE("registry drop races instantiate (remote)", "[vtextension][remote][registry][threading]")
{
    // As over the fake backend, with a real helper behind each entry: whichever thread wins, nothing is
    // left running once every reference is gone.
    auto const connector = makeConnector();
    {
        auto rig = RemoteRig { connector };
        for ([[maybe_unused]] auto const round: std::views::iota(0, 200))
        {
            auto first = std::optional { rig.acquire() };
            auto dropper = std::jthread { [&first] { first.reset(); } };
            auto const second = rig.registry.acquire(
                *rig.engine, RemoteRig::key(), toModule(vtextension::testing::MinimalModule));
            REQUIRE(second);
            auto const instance = rig.instantiate(**second);
            CHECK(instance);
            dropper.join();
            CHECK(rig.registry.liveEntries() <= 1);
        }
    }
    CHECK(connector->liveHosts() == 0);
}

TEST_CASE("host death during a memory read counts as the host's", "[vtextension][remote][registry]")
{
    // The host answers the render honestly, then dies before it answers the read of the string it returned:
    // the pane sees the host gone, not a guest trap, and the death counts once against the extension.
    auto const death = std::make_shared<DeathOnRead>();
    auto rig = RemoteRig { makeDyingConnector(death) };
    auto const compiled = rig.acquire();
    auto const instance = rig.instantiate(*compiled);
    REQUIRE(instance.has_value());

    death->arm(*rig.connector);
    CHECK((*instance)->render(surface(10, 1)).error() == ExtensionError::HostUnavailable);
    CHECK(compiled->breaker()->faultCount() == 1);
    CHECK(rig.connector->liveHosts() == 0);
}
