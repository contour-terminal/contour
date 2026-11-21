// SPDX-License-Identifier: Apache-2.0
//
// The daemon world: its WIT text against the import table the host serves it with, and its types'
// Canonical ABI layouts and round trips through a fake linear memory.
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <fstream>
#include <iterator>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vtextension/CanonicalAbi.hpp>
#include <vtextension/ControlImports.hpp>
#include <vtextension/DaemonImports.hpp>
#include <vtextension/DaemonWorld.hpp>
#include <vtextension/testing/FakeGuestMemory.hpp>
#include <vtextension/wasm/WasmRuntime.hpp>

using namespace std::string_view_literals;
using namespace vtextension;
namespace abi = vtextension::abi;

namespace
{

#ifdef VTEXTENSION_DAEMON_WIT_DIR
/// @return The contents of the file @p name in the daemon WIT package directory.
[[nodiscard]] std::string readDaemonWitFile(std::string_view name)
{
    auto const path = std::string(VTEXTENSION_DAEMON_WIT_DIR) + "/" + std::string(name);
    auto file = std::ifstream(path, std::ios::binary);
    REQUIRE(file.good());
    return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
}

/// @return The body of the `host` interface: from its opening line to the first brace closing a top-level
/// item.
[[nodiscard]] std::string_view hostInterfaceOf(std::string_view world)
{
    auto const begin = world.find("interface host {");
    REQUIRE(begin != std::string_view::npos);
    auto const end = world.find("\n}", begin);
    REQUIRE(end != std::string_view::npos);
    return world.substr(begin, end - begin);
}

/// @return How often @p needle occurs in @p text.
[[nodiscard]] std::size_t occurrences(std::string_view text, std::string_view needle)
{
    auto count = std::size_t { 0 };
    for (auto at = text.find(needle); at != std::string_view::npos;
         at = text.find(needle, at + needle.size()))
        ++count;
    return count;
}
#endif

/// Stores @p value into a fresh memory and loads it back.
template <typename T>
[[nodiscard]] T roundTrip(T const& value)
{
    auto memory = testing::FakeGuestMemory {};
    auto const address = abi::storeNew(memory, value);
    REQUIRE(address.has_value());
    auto loaded = abi::Codec<T>::load(memory, *address);
    REQUIRE(loaded.has_value());
    return std::move(*loaded);
}

/// A finished process's result, every field away from its default.
[[nodiscard]] ProcResult finishedResult()
{
    return ProcResult { .completion = ProcCompletion::Signalled,
                        .exitCode = std::optional<int32_t> { -9 },
                        .stdoutBytes = 1'048'576,
                        .stdoutExtent = OutputExtent::Truncated,
                        .stderrBytes = 17,
                        .stderrExtent = OutputExtent::Whole };
}

} // namespace

#ifdef VTEXTENSION_DAEMON_WIT_DIR
TEST_CASE("DaemonWit: declares the package and the world", "[daemon][wit]")
{
    auto const world = readDaemonWitFile("world.wit");
    CHECK(world.contains("package terminal-extensions:daemon@0.1.0;"));
    CHECK(world.contains("world daemon {"));
    CHECK(world.contains("export load: func(config-json: string, info: host-info);"));
    CHECK(world.contains("export update: func(ev: hint);"));
}

TEST_CASE("DaemonWit: every import row is declared verbatim, and nothing else", "[daemon][wit]")
{
    auto const world = readDaemonWitFile("world.wit");
    auto const host = hostInterfaceOf(world);
    for (auto const& row: DaemonImportRows)
    {
        CAPTURE(row.name);
        CHECK(host.contains(row.witSignature));
        CHECK(row.witSignature.starts_with(std::string(row.name) + ": func("));
    }
    CHECK(occurrences(host, ": func(") == DaemonImportRows.size());
    CHECK(DaemonImportRows.size() == 24);
}
#endif

TEST_CASE("DaemonImports: rows are indexed by their enumerator", "[daemon][wit]")
{
    STATIC_CHECK(DaemonImportRows.size() == std::to_underlying(DaemonImport::FileRead) + 1U);
    STATIC_CHECK(std::ranges::all_of(
        std::views::iota(std::size_t { 0 }, DaemonImportRows.size()),
        [](std::size_t index) { return std::to_underlying(DaemonImportRows.at(index).import) == index; }));
}

