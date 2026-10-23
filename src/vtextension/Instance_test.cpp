// SPDX-License-Identifier: Apache-2.0
//
// The sandboxed instance: what CompiledExtension accepts, the limits an Instance enforces against
// hostile guests, the host calls it routes, and the threads it may be called from.
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <vtextension/CompiledExtension.hpp>
#include <vtextension/Engine.hpp>
#include <vtextension/Instance.hpp>
#include <vtextension/testing/FakeWasmBackend.hpp>
#include <vtextension/testing/Harness.hpp>
#include <vtextension/testing/StandardStreamsCapture.hpp>
#include <vtextension/testing/TestModules.hpp>
#include <vtextension/wasm/DefaultBackend.hpp>
#include <vtextension/wasm/WasmRuntime.hpp>

using namespace std::string_view_literals;
using namespace vtextension;
using vtextension::testing::errorOf;
using vtextension::testing::RunningGuest;
using vtextension::testing::surface;
using vtextension::testing::toModule;

namespace
{

/// Loads @p module on a fresh engine of @p backend and returns how that went.
[[nodiscard]] std::expected<std::shared_ptr<CompiledExtension>, ExtensionError> loadModule(
    wasm::IWasmBackend& backend, std::vector<std::byte> module)
{
    auto const engine = Engine::create(backend, ExtensionLimits {});
    REQUIRE(engine.has_value());
    return CompiledExtension::load(**engine, std::move(module));
}

} // namespace

TEST_CASE("CompiledExtension.load", "[vtextension]")
{
    auto const backend = wasm::makeDefaultBackend();
    auto const compiled = loadModule(*backend, toModule(vtextension::testing::MinimalModule));
    REQUIRE(compiled.has_value());
    // A module without a manifest loads; it declares no commands.
    CHECK_FALSE((*compiled)->manifest().has_value());
}

TEST_CASE("CompiledExtension.invalid", "[vtextension]")
{
    auto const backend = wasm::makeDefaultBackend();
    CHECK(errorOf(loadModule(*backend, {})) == ExtensionError::InvalidComponent);
    auto truncated = toModule(vtextension::testing::MinimalModule);
    truncated.resize(truncated.size() / 2);
    CHECK(errorOf(loadModule(*backend, truncated)) == ExtensionError::InvalidComponent);
}

TEST_CASE("CompiledExtension.worldMismatch", "[vtextension]")
{
    auto const backend = wasm::makeDefaultBackend();
    // The runtime links imports lazily, so each of these would load and fail only when called.
    SECTION("an unknown host function")
    {
        CHECK(errorOf(loadModule(*backend, toModule(vtextension::testing::UnknownImportModule)))
              == ExtensionError::WorldMismatch);
    }
    SECTION("a host function with the wrong signature")
    {
        CHECK(errorOf(loadModule(*backend, toModule(vtextension::testing::WrongImportSignatureModule)))
              == ExtensionError::WorldMismatch);
    }
    SECTION("an import from a module that is neither the host nor WASI")
    {
        CHECK(errorOf(loadModule(*backend, toModule(vtextension::testing::ForeignImportModuleModule)))
              == ExtensionError::WorldMismatch);
    }
    SECTION("a missing export")
    {
        CHECK(errorOf(loadModule(*backend, toModule(vtextension::testing::MissingExportModule)))
              == ExtensionError::WorldMismatch);
    }
    SECTION("a start function, which would run where no deadline guards it")
    {
        CHECK(errorOf(loadModule(*backend, toModule(vtextension::testing::StartFunctionModule)))
              == ExtensionError::WorldMismatch);
    }
    SECTION("an export with the wrong signature")
    {
        CHECK(errorOf(loadModule(*backend, toModule(vtextension::testing::WrongExportSignatureModule)))
              == ExtensionError::WorldMismatch);
    }
}

TEST_CASE("Instance.minimal", "[vtextension]")
{
    auto guest = RunningGuest(toModule(vtextension::testing::MinimalModule));
    CHECK(guest.loadAndRender({}).empty());
    CHECK(guest.instance().update(TimerFired {}).value() == RenderRequest::None);
    CHECK_FALSE(guest.instance().fault().has_value());
}

