// SPDX-License-Identifier: Apache-2.0
//
// One engine per extension world: a module loads only on the engine of the world it was built for, checked
// against that world's exact imports, exports and placement; and two engines alive at once each reach
// their own handlers.
#include <core/platform/Clock.hpp>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vtextension/CompiledExtension.hpp>
#include <vtextension/DaemonImports.hpp>
#include <vtextension/DaemonInstance.hpp>
#include <vtextension/Engine.hpp>
#include <vtextension/ExtensionError.hpp>
#include <vtextension/ExtensionLimits.hpp>
#include <vtextension/HostBindings.hpp>
#include <vtextension/Instance.hpp>
#include <vtextension/World.hpp>
#include <vtextension/testing/FakeDaemonHostServices.hpp>
#include <vtextension/testing/Harness.hpp>
#include <vtextension/testing/TestModules.hpp>
#include <vtextension/wasm/DefaultBackend.hpp>
#include <vtextension/wasm/WasmBinary.hpp>
#include <vtextension/wasm/WasmRuntime.hpp>

using namespace std::string_view_literals;
using namespace vtextension;
using vtextension::testing::daemonModule;
using vtextension::testing::toModule;

namespace
{

/// One world check: a module, the world of the engine it is loaded on, and what loading answers.
struct WorldCheckRow
{
    std::string_view name;                 ///< What the row is about.
    std::vector<std::byte> module;         ///< The module's bytes.
    World world;                           ///< The engine's world.
    std::optional<ExtensionError> refusal; ///< Why it does not load; std::nullopt if it does.
};

/// @return Every world check, each with the module built for it.
[[nodiscard]] std::vector<WorldCheckRow> worldCheckRows()
{
    using namespace vtextension::testing;
    auto rows = std::vector<WorldCheckRow> {};
    rows.push_back({ "the daemon probe on its own world", daemonProbe(), World::Daemon, std::nullopt });
    rows.push_back({ "the daemon probe on the status line",
                     daemonProbe(),
                     World::StatusLine,
                     ExtensionError::WorldMismatch });
    rows.push_back({ "a status-line module on the daemon",
                     daemonModule(FollowerModule),
                     World::Daemon,
                     ExtensionError::WorldMismatch });
    rows.push_back({ "a status-line module on its own world",
                     toModule(FollowerModule),
                     World::StatusLine,
                     std::nullopt });
    rows.push_back({ "both worlds' imports, on the daemon",
                     daemonModule(MixedWorldsModule),
                     World::Daemon,
                     ExtensionError::WorldMismatch });
    rows.push_back({ "both worlds' imports, on the status line",
                     daemonModule(MixedWorldsModule),
                     World::StatusLine,
                     ExtensionError::WorldMismatch });
    rows.push_back({ "a control verb on the daemon",
                     daemonModule(DaemonImportsControlModule),
                     World::Daemon,
                     ExtensionError::WorldMismatch });
    rows.push_back({ "a render export on the daemon",
                     daemonModule(DaemonProbeWithRenderModule),
                     World::Daemon,
                     ExtensionError::WorldMismatch });
    rows.push_back({ "an unserved daemon major version",
                     daemonModule(DaemonNextMajorModule),
                     World::Daemon,
                     ExtensionError::WorldMismatch });
    rows.push_back({ "a status-line module importing a daemon function",
                     toModule(StatusLineImportsDaemonModule),
                     World::StatusLine,
                     ExtensionError::WorldMismatch });
    rows.push_back({ "a daemon module without a placement",
                     daemonProbeNoPlacement(),
                     World::Daemon,
                     ExtensionError::WorldMismatch });
    return rows;
}

/// @return Whether @p module was refused, and why; std::nullopt if it loaded. The other world's engine is
///         alive meanwhile: WAMR's registry of host functions is process-wide, so the other world's imports
///         are satisfied, and only the world's exact import list can refuse them.
[[nodiscard]] std::optional<ExtensionError> loadOutcome(wasm::IWasmBackend& backend,
                                                        World world,
                                                        std::vector<std::byte> module)
{
    auto const other = world == World::Daemon ? World::StatusLine : World::Daemon;
    auto const otherEngine = Engine::create(backend, other, ExtensionLimits {});
    REQUIRE(otherEngine.has_value());
    auto const engine = Engine::create(backend, world, ExtensionLimits {});
    REQUIRE(engine.has_value());
    auto const compiled = CompiledExtension::load(**engine, std::move(module));
    return compiled ? std::nullopt : std::optional { compiled.error() };
}

/// @return The import module names of @p module's function imports.
[[nodiscard]] std::set<std::string> importModulesOf(std::span<uint8_t const> module)
{
    auto const bytes = std::as_bytes(module);
    auto const sections = wasm::binary::sections(bytes);
    REQUIRE(sections.has_value());
    auto names = std::set<std::string> {};
    constexpr auto ImportSectionId = uint8_t { 2 };
    constexpr auto FunctionImport = uint8_t { 0 };
    for (auto const& section: *sections | std::views::filter([](wasm::binary::Section const& candidate) {
             return candidate.id == ImportSectionId;
         }))
    {
        auto reader = wasm::binary::Reader { section.content };
        auto const count = reader.u32();
        REQUIRE(count.has_value());
        for ([[maybe_unused]] auto const _: std::views::iota(uint32_t { 0 }, *count))
        {
            auto const moduleName = reader.name();
            auto const fieldName = reader.name();
            auto const kind = reader.byte();
            REQUIRE((moduleName && fieldName && kind));
            // The hand-assembled modules import functions only, whose description is a type index.
            REQUIRE(*kind == FunctionImport);
            REQUIRE(reader.u32().has_value());
            auto const chars =
                std::views::transform(*moduleName, [](std::byte b) { return static_cast<char>(b); });
            names.emplace(chars.begin(), chars.end());
        }
    }
    return names;
}

/// @return The worlds whose host-function modules @p modules names.
[[nodiscard]] std::set<World> worldsNamedBy(std::set<std::string> const& modules)
{
    auto worlds = std::set<World> {};
    for (auto const world: { World::StatusLine, World::Daemon })
        for (auto const& function: detail::hostFunctions(world))
            if (modules.contains(std::string(function.module)))
                worlds.insert(world);
    return worlds;
}

} // namespace