// The selected backend's own caps (WAMR's 32 functions per runtime) are checked by its own tests, which alone
// may include its headers: "DaemonImports: the table stays within WAMR's caps".
TEST_CASE("DaemonImports: the table stays within the backend's caps", "[daemon][wit]")
{
    for (auto const& row: DaemonImportRows)
    {
        CAPTURE(row.name);
        // The Canonical ABI's flat parameters, and the address of a result that does not flatten to one
        // value.
        CHECK(row.parameterCount <= MaxFlatParameters + 1);
    }

    auto const functions = daemonHostFunctions();
    REQUIRE(functions.size() == DaemonImportRows.size());
    for (auto const index: std::views::iota(std::size_t { 0 }, functions.size()))
    {
        auto const& function = functions[index];
        auto const& row = DaemonImportRows.at(index);
        CAPTURE(row.name);
        CHECK(function.module == DaemonImportModule);
        CHECK(function.name == row.name);
        CHECK(function.parameterCount == row.parameterCount);
    }
}

TEST_CASE("DaemonImports: these imports are served and no others", "[daemon][wit]")
{
    static constexpr auto Served = std::array {
        DaemonImport::Log,         DaemonImport::Wake,       DaemonImport::Now,
        DaemonImport::ProcStart,   DaemonImport::Procs,      DaemonImport::ProcTake,
        DaemonImport::ProcRelease, DaemonImport::StoreGet,   DaemonImport::StoreSet,
        DaemonImport::StoreRemove, DaemonImport::StoreKeys,  DaemonImport::PaneOpen,
        DaemonImport::PaneClose,   DaemonImport::Panes,      DaemonImport::TabOpen,
        DaemonImport::TabClose,    DaemonImport::TabReveal,  DaemonImport::Tabs,
        DaemonImport::TabRead,     DaemonImport::TabWrite,   DaemonImport::ActionsTake,
        DaemonImport::SummarySet,  DaemonImport::PaneReveal, DaemonImport::FileRead,
    };
    for (auto const& row: DaemonImportRows)
    {
        CAPTURE(row.name);
        CHECK((row.availability == ImportAvailability::Served)
              == (std::ranges::find(Served, row.import) != Served.end()));
    }
}

TEST_CASE("DaemonWorld: layouts follow the Canonical ABI", "[daemon][wit]")
{
    STATIC_CHECK(abi::Codec<ClockReading>::Size == 16);
    STATIC_CHECK(abi::Codec<ClockReading>::Alignment == 8);
    STATIC_CHECK(abi::Codec<Chunk>::Size == 12);
    STATIC_CHECK(abi::Codec<Chunk>::Alignment == 4);
    STATIC_CHECK(abi::Codec<ProcResult>::Size == 28);
    STATIC_CHECK(abi::Codec<ProcResult>::Alignment == 4);
    STATIC_CHECK(abi::Codec<ProcState>::Size == 32);
    STATIC_CHECK(abi::Codec<ProcState>::Alignment == 4);
    STATIC_CHECK(abi::Codec<ProcInfo>::Size == 52);
    STATIC_CHECK(abi::Codec<ProcInfo>::Alignment == 4);
    STATIC_CHECK(abi::Codec<PaneInfo>::Size == 32);
    STATIC_CHECK(abi::Codec<PaneInfo>::Alignment == 4);
    STATIC_CHECK(abi::Codec<TabInfo>::Size == 24);
    STATIC_CHECK(abi::Codec<TabInfo>::Alignment == 4);
    STATIC_CHECK(abi::Codec<ActionInfo>::Size == 12);
    STATIC_CHECK(abi::Codec<ActionInfo>::Alignment == 4);
    STATIC_CHECK(abi::Codec<FileContent>::Size == 16);
    STATIC_CHECK(abi::Codec<FileContent>::Alignment == 8);
    STATIC_CHECK(abi::Codec<ResolvedRoot>::Size == 16);
    STATIC_CHECK(abi::Codec<ResolvedRoot>::Alignment == 4);
    STATIC_CHECK(abi::Codec<SelfProgram>::Size == 16);
    STATIC_CHECK(abi::Codec<SelfProgram>::Alignment == 4);
    STATIC_CHECK(abi::Codec<DaemonHostInfo>::Size == 28);
    STATIC_CHECK(abi::Codec<DaemonHostInfo>::Alignment == 4);
    STATIC_CHECK(abi::Codec<std::expected<uint32_t, HostError>>::Size == 8);
    STATIC_CHECK(abi::Codec<std::expected<uint32_t, HostError>>::Alignment == 4);
    STATIC_CHECK(abi::Codec<std::expected<Chunk, HostError>>::Size == 16);
    STATIC_CHECK(abi::Codec<std::expected<Chunk, HostError>>::Alignment == 4);
    STATIC_CHECK(abi::Codec<std::expected<FileContent, FileError>>::Size == 24);
    STATIC_CHECK(abi::Codec<std::expected<FileContent, FileError>>::Alignment == 8);
    STATIC_CHECK(abi::Codec<std::expected<void, HostError>>::Size == 2);
    STATIC_CHECK(abi::Codec<std::expected<void, HostError>>::Alignment == 1);
}

