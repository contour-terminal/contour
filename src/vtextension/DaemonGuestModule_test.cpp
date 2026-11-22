// SPDX-License-Identifier: Apache-2.0
//
// A daemon-world guest as a toolchain builds it: the module check over hand-assembled modules, and, when the
// WASI C++ toolchain built it (CONTOUR_EXTENSIONS_ORCHESTRATOR_GUEST), the C++ probe checked and run on the
// daemon engine of this build's runtime.

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vtextension/CompiledExtension.hpp>
#include <vtextension/DaemonImports.hpp>
#include <vtextension/DaemonInstance.hpp>
#include <vtextension/DaemonWorld.hpp>
#include <vtextension/Engine.hpp>
#include <vtextension/ExtensionLimits.hpp>
#include <vtextension/World.hpp>
#include <vtextension/testing/FakeDaemonHostServices.hpp>
#include <vtextension/testing/GuestModuleCheck.hpp>
#include <vtextension/testing/TestModules.hpp>
#include <vtextension/wasm/DefaultBackend.hpp>
#include <vtextension/wasm/WasmRuntime.hpp>

using namespace std::string_view_literals;
using namespace vtextension;
using vtextension::testing::daemonGuestModuleProblems;

namespace
{

using vtextension::testing::appendLeb128;
using vtextension::testing::appendName;

/// One section of a hand-assembled module.
struct RawSection
{
    uint8_t id;                     ///< What kind of section it is.
    std::vector<std::byte> content; ///< Its content.
};

/// @return A module of the WebAssembly header and @p sections, in order.
[[nodiscard]] std::vector<std::byte> assemble(std::vector<RawSection> const& sections)
{
    auto module = vtextension::testing::toModule(wasm::binary::ModuleHeader);
    for (auto const& section: sections)
        vtextension::testing::appendSection(module, section.id, section.content);
    return module;
}

/// @return A module of @p sections and, last, one manifest section, as a daemon guest carries it.
[[nodiscard]] std::vector<std::byte> guest(std::vector<RawSection> const& sections)
{
    return vtextension::testing::withManifest(assemble(sections), vtextension::testing::DaemonProbeManifest);
}

/// One import of a hand-assembled import section.
struct RawImport
{
    std::string_view module;            ///< The module it is imported from.
    std::string_view name;              ///< Its field name.
    std::vector<std::byte> description; ///< Its kind byte and what follows it.
};

/// @return A function import's description: kind 0, type 0.
[[nodiscard]] std::vector<std::byte> functionImport()
{
    return { std::byte { 0x00 }, std::byte { 0x00 } };
}

/// @return An import section of @p imports.
[[nodiscard]] RawSection importSection(std::vector<RawImport> const& imports)
{
    auto content = std::vector<std::byte> {};
    appendLeb128(content, imports.size());
    for (auto const& import: imports)
    {
        appendName(content, import.module);
        appendName(content, import.name);
        content.insert(content.end(), import.description.begin(), import.description.end());
    }
    return { .id = 2, .content = content };
}

/// @return An export section exporting function 0 as @p name.
[[nodiscard]] RawSection exportSection(std::string_view name)
{
    auto content = std::vector<std::byte> {};
    appendLeb128(content, 1);
    appendName(content, name);
    content.push_back(std::byte { 0x00 }); // a function
    appendLeb128(content, 0);
    return { .id = wasm::binary::ExportSectionId, .content = content };
}

/// One module the check is run on, and the problems it names.
struct ModuleCheckRow
{
    std::string_view name;             ///< What the row is about.
    std::vector<std::byte> module;     ///< The module's bytes.
    std::vector<std::string> problems; ///< What daemonGuestModuleProblems() answers.
};

/// @return Every row of the module check.
[[nodiscard]] std::vector<ModuleCheckRow> moduleCheckRows()
{
    constexpr auto Daemon = DaemonImportModule;
    constexpr auto Wasi = wasm::WasiModule;
    constexpr auto NotOne = "{} manifest sections, not exactly one"sv;
    auto rows = std::vector<ModuleCheckRow> {};
    rows.push_back({ "the hand-assembled daemon probe", vtextension::testing::daemonProbe(), {} });
    rows.push_back({ "daemon and allowed WASI imports",
                     guest({ importSection({ { Daemon, "log", functionImport() },
                                             { Daemon, "summary-set", functionImport() },
                                             { Wasi, "fd_write", functionImport() } }) }),
                     {} });
    rows.push_back({ "no manifest section",
                     vtextension::testing::toModule(vtextension::testing::DaemonProbeModule),
                     { std::format(NotOne, 0) } });
    rows.push_back({ "two manifest sections",
                     vtextension::testing::withManifest(guest({}), vtextension::testing::DaemonProbeManifest),
                     { std::format(NotOne, 2) } });
    rows.push_back({ "a start section",
                     guest({ { .id = wasm::binary::StartSectionId, .content = { std::byte { 0 } } } }),
                     { "a start section" } });
    rows.push_back({ "a __wasm_call_ctors export",
                     guest({ exportSection("__wasm_call_ctors") }),
                     { "exports __wasm_call_ctors" } });
    rows.push_back({ "an import of env.abort",
                     guest({ importSection({ { "env", "abort", functionImport() } }) }),
                     { "an import outside the daemon world: env.abort" } });
    rows.push_back({ "a WASI function outside the allow-list",
                     guest({ importSection({ { Wasi, "poll_oneoff", functionImport() } }) }),
                     { "an import outside the daemon world: wasi_snapshot_preview1.poll_oneoff" } });
    rows.push_back(
        { "a function the daemon world does not have",
          guest({ importSection({ { Daemon, "render", functionImport() } }) }),
          { "an import outside the daemon world: terminal-extensions:daemon/host@0.1.0.render" } });
    // A memory import (kind 2, limits with a maximum) is read past, so that the import after it is checked.
    rows.push_back({ "a memory import, then a daemon import",
                     guest({ importSection(
                         { { "env",
                             "memory",
                             { std::byte { 0x02 }, std::byte { 0x01 }, std::byte { 1 }, std::byte { 2 } } },
                           { Daemon, "now", functionImport() } }) }),
                     { "an import outside the daemon world: env.memory" } });
    rows.push_back({ "every problem at once, in section order",
                     assemble({ importSection({ { "env", "abort", functionImport() } }),
                                exportSection("__wasm_call_ctors"),
                                { .id = wasm::binary::StartSectionId, .content = { std::byte { 0 } } } }),
                     { "an import outside the daemon world: env.abort",
                       "exports __wasm_call_ctors",
                       "a start section",
                       std::format(NotOne, 0) } });
    rows.push_back({ "a truncated import section",
                     guest({ { .id = 2, .content = { std::byte { 1 } } } }),
                     { "a section at byte 10 that cannot be read" } });
    rows.push_back({ "not a module",
                     { std::byte { 'w' }, std::byte { 'a' } },
                     { "not a readable WebAssembly module" } });
    return rows;
}

} // namespace