TEST_CASE("Instance.memoryCap.atInstantiation", "[vtextension]")
{
    // The minimal module asks for one 64 KiB page up front; a cap below that refuses it.
    auto limits = ExtensionLimits {};
    limits.memoryBytes = std::size_t { 32 } * 1024;
    auto const backend = wasm::makeDefaultBackend();
    auto const engine = Engine::create(*backend, limits);
    REQUIRE(engine.has_value());
    auto const compiled = CompiledExtension::load(**engine, toModule(vtextension::testing::MinimalModule));
    REQUIRE(compiled.has_value());
    auto services = vtextension::testing::FakeHostServices {};
    auto clock = core::platform::ManualClock {};
    CHECK(errorOf(Instance::create(**engine, **compiled, services, clock, limits))
          == ExtensionError::OutOfMemory);
}

TEST_CASE("Instance.malicious", "[vtextension]")
{
    // A guest that forges addresses and lengths gets an error, never a host access outside its memory.
    struct Case
    {
        std::string_view what;
        int rows; ///< What the malicious module's render does, by its rows argument.
        ExtensionError error;
    };
    auto const cases = std::to_array<Case>({
        { .what = "a result address outside memory", .rows = 1, .error = ExtensionError::Trapped },
        { .what = "a list outside memory", .rows = 2, .error = ExtensionError::Trapped },
        { .what = "a list longer than any render may be",
          .rows = 3,
          .error = ExtensionError::OutputTooLarge },
        { .what = "a log message outside memory", .rows = 4, .error = ExtensionError::Trapped },
        { .what = "a state result address outside memory", .rows = 5, .error = ExtensionError::Trapped },
        { .what = "a list that runs past the end of memory", .rows = 6, .error = ExtensionError::Trapped },
        { .what = "run-command arguments filling the whole memory",
          .rows = 7,
          .error = ExtensionError::Trapped },
        { .what = "run-command arguments that all alias one large string",
          .rows = 8,
          .error = ExtensionError::Trapped },
    });
    for (auto const& testCase: cases)
    {
        INFO(testCase.what);
        auto guest = RunningGuest(toModule(vtextension::testing::MaliciousModule));
        auto const result = guest.instance().render(surface(80, testCase.rows));
        REQUIRE_FALSE(result.has_value());
        CHECK(errorOf(result) == testCase.error);
        CHECK(guest.services.logged.empty());
        CHECK(guest.services.commands.empty());
        // The instance is done: the next call fails the same way, even one that would succeed.
        CHECK(errorOf(guest.instance().render(surface(80, 0))) == testCase.error);
    }

    auto guest = RunningGuest(toModule(vtextension::testing::MaliciousModule));
    CHECK(guest.instance().render(surface(80, 0)).value() == "ok");
    // Flags bits beyond the world's nine are dropped.
    CHECK(guest.instance().load({}).value() == (AlwaysDelivered | EventKinds::fromValue(0x1FF)));
    // An unknown render-request case is a trap.
    CHECK(errorOf(guest.instance().update(TimerFired {})) == ExtensionError::Trapped);
}

TEST_CASE("Instance.hostCallBudget", "[vtextension]")
{
    // A log message longer than one host call may copy traps; one within the budget is delivered.
    // (The run-command arguments are bounded by the same budget: see Instance.malicious.)
    auto limits = ExtensionLimits {};
    limits.maxHostCallBytes = 1024;
    auto tight = RunningGuest(toModule(vtextension::testing::MaliciousModule), limits);
    CHECK(errorOf(tight.instance().render(surface(80, 9))) == ExtensionError::Trapped);
    CHECK(tight.services.logged.empty());

    auto roomy = RunningGuest(toModule(vtextension::testing::MaliciousModule));
    CHECK(roomy.instance().render(surface(80, 9)).value() == "ok");
    REQUIRE(roomy.services.logged.size() == 1);
    CHECK(roomy.services.logged.front().message.size() == 4096);
}

