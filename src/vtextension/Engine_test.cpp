// SPDX-License-Identifier: Apache-2.0
//
// The engine on its own: the runtime's lifetime, and the watchdog that stops a runaway call. The
// module here is driven through WAMR's C API directly, below the Instance; the Instance's own tests
// cover guests that speak the statusline world.
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <wasm_export.h>

#ifndef _WIN32
    #include <csignal>
#endif

#include <vtextension/Engine.hpp>

using namespace std::string_view_literals;
using vtextension::Engine;
using vtextension::ExtensionError;
using vtextension::ExtensionLimits;
using vtextension::WatchToken;

namespace
{

/// The interpreter stack of the raw instances here.
constexpr auto RawStackBytes = std::uint32_t { 64 * 1024 };

/// A module loaded and instantiated with nothing but the runtime, with the execution environment
/// its calls run on.
class RawInstance
{
  public:
    /// Loads and instantiates @p bytes.
    [[nodiscard]] static std::unique_ptr<RawInstance> create(std::vector<std::uint8_t> bytes)
    {
        auto instance = std::make_unique<RawInstance>(std::move(bytes));
        auto errorBuffer = std::array<char, 128> {};
        instance->_module.reset(wasm_runtime_load(instance->_bytes.data(),
                                                  static_cast<std::uint32_t>(instance->_bytes.size()),
                                                  errorBuffer.data(),
                                                  static_cast<std::uint32_t>(errorBuffer.size())));
        REQUIRE(instance->_module);
        instance->_instance.reset(wasm_runtime_instantiate(instance->_module.get(),
                                                           RawStackBytes,
                                                           0,
                                                           errorBuffer.data(),
                                                           static_cast<std::uint32_t>(errorBuffer.size())));
        REQUIRE(instance->_instance);
        instance->_execEnv.reset(wasm_runtime_create_exec_env(instance->_instance.get(), RawStackBytes));
        REQUIRE(instance->_execEnv);
        return instance;
    }

    explicit RawInstance(std::vector<std::uint8_t> bytes): _bytes { std::move(bytes) } {}

    /// Calls the parameterless export @p name under the engine's call deadline.
    [[nodiscard]] std::expected<void, ExtensionError> call(Engine& engine, char const* name)
    {
        auto* const function = wasm_runtime_lookup_function(_instance.get(), name);
        REQUIRE(function);
        auto const token = this->token();
        engine.armDeadline(token, engine.limits().callDeadline);
        auto const succeeded = wasm_runtime_call_wasm(_execEnv.get(), function, 0, nullptr);
        engine.disarm(token);
        if (succeeded)
            return {};
        if (exception().contains("terminated by user"))
            return std::unexpected(ExtensionError::DeadlineExceeded);
        return std::unexpected(ExtensionError::Trapped);
    }

    /// @return The token naming this instance to the watchdog.
    [[nodiscard]] WatchToken token() const noexcept { return WatchToken { .instance = _instance.get() }; }

    /// @return The exception the instance carries; empty if none.
    [[nodiscard]] std::string_view exception() const
    {
        auto const* const text = wasm_runtime_get_exception(_instance.get());
        return text ? std::string_view(text) : std::string_view();
    }

  private:
    std::vector<std::uint8_t> _bytes; ///< Must outlive the module, which refers into it.
    std::unique_ptr<WASMModuleCommon, decltype(&wasm_runtime_unload)> _module { nullptr,
                                                                                &wasm_runtime_unload };
    std::unique_ptr<WASMModuleInstanceCommon, decltype(&wasm_runtime_deinstantiate)> _instance {
        nullptr, &wasm_runtime_deinstantiate
    };
    std::unique_ptr<WASMExecEnv, decltype(&wasm_runtime_destroy_exec_env)> _execEnv {
        nullptr, &wasm_runtime_destroy_exec_env
    };
};

/// @return @p duration in fractional milliseconds, for reporting.
[[nodiscard]] double toMilliseconds(std::chrono::steady_clock::duration duration)
{
    return std::chrono::duration<double, std::milli>(duration).count();
}

/// A module needing no toolchain: (module (func (export "spin") (loop (br 0))) (func (export "idle"))).
/// It keeps the engine's tests independent of any toolchain.
[[nodiscard]] std::vector<std::uint8_t> hostOnlyModule()
{
    return {
        0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,                   // magic, version
        0x01, 0x04, 0x01, 0x60, 0x00, 0x00,                               // type 0: () -> ()
        0x03, 0x03, 0x02, 0x00, 0x00,                                     // funcs 0, 1: type 0
        0x07, 0x0f, 0x02, 0x04, 's',  'p',  'i',  'n',  0x00, 0x00,       // export "spin" = 0
        0x04, 'i',  'd',  'l',  'e',  0x00, 0x01,                         // export "idle" = 1
        0x0a, 0x0c, 0x02, 0x07, 0x00, 0x03, 0x40, 0x0c, 0x00, 0x0b, 0x0b, // 0: loop { br 0 }
        0x02, 0x00, 0x0b,                                                 // 1: (empty)
    };
}

/// Calls @p module's "spin" export, which never returns, and checks that the deadline stops it.
void requireDeadlineStops(std::vector<std::uint8_t> module)
{
    auto limits = ExtensionLimits {};
    limits.callDeadline = std::chrono::milliseconds { 50 };
    auto const engine = Engine::create(limits);
    REQUIRE(engine.has_value());
    auto const instance = RawInstance::create(std::move(module));

    auto const start = std::chrono::steady_clock::now();
    auto const result = instance->call(**engine, "spin");
    auto const elapsed = std::chrono::steady_clock::now() - start;

    // The bound only proves that termination happened at all; how soon is the scheduler's business.
    INFO("spin stopped after " << toMilliseconds(elapsed) << " ms (deadline " << limits.callDeadline.count()
                               << " ms)");
    REQUIRE(!result.has_value());
    CHECK(result.error() == ExtensionError::DeadlineExceeded);
    CHECK(elapsed < 10 * limits.loadDeadline);
}

} // namespace

