// SPDX-License-Identifier: Apache-2.0
//
// The WAMR backend: the backend contract every runtime must meet, and what is WAMR's alone, the
// process-wide runtime its backends share, driven through WAMR's C API directly.
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <thread>
#include <vector>

#include <wasm_export.h>

#include <vtextension/ExtensionError.hpp>
#include <vtextension/ExtensionLimits.hpp>
#include <vtextension/testing/Harness.hpp>
#include <vtextension/testing/TestModules.hpp>
#include <vtextension/wasm/testing/BackendContract.hpp>
#include <vtextension/wasm/wamr/WamrBackend.hpp>
#include <vtextension/wasm/wamr/WamrHandles.hpp>
#include <vtextension/wasm/wamr/WamrRuntime.hpp>
#include <vtextension/wasm/wamr/WamrThreadEnv.hpp>

using vtextension::ExtensionError;
using vtextension::wasm::CallFailure;
using vtextension::wasm::IWasmBackend;
using vtextension::wasm::RuntimeLimits;
using vtextension::wasm::wamr::makeWamrBackend;
using vtextension::wasm::wamr::ThreadEnv;

namespace
{

/// Makes the backend under test, for the contract suite.
struct WamrTraits
{
    /// @return A WAMR backend.
    [[nodiscard]] static std::unique_ptr<IWasmBackend> make() { return makeWamrBackend(); }
};

/// The interpreter stack of the raw instance here.
constexpr auto RawStackBytes = std::uint32_t { 64 * 1024 };

/// A module needing no toolchain: (module (func (export "idle"))).
constexpr auto IdleModule = std::to_array<std::uint8_t>({
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,             // magic, version
    0x01, 0x04, 0x01, 0x60, 0x00, 0x00,                         // type 0: () -> ()
    0x03, 0x02, 0x01, 0x00,                                     // func 0: type 0
    0x07, 0x08, 0x01, 0x04, 'i',  'd',  'l',  'e',  0x00, 0x00, // export "idle" = 0
    0x0a, 0x04, 0x01, 0x02, 0x00, 0x0b,                         // 0: (empty)
});

/// Calls a host function taking no parameter at all:
/// (module (import "arity" "nop" (func)) (func (export "call-nop") (call 0))).
constexpr auto ZeroArityModule = std::to_array<std::uint8_t>({
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,                                        // magic, version
    0x01, 0x04, 0x01, 0x60, 0x00, 0x00,                                                    // type 0: () -> ()
    0x02, 0x0d, 0x01, 0x05, 'a',  'r',  'i',  't',  'y', 0x03, 'n', 'o', 'p',  0x00, 0x00, // import 0: type 0
    0x03, 0x02, 0x01, 0x00,                                                                // func 1: type 0
    0x07, 0x0c, 0x01, 0x08, 'c',  'a',  'l',  'l',  '-', 'n',  'o', 'p', 0x00, 0x01, // export "call-nop" = 1
    0x0a, 0x06, 0x01, 0x04, 0x00, 0x10, 0x00, 0x0b,                                  // 1: call 0
});

/// Loads, instantiates and calls the "idle" export of IdleModule with nothing but WAMR's C API.
/// @return Whether the call returned.
[[nodiscard]] bool callIdleRaw()
{
    auto bytes = std::vector<std::uint8_t>(IdleModule.begin(), IdleModule.end());
    auto errorBuffer = std::array<char, 128> {};
    auto const module = vtextension::wasm::wamr::detail::ModuleHandle { wasm_runtime_load(
        bytes.data(), static_cast<std::uint32_t>(bytes.size()), errorBuffer.data(), errorBuffer.size()) };
    REQUIRE(module);
    auto const instance = vtextension::wasm::wamr::detail::InstanceHandle { wasm_runtime_instantiate(
        module.get(), RawStackBytes, 0, errorBuffer.data(), errorBuffer.size()) };
    REQUIRE(instance);
    auto const execEnv = vtextension::wasm::wamr::detail::ExecEnvHandle { wasm_runtime_create_exec_env(
        instance.get(), RawStackBytes) };
    REQUIRE(execEnv);
    auto* const function = wasm_runtime_lookup_function(instance.get(), "idle");
    REQUIRE(function);
    return wasm_runtime_call_wasm(execEnv.get(), function, 0, nullptr);
}

/// A world module whose `render` grows its memory by 4 pages, and traps if it cannot.
///
///     (module
///       (memory (export "memory") 1)
///       (func (export "cabi_realloc") (param i32 i32 i32 i32) (result i32) (i32.const 64))
///       (func (export "load") (param i32 i32) (result i32) (i32.const 0))
///       (func (export "update") (param i32 i32 i32 i32 i32) (result i32) (i32.const 0))
///       (func (export "render") (param i32 i32) (result i32)
///         (if (i32.eq (memory.grow (i32.const 4)) (i32.const -1)) (then unreachable))
///         (i32.const 0)))
constexpr auto GreedyWorldModule = std::to_array<std::uint8_t>({
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x18, 0x03, 0x60, 0x04, 0x7f, 0x7f, 0x7f, 0x7f,
    0x01, 0x7f, 0x60, 0x02, 0x7f, 0x7f, 0x01, 0x7f, 0x60, 0x05, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x01, 0x7f,
    0x03, 0x05, 0x04, 0x00, 0x01, 0x02, 0x01, 0x05, 0x03, 0x01, 0x00, 0x01, 0x07, 0x32, 0x05, 0x06, 0x6d,
    0x65, 0x6d, 0x6f, 0x72, 0x79, 0x02, 0x00, 0x0c, 0x63, 0x61, 0x62, 0x69, 0x5f, 0x72, 0x65, 0x61, 0x6c,
    0x6c, 0x6f, 0x63, 0x00, 0x00, 0x04, 0x6c, 0x6f, 0x61, 0x64, 0x00, 0x01, 0x06, 0x75, 0x70, 0x64, 0x61,
    0x74, 0x65, 0x00, 0x02, 0x06, 0x72, 0x65, 0x6e, 0x64, 0x65, 0x72, 0x00, 0x03, 0x0a, 0x21, 0x04, 0x05,
    0x00, 0x41, 0xc0, 0x00, 0x0b, 0x04, 0x00, 0x41, 0x00, 0x0b, 0x04, 0x00, 0x41, 0x00, 0x0b, 0x0f, 0x00,
    0x41, 0x04, 0x40, 0x00, 0x41, 0x7f, 0x46, 0x04, 0x40, 0x00, 0x0b, 0x41, 0x00, 0x0b,
});

} // namespace

