// SPDX-License-Identifier: Apache-2.0
#include <vtextension/Engine.hpp>

#include <core/Assert.hpp>

#include <algorithm>
#include <atomic>
#include <system_error>
#include <tuple>
#include <utility>

#include <wasm_export.h>

#include <vtextension/HostBindings.hpp>

#ifdef _WIN32
    #include <Windows.h>
#else
    #include <fcntl.h>
    #include <unistd.h>
#endif

namespace vtextension
{

namespace
{
    /// Which thread holds the runtime's per-thread state because it brought the runtime up, and how
    /// many engines keep the runtime up. WAMR counts its own initializations; this mirrors that count
    /// so that a ThreadEnv can tell the bringing-up thread, whose state is the runtime's to tear down.
    struct RuntimeOwnership
    {
        std::mutex mutex;
        std::size_t engines = 0;
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

    /// Counts the runtime's bring-ups. @see Engine::runtimeGeneration
    std::atomic<uint64_t> currentGeneration { 0 };
} // namespace

namespace
{
    /// Runs @p body while the runtime of @p generation is up, held up meanwhile: a bring-up publishes the
    /// runtime here only once WAMR is initialized, and a teardown withdraws it here before WAMR goes.
    /// @return Whether the runtime was up and @p body succeeded.
    template <typename Body>
    [[nodiscard]] bool whileRuntimeUp(uint64_t generation, Body const& body)
    {
        auto& ownership = runtimeOwnership();
        auto const lock = std::scoped_lock { ownership.mutex };
        return ownership.engines > 0 && currentGeneration.load() == generation && body();
    }

    /// Whether the calling thread is the one that brought the running runtime up.
    [[nodiscard]] bool isInitializingThread()
    {
        auto& ownership = runtimeOwnership();
        auto const lock = std::scoped_lock { ownership.mutex };
        return ownership.engines > 0 && ownership.initializingThread == std::this_thread::get_id();
    }
} // namespace

uint64_t Engine::runtimeGeneration() noexcept
{
    return currentGeneration.load();
}

uint64_t Engine::ThreadEnv::preparedGeneration() noexcept
{
    return threadEnvGeneration;
}

bool Engine::ThreadEnv::isCurrentThreadPrepared()
{
    return (threadEnvDepth > 0 && threadEnvGeneration == runtimeGeneration()) || isInitializingThread();
}

std::expected<Engine::ThreadEnv, ExtensionError> Engine::ThreadEnv::enter()
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

Engine::ThreadEnv::ThreadEnv(ThreadEnv&& other) noexcept:
    _hold { std::exchange(other._hold, Hold::Released) },
    _thread { other._thread },
    _generation { other._generation }
{
}

Engine::ThreadEnv::~ThreadEnv()
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

std::optional<int64_t> Engine::openNullDevice() noexcept
{
#ifdef _WIN32
    auto* const handle = CreateFileW(L"NUL",
                                     GENERIC_READ | GENERIC_WRITE,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     nullptr,
                                     OPEN_EXISTING,
                                     0,
                                     nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return std::nullopt;
    return static_cast<int64_t>(reinterpret_cast<intptr_t>(handle));
#else
    auto const fd = ::open("/dev/null", O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return std::nullopt;
    return fd;
#endif
}

void Engine::closeNullDevice(int64_t handle) noexcept
{
#ifdef _WIN32
    CloseHandle(reinterpret_cast<HANDLE>(static_cast<intptr_t>(handle)));
#else
    ::close(static_cast<int>(handle));
#endif
}

std::expected<std::shared_ptr<Engine>, ExtensionError> Engine::create(ExtensionLimits limits)
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

    {
        auto& ownership = runtimeOwnership();
        auto const lock = std::scoped_lock { ownership.mutex };
        if (ownership.engines++ == 0)
        {
            ownership.initializingThread = std::this_thread::get_id();
            currentGeneration.fetch_add(1);
        }
    }

    auto const fail = [] {
        releaseRuntime();
        return std::unexpected(ExtensionError::RuntimeUnavailable);
    };

    auto hostRegistration = detail::registerHostFunctions();
    if (!hostRegistration)
        return fail();
    auto const nullDevice = openNullDevice();
    if (!nullDevice)
        return fail();

    try
    {
        return std::make_shared<Engine>(PrivateTag {}, limits, *nullDevice, std::move(hostRegistration));
    }
    catch (std::system_error const&)
    {
        // The watchdog thread could not be started: without it no deadline would hold. The engine's
        // constructor did not complete, so what it was handed is released here.
        closeNullDevice(*nullDevice);
        return fail();
    }
    catch (...)
    {
        closeNullDevice(*nullDevice);
        releaseRuntime();
        throw;
    }
}

void Engine::releaseRuntime() noexcept
{
    {
        auto& ownership = runtimeOwnership();
        auto const lock = std::scoped_lock { ownership.mutex };
        if (--ownership.engines == 0)
            ownership.initializingThread = {};
    }
    wasm_runtime_destroy();
}

Engine::Engine(PrivateTag /*unused*/,
               ExtensionLimits limits,
               int64_t nullDevice,
               std::unique_ptr<detail::HostRegistration> hostRegistration):
    _limits { limits },
    _nullDevice { nullDevice },
    _hostRegistration { std::move(hostRegistration) },
    _watchdog { [this] { watchdogLoop(); } }
{
}

Engine::~Engine()
{
    {
        auto const lock = std::scoped_lock { _mutex };
        _state = WatchdogState::Stopping;
    }
    _wakeup.notify_one();
    _watchdog.join();
    _hostRegistration.reset();
    closeNullDevice(_nullDevice);
    releaseRuntime();
}

bool Engine::expiresLater(Deadline const& a, Deadline const& b) noexcept
{
    return a.expiry > b.expiry;
}

void Engine::removeDeadline(WatchToken token)
{
    if (std::erase_if(_deadlines, [token](Deadline const& deadline) { return deadline.token == token; }) != 0)
        std::ranges::make_heap(_deadlines, expiresLater);
}

void Engine::armDeadline(WatchToken token, std::chrono::milliseconds timeout)
{
    Require(token.instance != nullptr);
    auto const expiry = std::chrono::steady_clock::now() + timeout;
    {
        auto const lock = std::scoped_lock { _mutex };
        removeDeadline(token);
        _deadlines.push_back(Deadline { .expiry = expiry, .token = token });
        std::ranges::push_heap(_deadlines, expiresLater);
    }
    _wakeup.notify_one();
}

void Engine::disarm(WatchToken token)
{
    auto const lock = std::scoped_lock { _mutex };
    removeDeadline(token);
    // No notification: a watchdog sleeping towards the dropped deadline wakes, finds nothing due, and
    // sleeps on.
}

void Engine::watchdogLoop()
{
    auto lock = std::unique_lock { _mutex };
    while (_state == WatchdogState::Running)
    {
        if (_deadlines.empty())
        {
            _wakeup.wait(lock);
            continue;
        }

        _wakeup.wait_until(lock, _deadlines.front().expiry);

        // Terminating under the lock is what makes disarm() final: once it has removed a deadline,
        // no terminate for it can still be on its way.
        auto const now = std::chrono::steady_clock::now();
        while (!_deadlines.empty() && _deadlines.front().expiry <= now)
        {
            std::ranges::pop_heap(_deadlines, expiresLater);
            wasm_runtime_terminate(_deadlines.back().token.instance);
            _deadlines.pop_back();
        }
    }
}

} // namespace vtextension
