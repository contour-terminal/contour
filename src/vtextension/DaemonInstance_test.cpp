// SPDX-License-Identifier: Apache-2.0
//
// The daemon world's instance, driven step by step through the fake backend: what `load` hands the guest,
// the imports this host does not serve yet, the call deadline, and the deadline's pause across durable
// host calls.
#include <core/platform/Clock.hpp>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <vtextension/CanonicalAbi.hpp>
#include <vtextension/CompiledExtension.hpp>
#include <vtextension/DaemonImports.hpp>
#include <vtextension/DaemonInstance.hpp>
#include <vtextension/DaemonLimits.hpp>
#include <vtextension/DaemonWorld.hpp>
#include <vtextension/DeadlineGuard.hpp>
#include <vtextension/Engine.hpp>
#include <vtextension/ExtensionError.hpp>
#include <vtextension/ExtensionLimits.hpp>
#include <vtextension/World.hpp>
#include <vtextension/testing/FakeDaemonHostServices.hpp>
#include <vtextension/testing/FakeWasmBackend.hpp>
#include <vtextension/testing/TestModules.hpp>

using namespace std::string_view_literals;
using namespace vtextension;
using vtextension::testing::FakeDaemonHostServices;
using vtextension::testing::FakeInstanceMemory;
using vtextension::testing::FakeWasmBackend;

namespace
{

/// A guest on the fake backend, loaded on a daemon engine: its `cabi_realloc` is a bump allocator from
/// 1024, and its `update` runs what the test sets.
struct FakeDaemonGuest
{
    explicit FakeDaemonGuest(ExtensionLimits limits = {}): limits { limits }
    {
        backend.guest.exports["cabi_realloc"] =
            [this](std::span<uint32_t> argv) -> std::expected<void, wasm::CallFailure> {
            next = abi::alignTo(next, argv[2]);
            argv[0] = next;
            next += argv[3];
            return {};
        };
        backend.guest.exports["update"] =
            [this](std::span<uint32_t> argv) -> std::expected<void, wasm::CallFailure> {
            return onUpdate ? onUpdate(argv) : std::expected<void, wasm::CallFailure> {};
        };
    }

    /// Loads the probe on a fresh daemon engine and instantiates it, deadlines enforced by @p watchdog
    /// (the engine if null).
    [[nodiscard]] std::expected<std::unique_ptr<DaemonInstance>, ExtensionError> start(
        IDeadlineWatchdog* watchdog = nullptr)
    {
        auto created = Engine::create(backend, World::Daemon, limits);
        REQUIRE(created.has_value());
        engine = *created;
        auto compiled = CompiledExtension::load(*engine, vtextension::testing::daemonProbe());
        REQUIRE(compiled.has_value());
        return DaemonInstance::create(*compiled, services, clock, watchdog ? *watchdog : *engine, limits);
    }

    /// @return The instance the backend created last.
    [[nodiscard]] vtextension::testing::FakeWasmInstance& guest() const
    {
        REQUIRE(backend.lastInstance != nullptr);
        return *backend.lastInstance;
    }

    /// Writes @p text into the guest's memory at @p address.
    void put(uint32_t address, std::string_view text) const
    {
        std::ranges::transform(
            text, guest().linearMemory.begin() + address, [](char c) { return static_cast<std::byte>(c); });
    }

    /// Calls the import @p import from inside the guest, as the guest would.
    [[nodiscard]] std::expected<void, ExtensionError> call(DaemonImport import,
                                                           std::vector<uint32_t> arguments) const
    {
        return guest().callHost(std::to_underlying(import), arguments);
    }

