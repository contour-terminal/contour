// SPDX-License-Identifier: Apache-2.0
#include <vtextension/wasm/wamr/WamrThreadEnv.hpp>

#include <core/Assert.hpp>

#include <atomic>
#include <cstddef>
#include <mutex>
#include <tuple>
#include <utility>

#include <wasm_export.h>

namespace vtextension::wasm::wamr
{

namespace
{
    /// Which thread holds the runtime's per-thread state because it brought the runtime up, and how
    /// many holders keep the runtime up. WAMR counts its own initializations; this mirrors that count
    /// so that a ThreadEnv can tell the bringing-up thread, whose state is the runtime's to tear down.
    struct RuntimeOwnership
    {
        std::mutex mutex;
        std::size_t holders = 0;
        std::thread::id initializingThread;
    };

    [[nodiscard]] RuntimeOwnership& runtimeOwnership()
    {
        static auto ownership = RuntimeOwnership {};
        return ownership;
    }

    /// How many ThreadEnv guards the calling thread holds.
    thread_local std::size_t threadEnvDepth = 0;

    /// The runtime generation the calling thread's state was set up for; 0 if none.
    thread_local uint64_t threadEnvGeneration = 0;

    /// Counts the runtime's bring-ups. @see runtimeGeneration
    std::atomic<uint64_t> currentGeneration { 0 };

    /// Runs @p body while the runtime of @p generation is up, held up meanwhile: a bring-up publishes the
    /// runtime here only once WAMR is initialized, and a teardown withdraws it here before WAMR goes.
    /// @return Whether the runtime was up and @p body succeeded.
    template <typename Body>
    [[nodiscard]] bool whileRuntimeUp(uint64_t generation, Body const& body)
    {
        auto& ownership = runtimeOwnership();
        auto const lock = std::scoped_lock { ownership.mutex };
        return ownership.holders > 0 && currentGeneration.load() == generation && body();
    }

    /// Whether the calling thread is the one that brought the running runtime up.
    [[nodiscard]] bool isInitializingThread()
    {
        auto& ownership = runtimeOwnership();
        auto const lock = std::scoped_lock { ownership.mutex };
        return ownership.holders > 0 && ownership.initializingThread == std::this_thread::get_id();
    }
} // namespace

uint64_t runtimeGeneration() noexcept
{
    return currentGeneration.load();
}

std::expected<void, ExtensionError> acquireRuntime()
{
    // The system allocator and the interpreter, the build's only running mode. Only the first call
    // initializes; later ones join the running runtime. WAMR reference-counts this pair with
    // wasm_runtime_destroy() under its own lock.
    if (!wasm_runtime_init())
        return std::unexpected(ExtensionError::RuntimeUnavailable);

    // WAMR logs to stdout, which is not ours to write to. Errors that matter reach the caller through
    // error buffers and exceptions; the warnings are noise, among them one on every load of a
    // wit-bindgen cdylib, which is neither a WASI command nor a reactor.
    wasm_runtime_set_log_level(WASM_LOG_LEVEL_ERROR);

    auto& ownership = runtimeOwnership();
    auto const lock = std::scoped_lock { ownership.mutex };
    if (ownership.holders++ == 0)
    {
        ownership.initializingThread = std::this_thread::get_id();
        currentGeneration.fetch_add(1);
    }
    return {};
}

void releaseRuntime() noexcept
{
    {
        auto& ownership = runtimeOwnership();
        auto const lock = std::scoped_lock { ownership.mutex };
        if (--ownership.holders == 0)
            ownership.initializingThread = {};
    }
    wasm_runtime_destroy();
}

uint64_t ThreadEnv::preparedGeneration() noexcept
{
    return threadEnvGeneration;
}

bool ThreadEnv::isCurrentThreadPrepared()
{
    return (threadEnvDepth > 0 && threadEnvGeneration == runtimeGeneration()) || isInitializingThread();
}

std::expected<ThreadEnv, ExtensionError> ThreadEnv::enter()
{
    auto const generation = runtimeGeneration();
    if (isCurrentThreadPrepared())
    {
        ++threadEnvDepth;
        return ThreadEnv { Hold::Nested, generation };
    }
    // With no runtime up there is nothing to prepare the thread for, and on Windows WAMR would keep the
    // thread's state in a thread-local slot the runtime does not hold: its key is freed, or never was.
    if (!whileRuntimeUp(generation, [] { return wasm_runtime_init_thread_env(); }))
        return std::unexpected(ExtensionError::RuntimeUnavailable);
    ++threadEnvDepth;
    threadEnvGeneration = generation;
    return ThreadEnv { Hold::Owned, generation };
}

ThreadEnv::ThreadEnv(ThreadEnv&& other) noexcept:
    _hold { std::exchange(other._hold, Hold::Released) },
    _thread { other._thread },
    _generation { other._generation }
{
}

ThreadEnv::~ThreadEnv()
{
    if (_hold == Hold::Released)
        return;
    // The depth and the runtime's state are the entering thread's: ending the guard elsewhere would
    // corrupt both.
    Require(_thread == std::this_thread::get_id());
    --threadEnvDepth;
    if (_hold != Hold::Owned)
        return;
    if (threadEnvGeneration == _generation)
        threadEnvGeneration = 0;
    // State set up for a runtime that is gone went with it (on Windows its thread-local slot was
    // freed): tearing it down then would reach into a freed slot, or into a new runtime's.
    std::ignore = whileRuntimeUp(_generation, [] {
        wasm_runtime_destroy_thread_env();
        return true;
    });
}

} // namespace vtextension::wasm::wamr
