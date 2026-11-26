// SPDX-License-Identifier: Apache-2.0
//
// The WAMR engine an extension host process drives: how it shares memory, the one null device its runtimes
// share, and how it prepares a thread.
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

#ifdef __linux__
    #include <filesystem>
#endif

#include <vtextension/wasm/WasmRuntime.hpp>
#include <vtextension/wasm/remote/proto/Pdu.hpp>
#include <vtextension/wasm/testing/BackendContract.hpp>
#include <vtextension/wasm/wamr/WamrBackend.hpp>
#include <vtextension/wasm/wamr/WamrEngine.hpp>
#include <vtextension/wasm/wamr/WamrThreadEnv.hpp>

namespace contract = vtextension::wasm::testing::contract;
using vtextension::wasm::IWasmRuntime;
using vtextension::wasm::host::GuestCode;
using vtextension::wasm::remote::proto::MemoryMode;
using vtextension::wasm::wamr::linkedNativeCompilation;
using vtextension::wasm::wamr::makeWamrEngine;
using vtextension::wasm::wamr::NativeCompilation;
using vtextension::wasm::wamr::NullDeviceOpener;
using vtextension::wasm::wamr::systemNullDeviceOpener;
using vtextension::wasm::wamr::ThreadEnv;
using vtextension::wasm::wamr::WamrEngineConfig;
using vtextension::wasm::wamr::WamrEngineError;
using vtextension::wasm::wamr::wamrEngineErrorText;

namespace
{

/// @return An opener of the system's null device that counts its calls in @p opened.
[[nodiscard]] NullDeviceOpener countingOpener(std::size_t& opened)
{
    return [&opened, open = systemNullDeviceOpener()] {
        ++opened;
        return open();
    };
}

/// @return How many descriptors this process holds open; nothing where the system does not say.
[[nodiscard]] std::optional<std::ptrdiff_t> openDescriptors()
{
#ifdef __linux__
    auto const entries = std::filesystem::directory_iterator { "/proc/self/fd" };
    return std::distance(std::filesystem::begin(entries), std::filesystem::end(entries));
#else
    return std::nullopt;
#endif
}

} // namespace

TEST_CASE("WamrEngine.copiesMemory", "[vtextension][wamr]")
{
    auto const made = makeWamrEngine();
    REQUIRE(made.has_value());
    auto const& engine = *made;
    CHECK(engine->memoryMode() == MemoryMode::Copy);
    CHECK(engine->memoryGuardBytes() == 0);
    CHECK(engine->guestCode() == GuestCode::Interpreted);
}

TEST_CASE("WamrEngine.runtimesShareOneNullDevice", "[vtextension][wamr]")
{
    auto const before = openDescriptors();
    auto opened = std::size_t { 0 };
    {
        auto const made = makeWamrEngine(WamrEngineConfig { .openNullDevice = countingOpener(opened) });
        REQUIRE(made.has_value());
        auto const& engine = *made;
        CHECK(opened == 1);
        auto const afterEngine = openDescriptors();

        // Three runtimes, each instantiating a module that imports WASI, which binds the device: none opens
        // another, so a host that can no longer open one still runs them.
        auto runtimes = std::vector<std::shared_ptr<IWasmRuntime>> {};
        auto context = contract::RecordingHostContext {};
        for ([[maybe_unused]] auto const index: { 0, 1, 2 })
        {
            auto const& runtime = runtimes.emplace_back(contract::createRuntime(*engine));
            auto const module = contract::load(*runtime, contract::DescribedModule);
            CHECK(module->instantiate(context).has_value());
        }
        CHECK(opened == 1);
        CHECK(openDescriptors() == afterEngine);
    }
    // ... and it is closed with the engine and the last runtime.
    CHECK(openDescriptors() == before);
}

TEST_CASE("WamrEngine.holdsTheNullDeviceItOpened", "[vtextension][wamr]")
{
    // The one device the engine keeps is the one a sandbox that limits descriptor by descriptor must leave
    // it, and it is what the opener returned.
    auto opened = std::optional<int64_t> {};
    auto const made =
        makeWamrEngine(WamrEngineConfig { .openNullDevice = [&opened, open = systemNullDeviceOpener()] {
            opened = open();
            return opened;
        } });
    REQUIRE(made.has_value());
    REQUIRE(opened.has_value());
    auto const held = (*made)->heldDevices();
    REQUIRE(held.size() == 1);
#ifndef _WIN32
    CHECK(held.front() == static_cast<int>(*opened));
#endif
}

TEST_CASE("WamrEngine.prepareThreadIsIdempotent", "[vtextension][wamr]")
{
    auto const made = makeWamrEngine();
    REQUIRE(made.has_value());
    auto const& engine = *made;
    // Before any runtime exists, as a host prepares its threads before it locks itself down: on a thread
    // that did not bring the runtime up, and on the one that did.
    auto prepared = std::vector<bool> {};
    auto worker = std::thread { [&] {
        for ([[maybe_unused]] auto const index: { 0, 1 })
        {
            auto const result = engine->prepareThread();
            prepared.push_back(result.has_value() && engine->isCurrentThreadPrepared());
        }
        prepared.push_back(ThreadEnv::preparedGeneration() == engine->runtimeGeneration());
    } };
    worker.join();
    CHECK(prepared == std::vector { true, true, true });
    CHECK(engine->prepareThread().has_value());
    CHECK(engine->prepareThread().has_value());
    CHECK(engine->isCurrentThreadPrepared());
}

TEST_CASE("WamrEngine.refusesAWamrThatCompiles", "[vtextension][wamr]")
{
    // The build's WAMR interprets only; one that could compile is refused before anything is opened, as its
    // host's sandbox would otherwise kill it, or leave its code cache executable.
    CHECK(linkedNativeCompilation() == NativeCompilation::Absent);
    auto opened = std::size_t { 0 };
    // Counts without opening anything: refused first, it must not be called at all.
    auto const made =
        makeWamrEngine(WamrEngineConfig { .openNullDevice =
                                              [&opened] {
                                                  ++opened;
                                                  return std::optional<int64_t> {};
                                              },
                                          .nativeCompilation = [] { return NativeCompilation::Present; } });
    REQUIRE_FALSE(made.has_value());
    CHECK(made.error() == WamrEngineError::CompilesNativeCode);
    CHECK(opened == 0);
    CHECK_FALSE(wamrEngineErrorText(made.error()).empty());
}