    FakeWasmBackend backend;
    ExtensionLimits limits;
    FakeDaemonHostServices services;
    core::platform::ManualClock clock;
    std::shared_ptr<Engine> engine;
    std::function<std::expected<void, wasm::CallFailure>(std::span<uint32_t>)> onUpdate;
    uint32_t next = 1024; ///< The bump allocator's next address.
};

/// @return Whether @p memory holds `err(not-supported-yet)` of the result type @p Result at @p address.
template <typename Result>
[[nodiscard]] bool holdsNotSupportedYet(abi::IGuestMemory& memory, abi::Address address)
{
    auto const loaded = abi::Codec<Result>::load(memory, address);
    return loaded && !*loaded && loaded->error() == Result::error_type::NotSupportedYet;
}

/// An import not served yet, and how to read its answer.
struct UnservedRow
{
    DaemonImport import;
    bool (*holdsNotSupportedYet)(abi::IGuestMemory&, abi::Address);
};

using HostResult = std::expected<void, HostError>;

/// Every import DaemonImportRows declares NotSupportedYet, with its WIT result (holdsNotSupportedYet<it>):
/// none, as this host serves every import.
constexpr auto UnservedRows = std::array<UnservedRow, 0> {};

/// The imports this host serves for served tabs and invoked actions.
constexpr auto TabAndActionImports = std::array {
    DaemonImport::TabOpen, DaemonImport::TabClose, DaemonImport::TabReveal,   DaemonImport::Tabs,
    DaemonImport::TabRead, DaemonImport::TabWrite, DaemonImport::ActionsTake,
};

/// Records what is armed and disarmed, in one log with the service calls the test adds to it.
class RecordingWatchdog final: public IDeadlineWatchdog
{
  public:
    explicit RecordingWatchdog(std::vector<std::string>& events) noexcept: _events { events } {}

    void armDeadline(WatchToken /*token*/, std::chrono::milliseconds timeout) override
    {
        _events.push_back(std::format("arm {}", timeout.count()));
    }

    void disarm(WatchToken /*token*/) override { _events.emplace_back("disarm"); }

  private:
    std::vector<std::string>& _events;
};

} // namespace

TEST_CASE("DaemonInstance: load hands the guest its configuration and the host's information",
          "[daemon][instance]")
{
    auto guest = FakeDaemonGuest {};
    auto configJson = std::optional<std::string> {};
    auto info = std::optional<DaemonHostInfo> {};
    guest.backend.guest.exports["load"] =
        [&](std::span<uint32_t> argv) -> std::expected<void, wasm::CallFailure> {
        auto memory = FakeInstanceMemory { guest.guest() };
        auto reader = abi::FlatReader(argv.first(7));
        auto const config = abi::Codec<std::string>::liftFlat(memory, reader);
        auto const host = abi::Codec<DaemonHostInfo>::liftFlat(memory, reader);
        if (!config || !host)
            return std::unexpected(wasm::CallFailure::Trap);
        configJson = *config;
        info = *host;
        return {};
    };
    auto instance = guest.start();
    REQUIRE(instance.has_value());

    auto const expected = DaemonHostInfo {
        .roots = { ResolvedRoot { .label = "repos", .paths = { "/src/a", "/src/b" } },
                   ResolvedRoot { .label = "worktrees", .paths = { "/wt" } } },
        .home = "/home/me",
        .utcOffsetMinutes = -330,
    };
    REQUIRE((*instance)->load(R"({"poll_every":"2m"})", expected).has_value());
    CHECK(configJson == R"({"poll_every":"2m"})");
    CHECK(info == expected);
}

TEST_CASE("DaemonInstance: an import not served yet answers not-supported-yet and calls no service",
          "[daemon][instance]")
{
    // The table and this list agree: every NotSupportedYet row has a row here.
    auto unserved = DaemonImportRows | std::views::filter([](DaemonImportRow const& row) {
                        return row.availability == ImportAvailability::NotSupportedYet;
                    });
    CHECK(std::ranges::distance(unserved) == static_cast<std::ptrdiff_t>(UnservedRows.size()));
    for (auto const& row: unserved)
        CHECK(std::ranges::contains(UnservedRows, row.import, &UnservedRow::import));

    constexpr auto ResultAddress = uint32_t { 2048 };
    auto guest = FakeDaemonGuest {};
    auto instance = guest.start();
    REQUIRE(instance.has_value());
    for (auto const& row: UnservedRows)
    {
        auto const& declared = DaemonImportRows.at(std::to_underlying(row.import));
        CAPTURE(declared.name);
        auto answered = std::expected<void, ExtensionError> {};
        guest.onUpdate = [&](std::span<uint32_t>) -> std::expected<void, wasm::CallFailure> {
            std::ranges::fill(std::span { guest.guest().linearMemory }.subspan(ResultAddress, 64),
                              std::byte { 0xAA });
            auto arguments = std::vector<uint32_t>(declared.parameterCount, 0);
            arguments.back() = ResultAddress;
            answered = guest.call(row.import, arguments);
            return {};
        };
        REQUIRE((*instance)->update(DaemonHint::Wake).has_value());
        CHECK(answered.has_value());
        auto memory = FakeInstanceMemory { guest.guest() };
        CHECK(row.holdsNotSupportedYet(memory, ResultAddress));
        CHECK_FALSE((*instance)->fault().has_value());
    }
    CHECK(guest.services.calls.empty());
}