TEST_CASE("Instance.hostCallException", "[vtextension]")
{
    // A host service that throws faults the call; the exception never crosses into the interpreter.
    auto guest = RunningGuest(toModule(vtextension::testing::MaliciousModule));
    guest.services.beforeCommandAnswer = [] {
        throw std::runtime_error("host service failed");
    };
    CHECK(errorOf(guest.instance().render(surface(80, 10))) == ExtensionError::Trapped);
    CHECK(guest.services.commands.size() == 1);

    // The same call, answered: a well-formed run-command reaches the host.
    auto calm = RunningGuest(toModule(vtextension::testing::MaliciousModule));
    calm.services.commandAnswer = "";
    CHECK(calm.instance().render(surface(80, 10)).value() == "ok");
    CHECK(calm.services.commands.size() == 1);
}

namespace
{

/// A clock that moves an hour on from every reading to the next.
class HastyClock final: public core::platform::IClock
{
  public:
    [[nodiscard]] core::platform::SteadyTimePoint now() const noexcept override
    {
        auto const lock = std::scoped_lock { _mutex };
        _now += std::chrono::hours { 1 };
        return _now;
    }

  private:
    mutable std::mutex _mutex;
    mutable core::platform::SteadyTimePoint _now;
};

} // namespace

TEST_CASE("Instance.deadlineSpentBeforeCommand", "[vtextension]")
{
    // When the call's deadline is spent by the time the guest asks for a command, the command does
    // not run, and the call fails as overrun.
    auto const backend = wasm::makeDefaultBackend();
    auto const engine = Engine::create(*backend, ExtensionLimits {});
    REQUIRE(engine.has_value());
    auto const compiled = CompiledExtension::load(**engine, toModule(vtextension::testing::MaliciousModule));
    REQUIRE(compiled.has_value());
    auto services = vtextension::testing::FakeHostServices {};
    services.commandAnswer = "";
    auto clock = HastyClock {};
    auto instance = Instance::create(**engine, **compiled, services, clock, ExtensionLimits {});
    REQUIRE(instance.has_value());

    CHECK(errorOf((*instance)->render(surface(80, 10))) == ExtensionError::DeadlineExceeded);
    CHECK(services.commands.empty());
}

TEST_CASE("Instance.expiredBetweenCalls", "[vtextension]")
{
    // One guarded body makes several export calls, and the watchdog fires a deadline only once. A
    // deadline that expires between two of them -- here while the host lifts render's result, between the
    // export and its post-return -- must still stop the next one from entering the guest: the call fails
    // as overrun, and the instance with it.
    auto backend = vtextension::testing::FakeWasmBackend {};
    auto renders = 0;
    auto postRenders = 0;
    auto memoryAccesses = 0;
    backend.guest.exports["render"] =
        [&](std::span<uint32_t> argv) -> std::expected<void, wasm::CallFailure> {
        ++renders;
        // The result at 16: the list's address (24) and length (2), then at 24 the list, "ok".
        auto& memory = backend.lastInstance->linearMemory;
        auto const put = [&](std::size_t at, uint32_t value) {
            for (auto const index: std::views::iota(std::size_t { 0 }, sizeof(value)))
                memory.at(at + index) = static_cast<std::byte>(value >> (8 * index));
        };
        put(16, 24);
        put(20, 2);
        memory.at(24) = std::byte { 'o' };
        memory.at(25) = std::byte { 'k' };
        argv.front() = 16;
        return {};
    };
    backend.guest.exports["cabi_post_render"] =
        [&](std::span<uint32_t> /*argv*/) -> std::expected<void, wasm::CallFailure> {
        ++postRenders;
        return {};
    };
    // The host's first look at the result lasts until the deadline has expired: it waits for the watchdog's
    // interrupt, however slow the machine (bounded, so that a watchdog that never fires fails the checks
    // below rather than hanging the test).
    backend.guest.onMemoryAccess = [&] {
        if (memoryAccesses++ != 0)
            return;
        auto const giveUp = std::chrono::steady_clock::now() + std::chrono::seconds { 5 };
        while (backend.lastInstance->interrupts.load() == 0 && std::chrono::steady_clock::now() < giveUp)
            std::this_thread::sleep_for(std::chrono::milliseconds { 1 });
    };

    auto limits = ExtensionLimits {};
    limits.callDeadline = std::chrono::milliseconds { 20 };
    auto const engine = Engine::create(backend, limits);
    REQUIRE(engine.has_value());
    auto const compiled = CompiledExtension::load(**engine, toModule(vtextension::testing::MinimalModule));
    REQUIRE(compiled.has_value());
    auto services = vtextension::testing::FakeHostServices {};
    auto clock = core::platform::ManualClock {};
    auto const instance = Instance::create(**engine, **compiled, services, clock, limits);
    REQUIRE(instance.has_value());

    CHECK(errorOf((*instance)->render(surface(80, 1))) == ExtensionError::DeadlineExceeded);
    CHECK(postRenders == 0);
    REQUIRE(backend.lastInstance != nullptr);
    CHECK(backend.lastInstance->interrupts.load() == 1);

    // The instance is done: the next call fails the same way, without entering the guest.
    CHECK(errorOf((*instance)->render(surface(80, 1))) == ExtensionError::DeadlineExceeded);
    CHECK(renders == 1);
}

