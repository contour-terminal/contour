// SPDX-License-Identifier: Apache-2.0
//
// connectNamedPipe against a pipe server made here with the Win32 API.

#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/testing/CoroTestSupport.hpp>
#include <core/net/windows/NamedPipe.hpp>

#include <catch2/catch_test_macros.hpp>

// clang-format off
#include <winsock2.h>
#include <windows.h>
// clang-format on

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <tuple>

using core::net::NetErrorCode;

TEST_CASE("connectNamedPipe: a pipe nobody serves is refused")
{
    auto backend = core::net::makeBackend(core::net::preferredBackendKind());
    REQUIRE(backend != nullptr);
    auto loop = core::net::EventLoop { *backend };

    auto const socket = core::net::detail::connectNamedPipe(loop, LR"(\\.\pipe\core-cpp-no-such-pipe)");
    REQUIRE_FALSE(socket.has_value());
    CHECK(socket.error().code == NetErrorCode::ConnRefused);
}

TEST_CASE("connectNamedPipe: bytes written reach the server, and its reply and hang-up come back")
{
    auto backend = core::net::makeBackend(core::net::preferredBackendKind());
    REQUIRE(backend != nullptr);
    auto loop = core::net::EventLoop { *backend };

    auto const name =
        std::wstring { LR"(\\.\pipe\core-cpp-named-pipe-test-)" } + std::to_wstring(::GetCurrentProcessId());
    auto const server = ::CreateNamedPipeW(
        name.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_WAIT, 1, 256, 256, 0, nullptr);
    REQUIRE(server != INVALID_HANDLE_VALUE);

    auto serving = std::thread { [server] {
        std::ignore = ::ConnectNamedPipe(server, nullptr);
        auto request = std::array<char, 4> {};
        auto got = DWORD { 0 };
        std::ignore = ::ReadFile(server, request.data(), 4, &got, nullptr);
        auto wrote = DWORD { 0 };
        std::ignore = ::WriteFile(server, "pong", 4, &wrote, nullptr);
        std::ignore = ::FlushFileBuffers(server);
        ::DisconnectNamedPipe(server);
    } };

    auto socket = core::net::detail::connectNamedPipe(loop, name);
    REQUIRE(socket.has_value());
    auto const ping =
        std::array { std::byte { 'p' }, std::byte { 'i' }, std::byte { 'n' }, std::byte { 'g' } };
    auto reply = std::array<std::byte, 8> {};
    loop.blockOn([](core::net::ISocket* pipe,
                    std::array<std::byte, 4> const* request,
                    std::array<std::byte, 8>* buffer) -> core::async::Task<void> {
        auto const wrote = co_await pipe->write(*request);
        REQUIRE(wrote.has_value());
        auto const got = co_await pipe->read(*buffer);
        REQUIRE(got.has_value());
        CHECK(*got == 4);
        auto const eof = co_await pipe->read(*buffer);
        REQUIRE(eof.has_value());
        CHECK(*eof == 0);
        // ISocket::isClosed turns true once a read has seen the peer hang up.
        CHECK(pipe->isClosed());
    }(socket->get(), &ping, &reply));
    serving.join();
    ::CloseHandle(server);
}

namespace
{

using core::async::Task;
using core::net::EventLoop;
using core::net::IoResult;
using core::net::ISocket;

/// A one-instance pipe server on its own thread: accepts, reads four bytes, waits @p replyDelay,
/// answers `pong` if @p reply, then reads until the client hangs up.
struct PipeServer
{
    std::wstring name;
    HANDLE pipe = INVALID_HANDLE_VALUE;
    std::thread thread;
    std::atomic<bool> sawHangUp = false;