TEST_CASE("DaemonInstance: an update that spins past its deadline fails as overrun", "[daemon][instance]")
{
    auto limits = ExtensionLimits {};
    limits.callDeadline = std::chrono::milliseconds { 20 };
    auto guest = FakeDaemonGuest { limits };
    auto instance = guest.start();
    REQUIRE(instance.has_value());
    // The guest spins until the watchdog interrupts it (bounded, so that a watchdog that never fires fails
    // the checks rather than hanging the test).
    guest.onUpdate = [&](std::span<uint32_t>) -> std::expected<void, wasm::CallFailure> {
        auto const giveUp = std::chrono::steady_clock::now() + std::chrono::seconds { 5 };
        while (guest.guest().interrupts.load() == 0 && std::chrono::steady_clock::now() < giveUp)
            std::this_thread::sleep_for(std::chrono::milliseconds { 1 });
        return std::unexpected(wasm::CallFailure::Interrupted);
    };

    auto const result = (*instance)->update(DaemonHint::Wake);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ExtensionError::DeadlineExceeded);
    CHECK((*instance)->fault() == ExtensionError::DeadlineExceeded);
    // Faults are final: the next call fails the same way without entering the guest.
    auto const again = (*instance)->update(DaemonHint::Wake);
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error() == ExtensionError::DeadlineExceeded);
}

TEST_CASE("DaemonInstance: a durable host call pauses the deadline", "[daemon][instance]")
{
    auto events = std::vector<std::string> {};
    auto watchdog = RecordingWatchdog { events };
    auto guest = FakeDaemonGuest {};
    guest.services.onCall = [&](DaemonImport import) {
        events.emplace_back(DaemonImportRows.at(std::to_underlying(import)).name);
    };
    auto instance = guest.start(&watchdog);
    REQUIRE(instance.has_value());

    guest.onUpdate = [&](std::span<uint32_t>) -> std::expected<void, wasm::CallFailure> {
        guest.put(100, "key");
        guest.put(110, "value");
        guest.put(120, "loaded");
        auto const calls = std::array {
            guest.call(DaemonImport::StoreSet, { 100, 3, 110, 5, 2048 }),
            guest.call(DaemonImport::StoreRemove, { 100, 3, 2048 }),
            guest.call(DaemonImport::ProcStart, { 0, 0, 0, 0, 0, 0, 0, 0, 2048 }),
            guest.call(DaemonImport::Log, { 2, 120, 6 }),
        };
        return std::ranges::all_of(calls, [](auto const& call) { return call.has_value(); })
                   ? std::expected<void, wasm::CallFailure> {}
                   : std::unexpected(wasm::CallFailure::HostFailure);
    };
    REQUIRE((*instance)->update(DaemonHint::Procs).has_value());

    auto const deadline = std::format("arm {}", guest.limits.callDeadline.count());
    CHECK(events
          == std::vector<std::string> { deadline,
                                        "disarm",
                                        "store-set",
                                        deadline,
                                        "disarm",
                                        "store-remove",
                                        deadline,
                                        "proc-start",
                                        "log",
                                        "disarm" });
    REQUIRE(guest.services.storeWrites.size() == 1);
    CHECK(guest.services.storeWrites.front().first == "key");
    CHECK(guest.services.storeWrites.front().second == ByteList { 'v', 'a', 'l', 'u', 'e' });
    CHECK(guest.services.storeRemovals == std::vector<std::string> { "key" });
}