TEST_CASE("Instance.growthRefusedInHostCallAllocation", "[vtextension]")
{
    // A host call whose allocation in the guest fails because memory refused to grow faults the call as
    // out of memory: render asks for the state, the host's store of it allocates through cabi_realloc,
    // which fails with refused growth, and the host call's error (OutOfMemory) is what the render fails
    // with, though the guest trapped afterwards. (An allocation that failed for another reason is a trap.)
    struct Case
    {
        std::string_view what;
        wasm::CallFailure reallocFailure;
        ExtensionError error;
    };
    auto const cases = std::to_array<Case>({
        { .what = "growth refused",
          .reallocFailure = wasm::CallFailure::MemoryGrowthRefused,
          .error = ExtensionError::OutOfMemory },
        { .what = "another trap",
          .reallocFailure = wasm::CallFailure::Trap,
          .error = ExtensionError::Trapped },
    });
    for (auto const& testCase: cases)
    {
        INFO(testCase.what);
        auto backend = vtextension::testing::FakeWasmBackend {};
        auto reallocs = 0;
        backend.guest.exports["cabi_realloc"] =
            [&](std::span<uint32_t> /*argv*/) -> std::expected<void, wasm::CallFailure> {
            ++reallocs;
            return std::unexpected(testCase.reallocFailure);
        };
        backend.guest.exports["render"] =
            [&](std::span<uint32_t> /*argv*/) -> std::expected<void, wasm::CallFailure> {
            constexpr auto StateFunction = std::size_t { 2 }; // `state`, in the world's host table
            auto const resultAddress = std::array { uint32_t { 64 } };
            if (!backend.lastInstance->callHost(StateFunction, resultAddress))
                return std::unexpected(wasm::CallFailure::Trap); // the guest traps on its own afterwards
            return {};
        };

        auto const limits = ExtensionLimits {};
        auto const engine = Engine::create(backend, limits);
        REQUIRE(engine.has_value());
        auto const compiled =
            CompiledExtension::load(**engine, toModule(vtextension::testing::MinimalModule));
        REQUIRE(compiled.has_value());
        auto services = vtextension::testing::FakeHostServices {};
        services.snapshot.title = "a title the host must allocate for";
        auto clock = core::platform::ManualClock {};
        auto const instance = Instance::create(**engine, **compiled, services, clock, limits);
        REQUIRE(instance.has_value());

        CHECK(errorOf((*instance)->render(surface(80, 1))) == testCase.error);
        CHECK(reallocs == 1);
        CHECK((*instance)->fault() == testCase.error);
    }
}

TEST_CASE("Instance.reentrantHostCall", "[vtextension]")
{
    // The guest's cabi_realloc logs. Called while the host lowers `load`'s configuration, no host call
    // is in progress, and the log is delivered ...
    auto guest = RunningGuest(toModule(vtextension::testing::ReentrantReallocModule));
    REQUIRE(guest.instance().load({ { "key", "value" } }).has_value());
    CHECK_FALSE(guest.services.logged.empty());

    // ... but called while the host stores `state`'s result, it would enter a second host call from
    // inside the first: the call faults instead, and the nested log never reaches the host.
    guest.services.logged.clear();
    guest.services.snapshot.title = "a title the host must allocate for";
    CHECK(errorOf(guest.instance().render(surface(80, 1))) == ExtensionError::Trapped);
    CHECK(guest.services.logged.empty());
    CHECK(guest.instance().fault() == ExtensionError::Trapped);
}