TEST_CASE("DaemonGuestModule: the module check names each problem", "[daemon][guest]")
{
    for (auto const& row: moduleCheckRows())
    {
        CAPTURE(row.name);
        CHECK(daemonGuestModuleProblems(row.module) == row.problems);
    }
}

#ifdef VTEXTENSION_CXX_PROBE_MODULE
TEST_CASE("DaemonGuestModule: the C++ probe is a well-formed daemon module", "[daemon][guest]")
{
    auto const module = vtextension::testing::readModuleFile(VTEXTENSION_CXX_PROBE_MODULE);
    CHECK(daemonGuestModuleProblems(module).empty());

    auto const backend = wasm::makeDefaultBackend();
    auto const limits = ExtensionLimits {};
    auto const engine = Engine::create(*backend, World::Daemon, limits);
    REQUIRE(engine.has_value());
    auto const compiled = CompiledExtension::load(**engine, module);
    REQUIRE(compiled.has_value());
    REQUIRE((*compiled)->manifest().has_value());
    CHECK((*compiled)->manifest()->name == "cxx-probe");

    auto services = vtextension::testing::FakeDaemonHostServices {};
    services.clock = ClockReading { .monotonicMs = 1'234'567'890'123, .wallMs = 1 };
    auto const instance = DaemonInstance::create(*compiled, services, **engine, limits);
    REQUIRE(instance.has_value());

    REQUIRE((*instance)->load("{}", DaemonHostInfo {}).has_value());
    CHECK(services.logged
          == std::vector { vtextension::testing::DaemonLogged { .level = LogLevel::Info,
                                                                .message = "cxx-probe loaded" } });

    REQUIRE((*instance)->update(DaemonHint::Wake).has_value());
    CHECK(services.summary == std::pair { std::string("probe"), std::string("1234567890123") });
    CHECK(services.calls == std::vector { DaemonImport::Log, DaemonImport::Now, DaemonImport::SummarySet });
}
#endif