TEST_CASE("DaemonInstance: a durable host call that throws traps the guest and resumes the deadline",
          "[daemon][instance]")
{
    auto events = std::vector<std::string> {};
    auto watchdog = RecordingWatchdog { events };
    auto guest = FakeDaemonGuest {};
    guest.services.onCall = [&](DaemonImport import) {
        events.emplace_back(DaemonImportRows.at(std::to_underlying(import)).name);
        throw std::runtime_error("the disk went away");
    };
    auto instance = guest.start(&watchdog);
    REQUIRE(instance.has_value());

    auto storeSet = std::expected<void, ExtensionError> {};
    guest.onUpdate = [&](std::span<uint32_t>) -> std::expected<void, wasm::CallFailure> {
        guest.put(100, "key");
        guest.put(110, "value");
        storeSet = guest.call(DaemonImport::StoreSet, { 100, 3, 110, 5, 2048 });
        return storeSet ? std::expected<void, wasm::CallFailure> {}
                        : std::unexpected(wasm::CallFailure::HostFailure);
    };
    auto const result = (*instance)->update(DaemonHint::Procs);

    REQUIRE_FALSE(storeSet.has_value());
    CHECK(storeSet.error() == ExtensionError::Trapped);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ExtensionError::Trapped);
    CHECK((*instance)->fault() == ExtensionError::Trapped);
    // The pause ends on the throwing path as on the returning one: the deadline is armed again with what
    // was left, and the end of the guarded call disarms it, so no deadline outlives the call.
    auto const deadline = std::format("arm {}", guest.limits.callDeadline.count());
    CHECK(events == std::vector<std::string> { deadline, "disarm", "store-set", deadline, "disarm" });
    CHECK(guest.services.storeWrites.empty());
}