TEST_CASE("Instance.stackOverflow", "[vtextension]")
{
    // Unbounded recursion runs into the instance's stack limit, checked in software: a trap, not a crash.
    auto guest = RunningGuest(toModule(vtextension::testing::UnboundedRecursionModule));
    CHECK(errorOf(guest.instance().render(surface(80, 1))) == ExtensionError::Trapped);
    CHECK(guest.instance().fault() == ExtensionError::Trapped);
}

TEST_CASE("CompiledExtension.wasi", "[vtextension]")
{
    auto const backend = wasm::makeDefaultBackend();
    // Only WASI functions that return at once and reach nothing outside the module are provided.
    CHECK(loadModule(*backend, toModule(vtextension::testing::WasiAllowedModule)).has_value());
    CHECK(errorOf(loadModule(*backend, toModule(vtextension::testing::WasiPollOneoffModule)))
          == ExtensionError::WorldMismatch);
    CHECK(errorOf(loadModule(*backend, toModule(vtextension::testing::WasiSockOpenModule)))
          == ExtensionError::WorldMismatch);
    CHECK(errorOf(loadModule(*backend, toModule(vtextension::testing::WasiPathOpenModule)))
          == ExtensionError::WorldMismatch);
}

TEST_CASE("Instance.realloc", "[vtextension]")
{
    // What the guest's allocator answers is checked before the host writes there.
    for (auto const& module: { toModule(vtextension::testing::OutOfBoundsReallocModule),
                               toModule(vtextension::testing::MisalignedReallocModule) })
    {
        auto guest = RunningGuest(module);
        CHECK(errorOf(guest.instance().load({ { "text", "hi" } })) == ExtensionError::Trapped);
    }
}

TEST_CASE("Instance.threadEnv", "[vtextension]")
{
    auto guest = RunningGuest(toModule(vtextension::testing::MinimalModule));

    // A thread other than the one that brought the runtime up calls in under a guard ...
    // (Catch2's assertions are not thread-safe: the worker records, the test thread checks.)
    auto entered = std::expected<void, ExtensionError> {};
    auto rendered = std::expected<std::string, ExtensionError> {};
    auto worker = std::thread([&] {
        auto const threadEnv = guest.backend().enterThread();
        auto const nested = guest.backend().enterThread(); // guards nest
        if (!threadEnv || !nested)
        {
            entered = std::unexpected(ExtensionError::RuntimeUnavailable);
            return;
        }
        rendered = guest.instance().render(surface(80, 1));
    });
    worker.join();
    REQUIRE(entered.has_value());
    CHECK(rendered.has_value());

    // ... and a guard on the bringing-up thread leaves that thread's state alone when it ends.
    {
        auto const threadEnv = guest.backend().enterThread();
        REQUIRE(threadEnv.has_value());
    }
    CHECK(guest.instance().render(surface(80, 1)).has_value());
}

#ifdef VTEXTENSION_TEST_MODULE_DIR
using vtextension::testing::readTestModule;

namespace
{

/// @return @p duration in fractional milliseconds, for reporting.
[[nodiscard]] double toMilliseconds(std::chrono::steady_clock::duration duration)
{
    return std::chrono::duration<double, std::milli>(duration).count();
}

/// A hostile guest loaded with `mode` set to @p mode.
[[nodiscard]] std::unique_ptr<RunningGuest> hostile(std::string_view mode, ExtensionLimits limits = {})
{
    auto guest = std::make_unique<RunningGuest>(readTestModule("hostile.wasm"), limits);
    REQUIRE(guest->instance().load({ { "mode", std::string(mode) } }).has_value());
    return guest;
}

} // namespace

TEST_CASE("CompiledExtension.sleeper", "[vtextension][guest]")
{
    auto const backend = wasm::makeDefaultBackend();
    // A Rust guest that sleeps imports WASI's poll_oneoff, a wait no deadline can interrupt.
    CHECK(errorOf(loadModule(*backend, readTestModule("sleeper.wasm"))) == ExtensionError::WorldMismatch);
}