TEST_CASE("Engine.create")
{
    auto const engine = Engine::create(ExtensionLimits {});
    REQUIRE(engine.has_value());
    CHECK((*engine)->limits().callDeadline == ExtensionLimits {}.callDeadline);
}

TEST_CASE("Engine.create.twice")
{
    // The runtime is shared: a second engine joins it, and the first one's end does not end it.
    auto first = Engine::create(ExtensionLimits {});
    REQUIRE(first.has_value());
    auto const second = Engine::create(ExtensionLimits {});
    REQUIRE(second.has_value());
    first->reset();

    auto const instance = RawInstance::create(hostOnlyModule());
    CHECK(instance->call(**second, "idle").has_value());
}

TEST_CASE("Engine.threadEnvNeedsARuntime")
{
    // Without an engine no runtime is up, and a thread is not prepared for one: on Windows WAMR's thread
    // state would land in a thread-local slot no runtime holds. (Catch2's assertions are not thread-safe:
    // the worker records, the test thread checks.)
    auto const enterOnAnotherThread = [] {
        auto result = std::expected<void, ExtensionError> {};
        auto worker = std::thread([&] {
            auto const threadEnv = Engine::ThreadEnv::enter();
            if (!threadEnv)
                result = std::unexpected(threadEnv.error());
            else if (!Engine::ThreadEnv::isCurrentThreadPrepared())
                result = std::unexpected(ExtensionError::RuntimeUnavailable);
        });
        worker.join();
        return result;
    };

    {
        auto const engine = Engine::create(ExtensionLimits {});
        REQUIRE(engine.has_value());
        CHECK(enterOnAnotherThread().has_value());
    }
    auto const refused = enterOnAnotherThread();
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == ExtensionError::RuntimeUnavailable);
}

TEST_CASE("Engine.deadline")
{
    requireDeadlineStops(hostOnlyModule());
}

TEST_CASE("Engine.disarm")
{
    // A disarmed deadline never fires: a call that finished in time leaves the instance usable.
    auto const limits = ExtensionLimits {};
    auto const engine = Engine::create(limits);
    REQUIRE(engine.has_value());
    auto const instance = RawInstance::create(hostOnlyModule());

    REQUIRE(instance->call(**engine, "idle").has_value());
    std::this_thread::sleep_for(4 * limits.callDeadline);
    CHECK(instance->exception().empty());
    CHECK(instance->call(**engine, "idle").has_value());
}

TEST_CASE("Engine.armWhileWatching")
{
    // The watchdog sleeps towards the earliest deadline while another thread arms more: arming may move the
    // deadlines in memory, and the watchdog must not be left holding on to where the earliest one was
    // (ASan checks that it is not).
    auto const engine = Engine::create(ExtensionLimits {});
    REQUIRE(engine.has_value());
    auto instances = std::vector<std::unique_ptr<RawInstance>>(256);
    std::ranges::generate(instances, [] { return RawInstance::create(hostOnlyModule()); });

    (*engine)->armDeadline(instances.front()->token(), std::chrono::hours { 1 });
    auto arming = std::thread([&] {
        for (auto const& instance: instances | std::views::drop(1))
            (*engine)->armDeadline(instance->token(), std::chrono::hours { 1 });
    });
    arming.join();
    for (auto const& instance: instances)
        (*engine)->disarm(instance->token());

    // No deadline expired: every instance is untouched.
    CHECK(std::ranges::all_of(instances, [](auto const& instance) { return instance->exception().empty(); }));
}

TEST_CASE("Engine.errorText")
{
    CHECK(vtextension::errorText(ExtensionError::DeadlineExceeded)
          == "extension call exceeded its deadline"sv);
    CHECK(vtextension::errorText(ExtensionError::OutputTooLarge) == "extension output too large"sv);
}

#ifndef _WIN32
TEST_CASE("Engine.leavesSignalsAlone")
{
    // The runtime is built without hardware bounds checks and without waking blocked calls by signal:
    // bringing it up, and preparing a thread to run guests, installs no SIGSEGV or SIGUSR1 handler (the
    // process's, or a sanitizer's, stays in charge).
    for (auto const signal: { SIGSEGV, SIGUSR1 })
    {
        INFO("signal " << signal);
        struct sigaction before {};
        REQUIRE(sigaction(signal, nullptr, &before) == 0);
        auto const engine = Engine::create(ExtensionLimits {});
        REQUIRE(engine.has_value());
        auto prepared = false;
        auto worker = std::thread([&] { prepared = Engine::ThreadEnv::enter().has_value(); });
        worker.join();
        struct sigaction after {};
        REQUIRE(sigaction(signal, nullptr, &after) == 0);
        CHECK(prepared);
        CHECK(after.sa_sigaction == before.sa_sigaction);
        CHECK(after.sa_flags == before.sa_flags);
    }
}
#endif