    PipeServer(std::wstring pipeName, std::chrono::milliseconds replyDelay, bool reply):
        name(std::move(pipeName)),
        pipe(::CreateNamedPipeW(
            name.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_WAIT, 1, 256, 256, 0, nullptr))
    {
        REQUIRE(pipe != INVALID_HANDLE_VALUE);
        thread = std::thread { [this, replyDelay, reply] {
            std::ignore = ::ConnectNamedPipe(pipe, nullptr);
            auto request = std::array<char, 4> {};
            auto got = DWORD { 0 };
            std::ignore = ::ReadFile(pipe, request.data(), 4, &got, nullptr);
            std::this_thread::sleep_for(replyDelay);
            if (reply)
            {
                auto wrote = DWORD { 0 };
                std::ignore = ::WriteFile(pipe, "pong", 4, &wrote, nullptr);
            }
            // The client's close shows here as a broken pipe.
            auto rest = std::array<char, 4> {};
            sawHangUp = ::ReadFile(pipe, rest.data(), 4, &got, nullptr) == FALSE
                        && ::GetLastError() == ERROR_BROKEN_PIPE;
        } };
    }

    PipeServer(PipeServer const&) = delete;
    PipeServer& operator=(PipeServer const&) = delete;
    PipeServer(PipeServer&&) = delete;
    PipeServer& operator=(PipeServer&&) = delete;

    ~PipeServer()
    {
        if (thread.joinable())
            thread.join();
        ::CloseHandle(pipe);
    }
};

std::wstring uniquePipeName(wchar_t const* tag)
{
    return std::wstring { LR"(\\.\pipe\core-cpp-named-pipe-)" } + tag + L"-"
           + std::to_wstring(::GetCurrentProcessId());
}

constexpr auto Ping =
    std::array { std::byte { 'p' }, std::byte { 'i' }, std::byte { 'n' }, std::byte { 'g' } };

/// Writes `ping`, then reads into @p out, recording the answer and whether @p ticked had been set.
Task<void> pingThenRead(ISocket* socket,
                        std::optional<IoResult>* out,
                        std::atomic<bool> const* ticked,
                        bool* tickedFirst)
{
    auto reply = std::array<std::byte, 8> {};
    auto const wrote = co_await socket->write(Ping);
    REQUIRE(wrote.has_value());
    *out = co_await socket->read(reply);
    *tickedFirst = ticked->load();
}

Task<void> setAfter(EventLoop* loop, std::chrono::milliseconds delay, std::atomic<bool>* flag)
{
    co_await loop->delay(delay);
    *flag = true;
}

/// After @p delay, runs @p action.
template <typename Action>
Task<void> doAfter(EventLoop* loop, std::chrono::milliseconds delay, Action action)
{
    co_await loop->delay(delay);
    action();
}

} // namespace

TEST_CASE("NamedPipe: a slow server parks the flow, not the loop")
{
    auto backend = core::net::makeBackend(core::net::preferredBackendKind());
    REQUIRE(backend != nullptr);
    auto loop = EventLoop { *backend };
    auto server = PipeServer { uniquePipeName(L"slow"), std::chrono::milliseconds { 400 }, true };
    auto socket = core::net::detail::connectNamedPipe(loop, server.name);
    REQUIRE(socket.has_value());

    auto answer = std::optional<IoResult> {};
    auto ticked = std::atomic<bool> { false };
    auto tickedFirst = false;
    loop.spawn(setAfter(&loop, std::chrono::milliseconds { 50 }, &ticked));
    loop.blockOn(pingThenRead(socket->get(), &answer, &ticked, &tickedFirst));

    REQUIRE(answer.has_value());
    REQUIRE(answer->has_value());
    CHECK(**answer == 4);
    CHECK(tickedFirst);
}