TEST_CASE("Instance.echo.render", "[vtextension][guest]")
{
    auto bytes = readTestModule("echo.wasm");
    auto const start = std::chrono::steady_clock::now();
    auto guest = RunningGuest(std::move(bytes));
    INFO("load + check + instantiate: " << toMilliseconds(std::chrono::steady_clock::now() - start) << " ms");
    CHECK(guest.loadAndRender({ { "text", "hi" } }) == "hi");
    // The echo guest's manifest came along.
    REQUIRE(guest.compiled().manifest().has_value());
    CHECK(guest.compiled().manifest()->name == "echo");
}

TEST_CASE("Instance.hostile.spin", "[vtextension][guest]")
{
    auto guest = hostile("spin");
    auto const start = std::chrono::steady_clock::now();
    auto const result = guest->instance().render(surface(80, 1));
    auto const elapsed = std::chrono::steady_clock::now() - start;

    // The bound only proves that termination happened at all; how soon is the scheduler's business.
    auto const limits = ExtensionLimits {};
    INFO("spin stopped after " << toMilliseconds(elapsed) << " ms (deadline " << limits.callDeadline.count()
                               << " ms)");
    REQUIRE_FALSE(result.has_value());
    CHECK(errorOf(result) == ExtensionError::DeadlineExceeded);
    CHECK(elapsed < 10 * limits.loadDeadline);

    // The instance is done: it answers with the same error and does not run the guest again.
    CHECK(guest->instance().fault() == ExtensionError::DeadlineExceeded);
    auto const again = std::chrono::steady_clock::now();
    CHECK(errorOf(guest->instance().update(TimerFired {})) == ExtensionError::DeadlineExceeded);
    CHECK(errorOf(guest->instance().render(surface(80, 1))) == ExtensionError::DeadlineExceeded);
    INFO("refused in " << toMilliseconds(std::chrono::steady_clock::now() - again) << " ms");
}

TEST_CASE("Instance.hostile.spinLoad", "[vtextension][guest]")
{
    auto guest = RunningGuest(readTestModule("hostile.wasm"));
    auto const result = guest.instance().load({ { "mode", "spin-load" } });
    REQUIRE_FALSE(result.has_value());
    CHECK(errorOf(result) == ExtensionError::DeadlineExceeded);
}

TEST_CASE("Instance.hostile.trap", "[vtextension][guest]")
{
    auto guest = hostile("trap");
    CHECK(errorOf(guest->instance().render(surface(80, 1))) == ExtensionError::Trapped);
    CHECK(errorOf(guest->instance().render(surface(80, 1))) == ExtensionError::Trapped);

    // A fresh instance of the same module works.
    guest->restart();
    CHECK(guest->loadAndRender({}) == "calm");
}

TEST_CASE("Instance.hostile.alloc", "[vtextension][guest]")
{
    // The cap must be what stops it, however long the way to the cap takes: growing a memory costs a
    // platform accident. Without hardware bounds checks WAMR grows one through os_mremap(), which on Linux
    // moves the mapping but on Windows copies it whole into a new one -- quadratic over the ~30 steps to
    // the cap, and slower than the default call deadline.
    auto limits = ExtensionLimits {};
    limits.callDeadline = std::chrono::seconds { 30 };
    auto guest = hostile("alloc", limits);
    auto const result = guest->instance().render(surface(80, 1));
    REQUIRE_FALSE(result.has_value());
    INFO(errorText(result.error()));
    // A backend that reports refused growth fails the call as out of memory; one that cannot tell it
    // from any other trap fails it as a trap (see the contract's memoryCapRefusesGrowth).
    auto const expected =
        guest->backend().capabilities().memoryGrowth == wasm::MemoryGrowthReporting::Reported
            ? ExtensionError::OutOfMemory
            : ExtensionError::Trapped;
    CHECK(result.error() == expected);
}

