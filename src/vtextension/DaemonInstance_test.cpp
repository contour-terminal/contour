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

/// Every import DaemonImportRows declares NotSupportedYet, with its WIT result.
constexpr auto UnservedRows = std::array {
    UnservedRow { DaemonImport::PaneOpen, &holdsNotSupportedYet<std::expected<std::string, HostError>> },
    UnservedRow { DaemonImport::PaneClose, &holdsNotSupportedYet<HostResult> },
    UnservedRow { DaemonImport::Panes,
                  &holdsNotSupportedYet<std::expected<std::vector<PaneInfo>, HostError>> },
    UnservedRow { DaemonImport::TabOpen, &holdsNotSupportedYet<std::expected<std::string, HostError>> },
    UnservedRow { DaemonImport::TabClose, &holdsNotSupportedYet<HostResult> },
    UnservedRow { DaemonImport::TabReveal, &holdsNotSupportedYet<HostResult> },
    UnservedRow { DaemonImport::Tabs, &holdsNotSupportedYet<std::expected<std::vector<TabInfo>, HostError>> },
    UnservedRow { DaemonImport::TabRead, &holdsNotSupportedYet<std::expected<ByteList, HostError>> },
    UnservedRow { DaemonImport::TabWrite, &holdsNotSupportedYet<HostResult> },
    UnservedRow { DaemonImport::ActionsTake,
                  &holdsNotSupportedYet<std::expected<std::vector<ActionInfo>, HostError>> },
    UnservedRow { DaemonImport::PaneReveal, &holdsNotSupportedYet<HostResult> },
    UnservedRow { DaemonImport::FileRead, &holdsNotSupportedYet<std::expected<FileContent, FileError>> },
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
