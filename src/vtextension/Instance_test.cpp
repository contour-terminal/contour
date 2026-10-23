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
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <vtextension/CompiledExtension.hpp>
#include <vtextension/Engine.hpp>
#include <vtextension/Instance.hpp>
#include <vtextension/testing/Harness.hpp>
#include <vtextension/testing/StandardStreamsCapture.hpp>
#include <vtextension/testing/TestModules.hpp>

using namespace std::string_view_literals;
using namespace vtextension;
using vtextension::testing::errorOf;
using vtextension::testing::RunningGuest;
using vtextension::testing::surface;
using vtextension::testing::toModule;

namespace
{

/// Loads @p module on a fresh engine and returns how that went.
[[nodiscard]] std::expected<std::shared_ptr<CompiledExtension>, ExtensionError> loadModule(
    std::vector<std::byte> module)
{
    auto const engine = Engine::create(ExtensionLimits {});
    REQUIRE(engine.has_value());
    return CompiledExtension::load(**engine, std::move(module));
}

} // namespace

TEST_CASE("CompiledExtension.load", "[vtextension]")
{
    auto const compiled = loadModule(toModule(vtextension::testing::MinimalModule));
    REQUIRE(compiled.has_value());
    // A module without a manifest loads; it declares no commands.
    CHECK_FALSE((*compiled)->manifest().has_value());
}

TEST_CASE("CompiledExtension.invalid", "[vtextension]")
{
    CHECK(errorOf(loadModule({})) == ExtensionError::InvalidComponent);
    auto truncated = toModule(vtextension::testing::MinimalModule);
    truncated.resize(truncated.size() / 2);
    CHECK(errorOf(loadModule(truncated)) == ExtensionError::InvalidComponent);
}

TEST_CASE("CompiledExtension.worldMismatch", "[vtextension]")
{
    // The runtime links imports lazily, so each of these would load and fail only when called.
    SECTION("an unknown host function")
    {
        CHECK(errorOf(loadModule(toModule(vtextension::testing::UnknownImportModule)))
              == ExtensionError::WorldMismatch);
    }
    SECTION("a host function with the wrong signature")
    {
        CHECK(errorOf(loadModule(toModule(vtextension::testing::WrongImportSignatureModule)))
              == ExtensionError::WorldMismatch);
    }
    SECTION("an import from a module that is neither the host nor WASI")
    {
        CHECK(errorOf(loadModule(toModule(vtextension::testing::ForeignImportModuleModule)))
              == ExtensionError::WorldMismatch);
    }
    SECTION("a missing export")
    {
        CHECK(errorOf(loadModule(toModule(vtextension::testing::MissingExportModule)))
              == ExtensionError::WorldMismatch);
    }
    SECTION("a start function, which would run where no deadline guards it")
    {
        CHECK(errorOf(loadModule(toModule(vtextension::testing::StartFunctionModule)))
              == ExtensionError::WorldMismatch);
    }
    SECTION("an export with the wrong signature")
    {
        CHECK(errorOf(loadModule(toModule(vtextension::testing::WrongExportSignatureModule)))
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
    auto const engine = Engine::create(limits);
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
    auto const engine = Engine::create(ExtensionLimits {});
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
    // Only WASI functions that return at once and reach nothing outside the module are provided.
    CHECK(loadModule(toModule(vtextension::testing::WasiAllowedModule)).has_value());
    CHECK(errorOf(loadModule(toModule(vtextension::testing::WasiPollOneoffModule)))
          == ExtensionError::WorldMismatch);
    CHECK(errorOf(loadModule(toModule(vtextension::testing::WasiSockOpenModule)))
          == ExtensionError::WorldMismatch);
    CHECK(errorOf(loadModule(toModule(vtextension::testing::WasiPathOpenModule)))
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
        auto const threadEnv = Engine::ThreadEnv::enter();
        auto const nested = Engine::ThreadEnv::enter(); // guards nest
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
        auto const threadEnv = Engine::ThreadEnv::enter();
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
    // A Rust guest that sleeps imports WASI's poll_oneoff, a wait no deadline can interrupt.
    CHECK(errorOf(loadModule(readTestModule("sleeper.wasm"))) == ExtensionError::WorldMismatch);
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
    CHECK(result.error() == ExtensionError::OutOfMemory);
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