TEST_CASE("Instance.hostile.hugeOutput", "[vtextension][guest]")
{
    auto guest = hostile("huge-output");
    CHECK(errorOf(guest->instance().render(surface(80, 1))) == ExtensionError::OutputTooLarge);
    // The runtime saw nothing wrong, but the instance is done all the same: even a call that would
    // succeed fails, without entering the guest.
    CHECK(errorOf(guest->instance().update(TimerFired {})) == ExtensionError::OutputTooLarge);

    // Under a higher limit, the same output is fine.
    auto limits = ExtensionLimits {};
    limits.maxRenderBytes = std::size_t { 4 } * 1024 * 1024;
    auto generous = hostile("huge-output", limits);
    CHECK(generous->instance().render(surface(80, 1)).value().size() == std::size_t { 2 } * 1024 * 1024);
}

TEST_CASE("Instance.hostile.frameFlood", "[vtextension][guest]")
{
    // Every request reaches the host, which is where they are rate-capped.
    auto guest = hostile("frame-flood");
    CHECK(guest->instance().render(surface(80, 1)).value() == "flooded");
    CHECK(guest->services.frameRequests.size() == 1000);
}

    #ifndef _WIN32
using vtextension::testing::StandardStreamsCapture;

TEST_CASE("Instance.hostile.print", "[vtextension][guest]")
{
    // The guest's standard output and error lead nowhere: nothing it prints reaches the host's.
    auto capture = StandardStreamsCapture {};
    auto rendered = std::expected<std::string, ExtensionError> {};
    {
        auto guest = hostile("print");
        rendered = guest->instance().render(surface(80, 1));
    }
    auto const captured = capture.finish();

    CHECK(rendered.value() == "printed");
    INFO("captured: " << captured);
    CHECK(captured.empty());
}
    #endif

TEST_CASE("Instance.spawner.runCommand", "[vtextension][guest]")
{
    auto guest = RunningGuest(readTestModule("spawner.wasm"));
    REQUIRE(guest.compiled().manifest().has_value());
    CHECK(guest.compiled().manifest()->commands.front().argvTemplate
          == std::vector<std::string> { "echo", "{word}" });

    guest.services.commandAnswer = "hello\n";
    CHECK(guest.loadAndRender({}) == "ok:0:hello\n:");
    REQUIRE(guest.services.commands.size() == 1);
    CHECK(guest.services.commands.front()
          == vtextension::testing::CommandCall { .name = "echo", .args = { { "word", "hello" } } });
}

TEST_CASE("Instance.spawner.commandErrors", "[vtextension][guest]")
{
    struct Case
    {
        ExtensionError error;
        std::string_view rendered;
    };
    auto const cases = std::to_array<Case>({
        { .error = ExtensionError::CommandNotDeclared, .rendered = "err:not-declared" },
        { .error = ExtensionError::ArgumentRejected, .rendered = "err:not-declared" },
        { .error = ExtensionError::PermissionPending, .rendered = "err:pending" },
        { .error = ExtensionError::PermissionDenied, .rendered = "err:denied" },
        { .error = ExtensionError::DeadlineExceeded, .rendered = "err:timed-out" },
        { .error = ExtensionError::RuntimeUnavailable, .rendered = "err:failed" },
    });
    for (auto const& testCase: cases)
    {
        INFO(errorText(testCase.error));
        auto guest = RunningGuest(readTestModule("spawner.wasm"));
        guest.services.commandAnswer = std::unexpected(testCase.error);
        CHECK(guest.loadAndRender({}) == testCase.rendered);
        // The instance survives a command that failed: that is the extension's business.
        CHECK_FALSE(guest.instance().fault().has_value());
    }
}

TEST_CASE("Instance.spawner.deadlineStandsStill", "[vtextension][guest]")
{
    // A command may take longer than a call's deadline: the host bounds it, not the guest's deadline.
    // An hour passes on the instance's clock while the command runs, and the call still succeeds.
    auto guest = RunningGuest(readTestModule("spawner.wasm"));
    guest.services.commandAnswer = "slow";
    guest.services.beforeCommandAnswer = [&] {
        guest.clock.advance(std::chrono::hours { 1 });
    };
    CHECK(guest.loadAndRender({}) == "ok:0:slow:");
}
#else
TEST_CASE("Instance: the Rust-guest cases", "[vtextension][guest]")
{
    SKIP("the Rust test guests are not built (CONTOUR_EXTENSIONS_BUNDLED=OFF)");
}
#endif