TEST_CASE("DaemonWorld: every record round-trips through guest memory", "[daemon][wit]")
{
    auto const clock = ClockReading { .monotonicMs = 0x1'0000'0002ULL, .wallMs = 1'763'200'000'123ULL };
    CHECK(roundTrip(clock) == clock);

    auto const chunk = Chunk { .bytes = { 0, 1, 0xFF }, .remaining = 65'536 };
    CHECK(roundTrip(chunk) == chunk);

    CHECK(roundTrip(finishedResult()) == finishedResult());

    auto const finished = ProcState { finishedResult() };
    CHECK(roundTrip(finished) == finished);
    auto const running = ProcState { ProcRunning {} };
    CHECK(roundTrip(running) == running);

    auto const info = ProcInfo { .handle = 7, .tag = "poll:prs", .command = "gh-prs", .state = finished };
    CHECK(roundTrip(info) == info);

    auto const withStatus = PaneInfo { .pane = "pane-1",
                                       .tag = "run:42",
                                       .status = std::optional<AgentStatus> { AgentStatus::Idle },
                                       .message = "Waiting for Build pace",
                                       .program = PaneProgram::Gone };
    CHECK(roundTrip(withStatus) == withStatus);
    auto const withoutStatus = PaneInfo { .pane = "pane-2",
                                          .tag = "run:43",
                                          .status = std::nullopt,
                                          .message = {},
                                          .program = PaneProgram::Alive };
    CHECK(roundTrip(withoutStatus) == withoutStatus);

    auto const tab = TabInfo { .tab = "tab-1", .scheme = "orchestrator", .cols = 120, .rows = 40 };
    CHECK(roundTrip(tab) == tab);

    auto const action = ActionInfo { .id = "open-dashboard", .origin = 3 };
    CHECK(roundTrip(action) == action);

    auto const file = FileContent { .bytes = { 'o', 'k' }, .modifiedUnixMs = 1'763'200'000'000ULL };
    CHECK(roundTrip(file) == file);

    auto const root = ResolvedRoot { .label = "repos", .paths = { "/home/u/src/a", "/home/u/src/b" } };
    CHECK(roundTrip(root) == root);

    auto const hostInfo = DaemonHostInfo {
        .roots = { root, ResolvedRoot { .label = "worktrees", .paths = {} } },
        .home = "/home/u",
        .utcOffsetMinutes = -330,
        .selfPrograms = { SelfProgram { .name = "contour", .path = "/opt/contour/bin/contour" } }
    };
    CHECK(roundTrip(hostInfo) == hostInfo);

    CHECK(roundTrip(std::expected<uint32_t, HostError> { 9U }) == std::expected<uint32_t, HostError> { 9U });
    CHECK(roundTrip(std::expected<uint32_t, HostError> { std::unexpected(HostError::NotSupportedYet) })
          == std::expected<uint32_t, HostError> { std::unexpected(HostError::NotSupportedYet) });
    CHECK(roundTrip(std::expected<uint32_t, HostError> { std::unexpected(HostError::ProgramMissing) })
          == std::expected<uint32_t, HostError> { std::unexpected(HostError::ProgramMissing) });
    CHECK(roundTrip(std::expected<Chunk, HostError> { chunk }) == std::expected<Chunk, HostError> { chunk });
    CHECK(roundTrip(std::expected<FileContent, FileError> { std::unexpected(FileError::Escapes) })
          == std::expected<FileContent, FileError> { std::unexpected(FileError::Escapes) });
    CHECK(roundTrip(std::expected<void, HostError> {}) == std::expected<void, HostError> {});
    CHECK(roundTrip(std::expected<void, HostError> { std::unexpected(HostError::Gone) })
          == std::expected<void, HostError> { std::unexpected(HostError::Gone) });
}
