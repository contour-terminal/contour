// SPDX-License-Identifier: Apache-2.0
#include <vtextension/wasm/host/ExecutorThread.hpp>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <system_error>
#include <utility>

#ifdef _WIN32
    #include <Windows.h>
#else
    #include <climits>

    #include <pthread.h>
#endif

namespace vtextension::wasm::host
{

struct ExecutorThread::State
{
    std::function<void()> body;
#ifdef _WIN32
    HANDLE thread = nullptr;
#else
    pthread_t thread {};
#endif
};

namespace
{
#ifdef _WIN32
    DWORD WINAPI runBody(LPVOID state)
    {
        static_cast<ExecutorThread::State*>(state)->body();
        return 0;
    }
#else
    void* runBody(void* state)
    {
        static_cast<ExecutorThread::State*>(state)->body();
        return nullptr;
    }
#endif
} // namespace

ExecutorThread::ExecutorThread(std::size_t stackBytes, std::function<void()> body):
    _state { std::make_unique<State>(State { .body = std::move(body) }) }
{
#ifdef _WIN32
    // Reserved, not committed: the pages are touched only as deep as the calls go.
    _state->thread = ::CreateThread(
        nullptr, stackBytes, &runBody, _state.get(), STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!_state->thread)
        throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(), "CreateThread");
#else
    auto attributes = pthread_attr_t {};
    if (auto const failed = ::pthread_attr_init(&attributes); failed != 0)
        throw std::system_error(failed, std::generic_category(), "pthread_attr_init");
    auto const size = std::max(stackBytes, static_cast<std::size_t>(PTHREAD_STACK_MIN));
    auto failed = ::pthread_attr_setstacksize(&attributes, size);
    if (failed == 0)
        failed = ::pthread_create(&_state->thread, &attributes, &runBody, _state.get());
    ::pthread_attr_destroy(&attributes);
    if (failed != 0)
        throw std::system_error(failed, std::generic_category(), "pthread_create");
#endif
}

ExecutorThread::ExecutorThread(ExecutorThread&&) noexcept = default;

ExecutorThread& ExecutorThread::operator=(ExecutorThread&& other) noexcept
{
    if (this != &other)
    {
        join();
        _state = std::move(other._state);
    }
    return *this;
}

ExecutorThread::~ExecutorThread()
{
    join();
}

void ExecutorThread::join() noexcept
{
    if (!_state)
        return;
#ifdef _WIN32
    ::WaitForSingleObject(_state->thread, INFINITE);
    ::CloseHandle(_state->thread);
#else
    ::pthread_join(_state->thread, nullptr);
#endif
    _state.reset();
}

} // namespace vtextension::wasm::host