TEST_CASE("NamedPipe: cancelRead on a parked read answers Cancelled")
{
    auto backend = core::net::makeBackend(core::net::preferredBackendKind());
    REQUIRE(backend != nullptr);
    auto loop = EventLoop { *backend };
    auto server = PipeServer { uniquePipeName(L"cancel"), std::chrono::milliseconds { 600 }, false };
    auto socket = core::net::detail::connectNamedPipe(loop, server.name);
    REQUIRE(socket.has_value());

    auto answer = std::optional<IoResult> {};
    auto ticked = std::atomic<bool> { false };
    auto tickedFirst = false;
    auto* const raw = socket->get();
    loop.spawn(doAfter(&loop, std::chrono::milliseconds { 50 }, [raw] { raw->cancelRead(); }));
    loop.blockOn(pingThenRead(raw, &answer, &ticked, &tickedFirst));

    REQUIRE(answer.has_value());
    REQUIRE_FALSE(answer->has_value());
    CHECK(answer->error().code == NetErrorCode::Cancelled);
}

TEST_CASE("NamedPipe: a stopped flow leaves nothing behind")
{
    auto backend = core::net::makeBackend(core::net::preferredBackendKind());
    REQUIRE(backend != nullptr);
    auto loop = EventLoop { *backend };
    auto server = PipeServer { uniquePipeName(L"stop"), std::chrono::milliseconds { 300 }, false };
    auto socket = core::net::detail::connectNamedPipe(loop, server.name);
    REQUIRE(socket.has_value());

    auto answer = std::optional<IoResult> {};
    auto ticked = std::atomic<bool> { false };
    auto tickedFirst = false;
    // The timer wins and the read, the loser, is stopped while parked.
    loop.blockOn(
        core::net::testing::anyOf(pingThenRead(socket->get(), &answer, &ticked, &tickedFirst),
                                  core::net::testing::sleepFor(&loop, std::chrono::milliseconds { 50 })));
    CHECK_FALSE(answer.has_value());
    (*socket)->close();
}

TEST_CASE("NamedPipe: close on a parked read answers Cancelled and closes the pipe")
{
    auto backend = core::net::makeBackend(core::net::preferredBackendKind());
    REQUIRE(backend != nullptr);
    auto loop = EventLoop { *backend };
    auto server = PipeServer { uniquePipeName(L"close"), std::chrono::milliseconds { 0 }, false };
    auto socket = core::net::detail::connectNamedPipe(loop, server.name);
    REQUIRE(socket.has_value());

    auto answer = std::optional<IoResult> {};
    auto ticked = std::atomic<bool> { false };
    auto tickedFirst = false;
    auto* const raw = socket->get();
    loop.spawn(doAfter(&loop, std::chrono::milliseconds { 50 }, [raw] { raw->close(); }));
    loop.blockOn(pingThenRead(raw, &answer, &ticked, &tickedFirst));

    REQUIRE(answer.has_value());
    REQUIRE_FALSE(answer->has_value());
    CHECK(answer->error().code == NetErrorCode::Cancelled);
    CHECK(raw->isClosed());
    socket->reset();
    server.thread.join();
    CHECK(server.sawHangUp);
}

TEST_CASE("NamedPipe: the socket destroyed after close while a read is parked")
{
    auto backend = core::net::makeBackend(core::net::preferredBackendKind());
    REQUIRE(backend != nullptr);
    auto loop = EventLoop { *backend };
    auto server = PipeServer { uniquePipeName(L"destroy"), std::chrono::milliseconds { 0 }, false };
    auto socket = core::net::detail::connectNamedPipe(loop, server.name);
    REQUIRE(socket.has_value());

    auto answer = std::optional<IoResult> {};
    auto ticked = std::atomic<bool> { false };
    auto tickedFirst = false;
    auto* const owner = &*socket;
    auto* const raw = socket->get();
    loop.spawn(doAfter(&loop, std::chrono::milliseconds { 50 }, [owner] {
        (*owner)->close();
        owner->reset();
    }));
    loop.blockOn(pingThenRead(raw, &answer, &ticked, &tickedFirst));

    REQUIRE(answer.has_value());
    REQUIRE_FALSE(answer->has_value());
    CHECK(answer->error().code == NetErrorCode::Cancelled);
    server.thread.join();
    CHECK(server.sawHangUp);
}