CONTOUR_WASM_BACKEND_CONTRACT(WamrTraits)

TEST_CASE("Wamr.memoryGrowthLabelsEachInstance")
{
    // WAMR has one memory-growth callback per process, which the backend owns. With two runtimes alive,
    // an instance of each still learns of its own refused growth: the extension host's Instance (as
    // OutOfMemory) and a bare backend instance (as MemoryGrowthRefused). Without the Rust guests, this is
    // also what shows that a guest whose memory refused to grow fails the Instance as OutOfMemory.
    namespace contract = vtextension::wasm::testing::contract;
    auto const capped = RuntimeLimits { .memoryBytes = std::size_t { 2 } * contract::PageBytes,
                                        .stackBytes = contract::Limits.stackBytes };
    auto const backend = makeWamrBackend();
    auto const runtime = contract::createRuntime(*backend, capped);
    auto const guard = contract::enterThread(*backend);
    auto const module = contract::load(*runtime, contract::InstanceModule);
    auto context = contract::RecordingHostContext {};
    auto const instance = contract::instantiate(*module, context);

    auto limits = vtextension::ExtensionLimits {};
    limits.memoryBytes = capped.memoryBytes;
    auto const guest =
        vtextension::testing::RunningGuest(vtextension::testing::toModule(GreedyWorldModule), limits);

    auto const rendered = guest.instance().render(vtextension::testing::surface(80, 1));
    REQUIRE_FALSE(rendered.has_value());
    CHECK(rendered.error() == ExtensionError::OutOfMemory);

    auto const grown = instance->call(contract::functionOf(*instance, "grow"), {});
    REQUIRE_FALSE(grown.has_value());
    CHECK(grown.error() == CallFailure::MemoryGrowthRefused);
}

TEST_CASE("Wamr.runtimeTwice")
{
    // The runtime is shared: a second runtime joins it, and the first one's end does not end it.
    auto const backend = makeWamrBackend();
    auto first = vtextension::wasm::testing::contract::createRuntime(*backend);
    auto const second = vtextension::wasm::testing::contract::createRuntime(*backend);
    first.reset();

    CHECK(callIdleRaw());
}

TEST_CASE("Wamr.threadEnvNeedsARuntime")
{
    // Without a runtime none is up, and a thread is not prepared for one: on Windows WAMR's thread state
    // would land in a thread-local slot no runtime holds. (Catch2's assertions are not thread-safe: the
    // worker records, the test thread checks.)
    auto const enterOnAnotherThread = [] {
        auto result = std::expected<void, ExtensionError> {};
        auto worker = std::thread([&] {
            auto const threadEnv = ThreadEnv::enter();
            if (!threadEnv)
                result = std::unexpected(threadEnv.error());
            else if (!ThreadEnv::isCurrentThreadPrepared())
                result = std::unexpected(ExtensionError::RuntimeUnavailable);
        });
        worker.join();
        return result;
    };

    auto const backend = makeWamrBackend();
    {
        auto const runtime = vtextension::wasm::testing::contract::createRuntime(*backend);
        CHECK(enterOnAnotherThread().has_value());
    }
    auto const refused = enterOnAnotherThread();
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == ExtensionError::RuntimeUnavailable);
}

TEST_CASE("Wamr.hostFunctionArities")
{
    // A host function takes from none up to MaxHostParameters i32s: a trampoline exists for each count.
    // One taking more cannot be provided, and the runtime is refused.
    namespace contract = vtextension::wasm::testing::contract;
    using vtextension::wasm::HostFunction;
    auto const backend = makeWamrBackend();

    SECTION("none")
    {
        auto const hostFunctions =
            std::array { HostFunction { .module = "arity", .name = "nop", .parameterCount = 0 } };
        auto const runtime = backend->createRuntime(contract::Limits, hostFunctions);
        REQUIRE(runtime.has_value());
        auto const guard = contract::enterThread(*backend);
        auto const module = contract::load(**runtime, ZeroArityModule);
        auto context = contract::RecordingHostContext {};
        auto const instance = contract::instantiate(*module, context);

        CHECK(instance->call(contract::functionOf(*instance, "call-nop"), {}).has_value());
        CHECK(context.calls()
              == std::vector { contract::HostCall {
                  .functionIndex = 0, .arguments = {}, .thread = std::this_thread::get_id() } });
    }

    SECTION("too many")
    {
        auto const hostFunctions =
            std::array { HostFunction { .module = "arity",
                                        .name = "wide",
                                        .parameterCount = vtextension::wasm::wamr::MaxHostParameters + 1 } };
        auto const runtime = backend->createRuntime(contract::Limits, hostFunctions);
        REQUIRE_FALSE(runtime.has_value());
        CHECK(runtime.error() == ExtensionError::RuntimeUnavailable);
    }
}