TEST_CASE("DaemonInstance: the tab and action imports are served", "[daemon][instance]")
{
    for (auto const import: TabAndActionImports)
    {
        CAPTURE(DaemonImportRows.at(std::to_underlying(import)).name);
        CHECK(DaemonImportRows.at(std::to_underlying(import)).availability == ImportAvailability::Served);
    }
    // Every row is served exactly when its handler serves it (a static_assert in DaemonInstance.cpp); all 24
    // are.
    CHECK(std::ranges::count(DaemonImportRows, ImportAvailability::Served, &DaemonImportRow::availability)
          == static_cast<std::ptrdiff_t>(DaemonImportRows.size() - UnservedRows.size()));

    constexpr auto ResultAddress = uint32_t { 2048 };
    constexpr auto TabAddress = uint32_t { 100 };
    constexpr auto BytesAddress = uint32_t { 200 };
    auto guest = FakeDaemonGuest {};
    guest.services.servedTabs = { TabInfo { .tab = "tab-1", .scheme = "agents", .cols = 80, .rows = 24 } };
    guest.services.tabInput = { 'j', 'k' };
    guest.services.actions = { ActionInfo { .id = "open", .origin = 7 } };
    auto instance = guest.start();
    REQUIRE(instance.has_value());

    auto opened = std::optional<std::expected<std::string, HostError>> {};
    auto listed = std::optional<std::expected<std::vector<TabInfo>, HostError>> {};
    auto read = std::optional<std::expected<ByteList, HostError>> {};
    auto taken = std::optional<std::expected<std::vector<ActionInfo>, HostError>> {};
    guest.onUpdate = [&](std::span<uint32_t>) -> std::expected<void, wasm::CallFailure> {
        auto memory = FakeInstanceMemory { guest.guest() };
        guest.put(TabAddress, "agents");
        REQUIRE(guest.call(DaemonImport::TabOpen, { TabAddress, 6, 7, ResultAddress }).has_value());
        opened = *abi::Codec<std::expected<std::string, HostError>>::load(memory, ResultAddress);
        guest.put(TabAddress, "tab-1");
        guest.put(BytesAddress, "drawn");
        REQUIRE(guest.call(DaemonImport::TabWrite, { TabAddress, 5, BytesAddress, 5, ResultAddress })
                    .has_value());
        REQUIRE(guest.call(DaemonImport::TabReveal, { TabAddress, 5, 7, ResultAddress }).has_value());
        REQUIRE(guest.call(DaemonImport::Tabs, { ResultAddress }).has_value());
        listed = *abi::Codec<std::expected<std::vector<TabInfo>, HostError>>::load(memory, ResultAddress);
        REQUIRE(guest.call(DaemonImport::TabRead, { TabAddress, 5, ResultAddress }).has_value());
        read = *abi::Codec<std::expected<ByteList, HostError>>::load(memory, ResultAddress);
        REQUIRE(guest.call(DaemonImport::ActionsTake, { ResultAddress }).has_value());
        taken = *abi::Codec<std::expected<std::vector<ActionInfo>, HostError>>::load(memory, ResultAddress);
        REQUIRE(guest.call(DaemonImport::TabClose, { TabAddress, 5, ResultAddress }).has_value());
        return {};
    };
    REQUIRE((*instance)->update(DaemonHint::Actions).has_value());

    CHECK(guest.services.calls
          == std::vector { DaemonImport::TabOpen,
                           DaemonImport::TabWrite,
                           DaemonImport::TabReveal,
                           DaemonImport::Tabs,
                           DaemonImport::TabRead,
                           DaemonImport::ActionsTake,
                           DaemonImport::TabClose });
    CHECK(guest.services.tabOpens == std::vector { std::pair { std::string { "agents" }, Origin { 7 } } });
    CHECK(opened == std::expected<std::string, HostError> { "tab-1" });
    REQUIRE(guest.services.tabWrites.size() == 1);
    CHECK(guest.services.tabWrites.front().first == "tab-1");
    CHECK(guest.services.tabWrites.front().second == ByteList { 'd', 'r', 'a', 'w', 'n' });
    CHECK(guest.services.tabReveals == std::vector { std::pair { std::string { "tab-1" }, Origin { 7 } } });
    CHECK(listed == std::expected<std::vector<TabInfo>, HostError> { guest.services.servedTabs });
    CHECK(read == std::expected<ByteList, HostError> { ByteList { 'j', 'k' } });
    CHECK(taken
          == std::expected<std::vector<ActionInfo>, HostError> {
              std::vector { ActionInfo { .id = "open", .origin = 7 } } });
    CHECK(guest.services.tabCloses == std::vector<std::string> { "tab-1" });
}