TEST_CASE("WorldCheck: a module loads on the engine of its own world only", "[daemon][world]")
{
    auto const backend = wasm::makeDefaultBackend();
    for (auto const& row: worldCheckRows())
    {
        CAPTURE(row.name);
        CHECK(loadOutcome(*backend, row.world, row.module) == row.refusal);
    }
}

TEST_CASE("WorldCheck: each world's import list is its host functions, exactly", "[daemon][world]")
{
    for (auto const world: { World::StatusLine, World::Daemon })
    {
        auto const functions = detail::hostFunctions(world);
        auto const imports = detail::worldImports(world);
        REQUIRE(imports.size() == functions.size());
        for (auto const index: std::views::iota(std::size_t { 0 }, functions.size()))
            CHECK(
                imports[index]
                == detail::WorldImport { .module = functions[index].module, .name = functions[index].name });
    }
    CHECK(detail::worldImports(World::Daemon).size() == DaemonImportRows.size());
}

TEST_CASE("WorldCheck: no module name is shared between the worlds' tables", "[daemon][world]")
{
    auto const modulesOf = [](World world) {
        auto modules = std::set<std::string_view> {};
        for (auto const& function: detail::hostFunctions(world))
            modules.insert(function.module);
        return modules;
    };
    auto const statusLine = modulesOf(World::StatusLine);
    auto const daemon = modulesOf(World::Daemon);
    for (auto const& module: daemon)
    {
        CAPTURE(module);
        CHECK_FALSE(statusLine.contains(module));
    }

    // Every hand-assembled module imports from one world's modules at most; only those built to mix them
    // name both (a control verb is the status line's).
    constexpr auto Mixers = std::array { "MixedWorldsModule"sv, "DaemonImportsControlModule"sv };
    for (auto const& entry: vtextension::testing::HandAssembledModules)
    {
        CAPTURE(entry.name);
        auto const worlds = worldsNamedBy(importModulesOf(entry.bytes));
        if (std::ranges::contains(Mixers, entry.name))
            CHECK(worlds.size() == 2);
        else
            CHECK(worlds.size() <= 1);
    }
}

TEST_CASE("WorldCheck: with both engines alive each world's call reaches its own handler", "[daemon][world]")
{
    // WAMR's registry of host functions is process-wide and searched by module and field name: a daemon
    // module's `log` must reach the daemon's handler, and the status line's the status line's, with both
    // engines alive -- and a status-line engine going away must not take the daemon's registrations along.
    auto const backend = wasm::makeDefaultBackend();
    auto clock = core::platform::ManualClock {};
    auto const limits = ExtensionLimits {};

    auto daemonEngine = Engine::create(*backend, World::Daemon, limits);
    REQUIRE(daemonEngine.has_value());
    auto statusEngine = Engine::create(*backend, World::StatusLine, limits);
    REQUIRE(statusEngine.has_value());

    auto const daemonCompiled = CompiledExtension::load(**daemonEngine, vtextension::testing::daemonProbe());
    REQUIRE(daemonCompiled.has_value());
    auto statusCompiled =
        CompiledExtension::load(**statusEngine, toModule(vtextension::testing::FollowerModule));
    REQUIRE(statusCompiled.has_value());

    auto daemonServices = vtextension::testing::FakeDaemonHostServices {};
    auto daemonInstance =
        DaemonInstance::create(*daemonCompiled, daemonServices, clock, **daemonEngine, limits);
    REQUIRE(daemonInstance.has_value());
    auto statusServices = vtextension::testing::FakeHostServices {};
    auto statusInstance = Instance::create(**statusEngine, **statusCompiled, statusServices, clock, limits);
    REQUIRE(statusInstance.has_value());

    REQUIRE((*daemonInstance)->load("{}", DaemonHostInfo {}).has_value());
    REQUIRE((*statusInstance)->load({}).has_value());
    REQUIRE((*statusInstance)->render(vtextension::testing::surface(10, 1)).has_value());
    REQUIRE((*daemonInstance)->update(DaemonHint::Wake).has_value());

    REQUIRE(statusServices.logged.size() == 1);
    CHECK(statusServices.logged.front().message == "render");
    CHECK(daemonServices.logged
          == std::vector {
              vtextension::testing::DaemonLogged { .level = LogLevel::Info, .message = "loaded" } });
    CHECK(daemonServices.calls == std::vector { DaemonImport::Log, DaemonImport::Now });

    // The status-line world goes away, its registrations with it.
    statusInstance->reset();
    statusCompiled->reset();
    statusEngine->reset();

    REQUIRE((*daemonInstance)->update(DaemonHint::Wake).has_value());
    CHECK(daemonServices.calls == std::vector { DaemonImport::Log, DaemonImport::Now, DaemonImport::Now });
}