TEST_CASE("pane-open, pane-close, panes and pane-reveal are served", "[daemon][instance][panes]")
{
    for (auto const import:
         { DaemonImport::PaneOpen, DaemonImport::PaneClose, DaemonImport::Panes, DaemonImport::PaneReveal })
    {
        CAPTURE(DaemonImportRows.at(std::to_underlying(import)).name);
        CHECK(DaemonImportRows.at(std::to_underlying(import)).availability == ImportAvailability::Served);
    }

    constexpr auto ResultAddress = uint32_t { 2048 };
    constexpr auto TemplateAddress = uint32_t { 100 };
    constexpr auto CwdAddress = uint32_t { 200 };
    constexpr auto TitleAddress = uint32_t { 300 };
    constexpr auto TagAddress = uint32_t { 400 };
    constexpr auto NearAddress = uint32_t { 500 };
    constexpr auto PaneAddress = uint32_t { 600 };
    using OpenResult = std::expected<std::string, HostError>;
    using ListResult = std::expected<std::vector<PaneInfo>, HostError>;
    auto guest = FakeDaemonGuest {};
    guest.services.paneId = std::string { "0e1f2a3b-4c5d-4e6f-8a9b-0c1d2e3f4a5b" };
    guest.services.listedPanes =
        std::vector<PaneInfo> { PaneInfo { .pane = "0e1f2a3b-4c5d-4e6f-8a9b-0c1d2e3f4a5b",
                                           .tag = "run-1",
                                           .status = AgentStatus::Blocked,
                                           .program = PaneProgram::Alive } };
    guest.services.paneAnswer = std::unexpected(HostError::NotOwned);
    auto instance = guest.start();
    REQUIRE(instance.has_value());

    auto opened = std::optional<OpenResult> {};
    auto openedFar = std::optional<OpenResult> {};
    auto closed = std::optional<HostResult> {};
    auto listed = std::optional<ListResult> {};
    auto revealed = std::optional<HostResult> {};
    guest.onUpdate = [&](std::span<uint32_t>) -> std::expected<void, wasm::CallFailure> {
        auto memory = FakeInstanceMemory { guest.guest() };
        guest.put(TemplateAddress, "agent");
        guest.put(CwdAddress, "/w/run-1");
        guest.put(TitleAddress, "run-1");
        guest.put(TagAddress, "run-1");
        guest.put(NearAddress, "tab-7");
        // template, args (an empty list), cwd, title, tag, near = some("tab-7"), the result.
        REQUIRE(guest
                    .call(DaemonImport::PaneOpen,
                          { TemplateAddress,
                            5,
                            0,
                            0,
                            CwdAddress,
                            8,
                            TitleAddress,
                            5,
                            TagAddress,
                            5,
                            1,
                            NearAddress,
                            5,
                            ResultAddress })
                    .has_value());
        opened = *abi::Codec<OpenResult>::load(memory, ResultAddress);
        // near = none.
        REQUIRE(guest
                    .call(DaemonImport::PaneOpen,
                          { TemplateAddress,
                            5,
                            0,
                            0,
                            CwdAddress,
                            8,
                            TitleAddress,
                            5,
                            TagAddress,
                            5,
                            0,
                            0,
                            0,
                            ResultAddress })
                    .has_value());
        openedFar = *abi::Codec<OpenResult>::load(memory, ResultAddress);
        guest.put(PaneAddress, "pane-9");
        REQUIRE(guest.call(DaemonImport::PaneClose, { PaneAddress, 6, ResultAddress }).has_value());
        closed = *abi::Codec<HostResult>::load(memory, ResultAddress);
        REQUIRE(guest.call(DaemonImport::Panes, { ResultAddress }).has_value());
        listed = *abi::Codec<ListResult>::load(memory, ResultAddress);
        // pane, origin 7, the result.
        REQUIRE(guest.call(DaemonImport::PaneReveal, { PaneAddress, 6, 7, ResultAddress }).has_value());
        revealed = *abi::Codec<HostResult>::load(memory, ResultAddress);
        return {};
    };
    REQUIRE((*instance)->update(DaemonHint::Panes).has_value());

    CHECK(guest.services.calls
          == std::vector { DaemonImport::PaneOpen,
                           DaemonImport::PaneOpen,
                           DaemonImport::PaneClose,
                           DaemonImport::Panes,
                           DaemonImport::PaneReveal });
    REQUIRE(guest.services.paneOpens.size() == 2);
    auto const& open = guest.services.paneOpens.front();
    CHECK(open.paneTemplate == "agent");
    CHECK(open.args.empty());
    CHECK(open.cwd == "/w/run-1");
    CHECK(open.title == "run-1");
    CHECK(open.tag == "run-1");
    CHECK(open.near == "tab-7");
    CHECK_FALSE(guest.services.paneOpens.back().near.has_value());
    CHECK(opened == OpenResult { "0e1f2a3b-4c5d-4e6f-8a9b-0c1d2e3f4a5b" });
    // Not decomposed: clang over MSVC's STL recurses into std::expected's operator== with Catch's
    // expression on one side.
    CHECK((openedFar == opened));
    CHECK(guest.services.paneCloses == std::vector<std::string> { "pane-9" });
    CHECK(closed == HostResult { std::unexpect, HostError::NotOwned });
    CHECK(listed == guest.services.listedPanes);
    CHECK(guest.services.paneReveals == std::vector { std::pair { std::string { "pane-9" }, Origin { 7 } } });
    CHECK(revealed == HostResult { std::unexpect, HostError::NotOwned });
}

TEST_CASE("DaemonInstance: file-read is served", "[daemon][instance][file-read]")
{
    CHECK(DaemonImportRows.at(std::to_underlying(DaemonImport::FileRead)).availability
          == ImportAvailability::Served);

    constexpr auto ResultAddress = uint32_t { 2048 };
    constexpr auto RootAddress = uint32_t { 100 };
    constexpr auto PathAddress = uint32_t { 200 };
    using FileResult = std::expected<FileContent, FileError>;
    auto guest = FakeDaemonGuest {};
    guest.services.fileContent =
        FileContent { .bytes = { 'g', 'i', 't' }, .modifiedUnixMs = 1'700'000'000'123U };
    auto instance = guest.start();
    REQUIRE(instance.has_value());

    auto read = std::optional<FileResult> {};
    auto refused = std::optional<FileResult> {};
    guest.onUpdate = [&](std::span<uint32_t>) -> std::expected<void, wasm::CallFailure> {
        auto memory = FakeInstanceMemory { guest.guest() };
        guest.put(RootAddress, "worktrees");
        guest.put(PathAddress, "run-1/.git");
        REQUIRE(guest.call(DaemonImport::FileRead, { RootAddress, 9, PathAddress, 10, ResultAddress })
                    .has_value());
        read = *abi::Codec<FileResult>::load(memory, ResultAddress);
        guest.services.fileContent = std::unexpected(FileError::Escapes);
        REQUIRE(guest.call(DaemonImport::FileRead, { RootAddress, 9, PathAddress, 10, ResultAddress })
                    .has_value());
        refused = *abi::Codec<FileResult>::load(memory, ResultAddress);
        return {};
    };
    REQUIRE((*instance)->update(DaemonHint::Wake).has_value());

    CHECK(guest.services.calls == std::vector { DaemonImport::FileRead, DaemonImport::FileRead });
    CHECK(guest.services.fileReads.front()
          == std::pair { std::string { "worktrees" }, std::string { "run-1/.git" } });
    CHECK(read
          == FileResult { FileContent { .bytes = { 'g', 'i', 't' }, .modifiedUnixMs = 1'700'000'000'123U } });
    CHECK(refused == FileResult { std::unexpect, FileError::Escapes });
    CHECK_FALSE((*instance)->fault().has_value());
}

TEST_CASE("DaemonInstance: a tab-write of one whole input page passes the call budget", "[daemon][instance]")
{
    constexpr auto TabAddress = uint32_t { 100 };
    constexpr auto BytesAddress = uint32_t { 4096 };
    constexpr auto ResultAddress = uint32_t { 2048 };
    auto guest = FakeDaemonGuest { daemonWorldLimits() };
    auto instance = guest.start();
    REQUIRE(instance.has_value());
    // Room for more than the budget, so that only the budget can refuse.
    guest.guest().linearMemory.resize(std::size_t { 256 } * 1024);
    auto const tab = std::string(36, 't');
    guest.put(TabAddress, tab);

    auto const writeOf = [&](uint32_t bytes) {
        guest.onUpdate = [&, bytes](std::span<uint32_t>) -> std::expected<void, wasm::CallFailure> {
            auto const called = guest.call(
                DaemonImport::TabWrite,
                { TabAddress, static_cast<uint32_t>(tab.size()), BytesAddress, bytes, ResultAddress });
            if (!called)
                return std::unexpected(wasm::CallFailure::HostFailure);
            return {};
        };
        return (*instance)->update(DaemonHint::Tabs);
    };

    SECTION("65 536 bytes, beside a tab id, are written")
    {
        CHECK(writeOf(65'536).has_value());
        REQUIRE(guest.services.tabWrites.size() == 1);
        CHECK(guest.services.tabWrites.front().second.size() == 65'536);
    }
    SECTION("65 665 bytes trap")
    {
        CHECK_FALSE(writeOf(65'665).has_value());
        CHECK(guest.services.tabWrites.empty());
        CHECK((*instance)->fault().has_value());
    }
}
